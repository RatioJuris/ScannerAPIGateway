// =============================================================================
//  Scanner API Gateway by RatioJuris
//  (c) 2026 RatioJuris
//
//  A headless, windowless Windows x64 helper that exposes the machine's WIA 2.0
//  scanners through a small REST-style API bound to 127.0.0.1 only.
//
//  Build  : MSVC, C++17, x64 only (see .github/workflows/build.yml)
//  Runs   : in the background (GUI subsystem, no window, no console)
//
//  -----------------------------------------------------------------------------
//  API (all responses are JSON unless noted; auth = "Authorization: Bearer <token>"
//  or "X-Api-Key: <token>"; the token lives in gateway.ini)
//  -----------------------------------------------------------------------------
//    GET    /v1/health                       no auth. Liveness + version.
//    GET    /v1/devices                      List WIA devices.
//    POST   /v1/scan                         Start a scan (blocks until done).
//    GET    /v1/scans/{id}                   Metadata of a finished scan.
//    GET    /v1/scans/{id}/pages/{n}         Download page n (1-based) as image.
//    DELETE /v1/scans/{id}                   Delete a scan from the spool.
//
//  POST /v1/scan parameters (query string or x-www-form-urlencoded body):
//    device      auto | <id from /v1/devices>             default auto
//    mode        color | gray | bw                        default color
//    dpi         75..1200                                 default 300
//    source      auto | flatbed | adf | duplex            default auto
//    format      png | jpeg | bmp | tiff                  default png
//    area        left,top,width,height in millimetres     default full bed
//    brightness  -1000..1000 (driver specific)            optional
//    contrast    -1000..1000 (driver specific)            optional
//    threshold   0..255 (bw mode)                         optional
//    pages       0..N  (0 = everything the feeder holds)  default 0
//    timeout     5..600  idle seconds before abort        default 60
//    return      json | image (first page bytes inline)   default json
//
//  Extending: implement IScanBackend (e.g. TWAIN, eSCL/AirScan) and register it
//  in Server's constructor. Nothing else needs to change.
// =============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <wia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wiaguid.lib")
#pragma comment(lib, "uuid.lib")

static_assert(sizeof(void*) == 8, "Scanner API Gateway is a 64-bit only application.");

#ifndef WIA_IPA_FIRST
#define WIA_IPA_FIRST 2
#endif

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
//  Constants
// -----------------------------------------------------------------------------
static constexpr char kProductName[] = "Scanner API Gateway by RatioJuris";
static constexpr char kVendor[]      = "RatioJuris";
static constexpr char kCopyright[]   = "(c) 2026 RatioJuris";
static constexpr char kVersion[]     = "1.0.0";
static constexpr wchar_t kMutexName[]     = L"Local\\RatioJuris.ScannerAPIGateway";
static constexpr wchar_t kStopEventName[] = L"Local\\RatioJuris.ScannerAPIGateway.Stop";
static constexpr wchar_t kRunKeyPath[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t kRunValueName[]  = L"RatioJurisScannerAPIGateway";

static constexpr size_t kMaxHeaderBytes = 16 * 1024;
static constexpr size_t kMaxBodyBytes   = 64 * 1024;
static constexpr int    kMaxConnections = 32;

// WIA image format GUIDs (stable, published values).
static const GUID kFmtBmp  = { 0xb96b3cab, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtJpeg = { 0xb96b3cae, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtPng  = { 0xb96b3caf, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtTiff = { 0xb96b3cb1, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

// -----------------------------------------------------------------------------
//  Small utilities
// -----------------------------------------------------------------------------
static std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string ToLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

static std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

static std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

static std::string HrHex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

static std::string JsonEscape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}
static std::string JStr(std::string_view s) { return "\"" + JsonEscape(s) + "\""; }

static std::string RandomHex(size_t bytes) {
    std::vector<UCHAR> buf(bytes);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("System random generator failed.");
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (UCHAR b : buf) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

static bool ConstTimeEquals(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

static bool IsHexId(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// -----------------------------------------------------------------------------
//  Logging (file, size-rotated; optional console mirror for --console)
// -----------------------------------------------------------------------------
class Logger {
public:
    void Init(const fs::path& file, bool console) {
        std::lock_guard<std::mutex> lk(m_);
        path_ = file;
        console_ = console;
        std::error_code ec;
        if (fs::exists(path_, ec) && fs::file_size(path_, ec) > 2u * 1024u * 1024u)
            fs::rename(path_, fs::path(path_.wstring() + L".old"), ec);
    }
    void Write(const char* level, const std::string& msg) {
        SYSTEMTIME st; GetLocalTime(&st);
        char ts[40];
        snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::string clean = msg;
        for (char& c : clean) if (static_cast<unsigned char>(c) < 0x20) c = '?';   // no log injection
        std::string line = std::string(ts) + " [" + level + "] " + clean + "\n";
        std::lock_guard<std::mutex> lk(m_);
        if (!path_.empty()) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path_.c_str(), L"ab") == 0 && f) { fwrite(line.data(), 1, line.size(), f); fclose(f); }
        }
        if (console_) { fputs(line.c_str(), stdout); fflush(stdout); }
    }
private:
    std::mutex m_;
    fs::path path_;
    bool console_ = false;
};
static Logger g_log;
static void LogInfo(const std::string& m)  { g_log.Write("INFO", m); }
static void LogWarn(const std::string& m)  { g_log.Write("WARN", m); }
static void LogError(const std::string& m) { g_log.Write("ERROR", m); }

// -----------------------------------------------------------------------------
//  Errors
// -----------------------------------------------------------------------------
class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string code, const std::string& msg, HRESULT hr = S_OK)
        : std::runtime_error(msg), status(status), code(std::move(code)), hr(hr) {}
    int status;
    std::string code;
    HRESULT hr;
};

static ApiError MapWiaError(HRESULT hr, const std::string& context, int defStatus = 500, const char* defCode = "scan_failed") {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80210002UL: return ApiError(409, "paper_jam", "Paper jam detected.", hr);
        case 0x80210003UL: return ApiError(409, "paper_empty", "No paper in the document feeder.", hr);
        case 0x80210004UL: return ApiError(409, "paper_problem", "Paper problem reported by the scanner.", hr);
        case 0x80210005UL: return ApiError(503, "offline", "Scanner is offline or disconnected.", hr);
        case 0x80210006UL: return ApiError(409, "device_busy", "Scanner is busy.", hr);
        case 0x80210007UL: return ApiError(409, "warming_up", "Scanner is warming up.", hr);
        case 0x80210008UL: return ApiError(409, "user_intervention", "Scanner needs user intervention.", hr);
        case 0x8021000AUL: return ApiError(503, "device_communication", "Cannot communicate with the scanner.", hr);
        case 0x8021000DUL: return ApiError(409, "device_locked", "Scanner is locked by another application.", hr);
        case 0x80210016UL: return ApiError(409, "cover_open", "Scanner cover is open.", hr);
        case 0x80210017UL: return ApiError(409, "lamp_off", "Scanner lamp is off.", hr);
        case 0x80210020UL: return ApiError(409, "multi_feed", "Multiple sheets were fed at once.", hr);
        default:
            return ApiError(defStatus, defCode, context + " failed (" + HrHex(hr) + ").", hr);
    }
}

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
};

// -----------------------------------------------------------------------------
//  Configuration (gateway.ini in %LOCALAPPDATA%\RatioJuris\ScannerAPIGateway)
// -----------------------------------------------------------------------------
struct Config {
    int port = 18222;
    std::string token;
    std::vector<std::string> origins;   // lower-cased, no trailing slash
    bool anyOrigin = false;
    int retentionMin = 60;
    int maxPages = 200;
    fs::path dataDir;
    fs::path spoolDir;
    fs::path iniPath;
};

static fs::path DataDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path base = (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
    return base / L"RatioJuris" / L"ScannerAPIGateway";
}

static std::wstring ReadIniString(const fs::path& ini, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(4096);
    DWORD n = GetPrivateProfileStringW(L"gateway", key, def, buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    return std::wstring(buf.data(), n);
}

static Config LoadConfig() {
    Config c;
    c.dataDir = DataDirectory();
    std::error_code ec;
    fs::create_directories(c.dataDir, ec);
    c.iniPath = c.dataDir / L"gateway.ini";
    const bool fresh = !fs::exists(c.iniPath, ec);

    c.port = static_cast<int>(GetPrivateProfileIntW(L"gateway", L"port", 18222, c.iniPath.c_str()));
    if (c.port < 1024 || c.port > 65535) c.port = 18222;
    c.retentionMin = static_cast<int>(GetPrivateProfileIntW(L"gateway", L"retention_minutes", 60, c.iniPath.c_str()));
    c.retentionMin = std::clamp(c.retentionMin, 5, 10080);
    c.maxPages = static_cast<int>(GetPrivateProfileIntW(L"gateway", L"max_pages", 200, c.iniPath.c_str()));
    c.maxPages = std::clamp(c.maxPages, 1, 1000);

    c.token = Trim(WideToUtf8(ReadIniString(c.iniPath, L"token", L"")));
    if (c.token.size() < 24) {
        c.token = RandomHex(32);
        WritePrivateProfileStringW(L"gateway", L"token", Utf8ToWide(c.token).c_str(), c.iniPath.c_str());
    }

    std::string origins = WideToUtf8(ReadIniString(c.iniPath, L"allowed_origins", L""));
    for (auto& o : Split(origins, ',')) {
        std::string t = ToLower(Trim(o));
        while (!t.empty() && t.back() == '/') t.pop_back();
        if (t.empty()) continue;
        if (t == "*") c.anyOrigin = true; else c.origins.push_back(t);
    }

    std::wstring spool = ReadIniString(c.iniPath, L"spool_dir", L"");
    c.spoolDir = spool.empty() ? c.dataDir / L"spool" : fs::path(spool);
    fs::create_directories(c.spoolDir, ec);

    if (fresh) {
        WritePrivateProfileStringW(L"gateway", L"port", std::to_wstring(c.port).c_str(), c.iniPath.c_str());
        WritePrivateProfileStringW(L"gateway", L"allowed_origins", L"", c.iniPath.c_str());
        WritePrivateProfileStringW(L"gateway", L"retention_minutes", std::to_wstring(c.retentionMin).c_str(), c.iniPath.c_str());
        WritePrivateProfileStringW(L"gateway", L"max_pages", std::to_wstring(c.maxPages).c_str(), c.iniPath.c_str());
    }
    return c;
}

// -----------------------------------------------------------------------------
//  Scan backend abstraction
// -----------------------------------------------------------------------------
struct DeviceInfo {
    std::string id, name, description, manufacturer, kind, backend;
};

struct ScanParams {
    std::string deviceId = "auto";
    std::string mode = "color";       // color | gray | bw
    int dpi = 300;
    std::string source = "auto";      // auto | flatbed | adf | duplex
    std::string format = "png";       // png | jpeg | bmp | tiff
    bool hasArea = false;
    double area[4] = { 0, 0, 0, 0 };  // left, top, width, height (mm)
    std::optional<int> brightness, contrast, threshold;
    int pages = 0;                    // 0 = all
    int timeoutSec = 60;
    bool returnImage = false;
    int maxPagesCap = 200;            // from config
};

struct ScanResult {
    std::string device;
    std::vector<fs::path> pages;
    std::vector<std::string> warnings;
};

class IScanBackend {
public:
    virtual ~IScanBackend() = default;
    virtual std::string Name() const = 0;
    virtual std::vector<DeviceInfo> ListDevices() = 0;
    virtual ScanResult Scan(const ScanParams& params, const fs::path& outDir) = 0;
};

static std::wstring ExtensionFor(const std::string& fmt) {
    if (fmt == "jpeg") return L".jpg";
    if (fmt == "bmp")  return L".bmp";
    if (fmt == "tiff") return L".tif";
    return L".png";
}
static const char* ContentTypeForExt(const std::wstring& ext) {
    std::string e = ToLower(WideToUtf8(ext));
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".bmp") return "image/bmp";
    if (e == ".tif" || e == ".tiff") return "image/tiff";
    return "image/png";
}

// -----------------------------------------------------------------------------
//  WIA 2.0 backend
// -----------------------------------------------------------------------------
namespace wia {

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const std::wstring& w) { b = SysAllocStringLen(w.data(), static_cast<UINT>(w.size())); }
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

struct PropVar {
    PROPVARIANT v;
    PropVar() { PropVariantInit(&v); }
    ~PropVar() { PropVariantClear(&v); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
};

static HRESULT WriteProp(IWiaPropertyStorage* ps, PROPID id, PROPVARIANT& v) {
    PROPSPEC spec;
    spec.ulKind = PRSPEC_PROPID;
    spec.propid = id;
    return ps->WriteMultiple(1, &spec, &v, WIA_IPA_FIRST);
}
static HRESULT WriteLong(IWiaPropertyStorage* ps, PROPID id, LONG value) {
    PROPVARIANT v; PropVariantInit(&v);
    v.vt = VT_I4; v.lVal = value;
    return WriteProp(ps, id, v);
}
static HRESULT WriteGuid(IWiaPropertyStorage* ps, PROPID id, const GUID& g) {
    PROPVARIANT v; PropVariantInit(&v);
    v.vt = VT_CLSID; v.puuid = const_cast<GUID*>(&g);
    return WriteProp(ps, id, v);
}
static std::optional<std::wstring> ReadStr(IWiaPropertyStorage* ps, PROPID id) {
    PROPSPEC spec; spec.ulKind = PRSPEC_PROPID; spec.propid = id;
    PropVar pv;
    if (ps->ReadMultiple(1, &spec, &pv.v) == S_OK && pv.v.vt == VT_BSTR && pv.v.bstrVal)
        return std::wstring(pv.v.bstrVal, SysStringLen(pv.v.bstrVal));
    return std::nullopt;
}
static std::optional<LONG> ReadLong(IWiaPropertyStorage* ps, PROPID id) {
    PROPSPEC spec; spec.ulKind = PRSPEC_PROPID; spec.propid = id;
    PropVar pv;
    if (ps->ReadMultiple(1, &spec, &pv.v) == S_OK && pv.v.vt == VT_I4) return pv.v.lVal;
    return std::nullopt;
}

static ComPtr<IWiaDevMgr2> CreateManager() {
    ComPtr<IWiaDevMgr2> m;
    HRESULT hr = CoCreateInstance(CLSID_WiaDevMgr2, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(m.GetAddressOf()));
    if (FAILED(hr) || !m)
        throw ApiError(503, "wia_unavailable", "Windows Image Acquisition service is unavailable.", hr);
    return m;
}

static std::vector<DeviceInfo> EnumerateCore(IWiaDevMgr2* mgr) {
    std::vector<DeviceInfo> out;
    ComPtr<IEnumWIA_DEV_INFO> en;
    HRESULT hr = mgr->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, en.GetAddressOf());
    if (FAILED(hr)) throw MapWiaError(hr, "Enumerating devices");
    for (;;) {
        ComPtr<IWiaPropertyStorage> ps;
        ULONG got = 0;
        if (en->Next(1, ps.ReleaseAndGetAddressOf(), &got) != S_OK || got != 1) break;
        DeviceInfo d;
        d.id           = WideToUtf8(ReadStr(ps.Get(), WIA_DIP_DEV_ID).value_or(L""));
        d.name         = WideToUtf8(ReadStr(ps.Get(), WIA_DIP_DEV_NAME).value_or(L""));
        d.description  = WideToUtf8(ReadStr(ps.Get(), WIA_DIP_DEV_DESC).value_or(L""));
        d.manufacturer = WideToUtf8(ReadStr(ps.Get(), WIA_DIP_VEND_DESC).value_or(L""));
        LONG type = ReadLong(ps.Get(), WIA_DIP_DEV_TYPE).value_or(0);
        switch (type & 0xFFFF) {
            case 1:  d.kind = "scanner"; break;
            case 2:  d.kind = "camera";  break;
            case 3:  d.kind = "video";   break;
            default: d.kind = "other";   break;
        }
        d.backend = "wia";
        if (!d.id.empty()) out.push_back(std::move(d));
    }
    return out;
}

static std::wstring ItemName(IWiaItem2* item) {
    ComPtr<IWiaPropertyStorage> ps;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(ps.GetAddressOf())))) return L"";
    return ReadStr(ps.Get(), WIA_IPA_ITEM_NAME).value_or(L"");
}

static void CollectItems(IWiaItem2* parent, int depth, std::vector<ComPtr<IWiaItem2>>& out) {
    if (depth > 3) return;
    ComPtr<IEnumWiaItem2> en;
    if (FAILED(parent->EnumChildItems(nullptr, en.GetAddressOf())) || !en) return;
    for (;;) {
        ComPtr<IWiaItem2> child;
        ULONG got = 0;
        if (en->Next(1, child.GetAddressOf(), &got) != S_OK || got != 1) break;
        LONG type = 0;
        child->GetItemType(&type);
        if (type & WiaItemTypeFolder) CollectItems(child.Get(), depth + 1, out);
        if (!(type & (WiaItemTypeFolder | WiaItemTypeRoot)) && (type & (WiaItemTypeImage | WiaItemTypeTransfer)))
            out.push_back(child);
    }
}

// Receives page streams from WIA and writes them to the spool directory.
class ScanCallback : public IWiaTransferCallback {
public:
    ScanCallback(fs::path dir, std::wstring ext, int maxPages)
        : dir_(std::move(dir)), ext_(std::move(ext)), maxPages_(maxPages) {
        Touch();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IWiaTransferCallback))) {
            *ppv = static_cast<IWiaTransferCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP TransferCallback(LONG, WiaTransferParams* p) override {
        Touch();
        if (p && FAILED(p->hrErrorStatus)) {
            std::lock_guard<std::mutex> lk(m_);
            errorStatus_ = p->hrErrorStatus;
        }
        return S_OK;
    }

    STDMETHODIMP GetNextStream(LONG, BSTR, BSTR, IStream** ppDest) override {
        Touch();
        if (!ppDest) return E_POINTER;
        *ppDest = nullptr;
        std::lock_guard<std::mutex> lk(m_);
        if (static_cast<int>(pages_.size()) >= maxPages_) { capped_ = true; return E_ABORT; }
        wchar_t name[32];
        swprintf_s(name, L"page_%04zu", pages_.size() + 1);
        fs::path file = dir_ / (std::wstring(name) + ext_);
        HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                            FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, ppDest);
        if (SUCCEEDED(hr)) pages_.push_back(file);
        return hr;
    }

    std::vector<fs::path> Pages() { std::lock_guard<std::mutex> lk(m_); return pages_; }
    bool Capped() { std::lock_guard<std::mutex> lk(m_); return capped_; }
    HRESULT ErrorStatus() { std::lock_guard<std::mutex> lk(m_); return errorStatus_; }
    std::atomic<ULONGLONG> lastActivity{ 0 };

private:
    ~ScanCallback() = default;
    void Touch() { lastActivity = GetTickCount64(); }

    std::atomic<ULONG> ref_{ 1 };
    std::mutex m_;
    fs::path dir_;
    std::wstring ext_;
    int maxPages_;
    std::vector<fs::path> pages_;
    bool capped_ = false;
    HRESULT errorStatus_ = S_OK;
};

// Cancels the transfer if the device shows no activity for too long.
class Watchdog {
public:
    Watchdog(IWiaTransfer* t, std::atomic<ULONGLONG>& last, ULONGLONG idleMs)
        : transfer_(t), last_(last), idleMs_(idleMs), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        thread_ = std::thread([this] { Run(); });
    }
    ~Watchdog() { Stop(); if (stop_) CloseHandle(stop_); }
    void Stop() {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
    }
    bool TimedOut() const { return timedOut_; }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    void Run() {
        ComInit com;
        while (WaitForSingleObject(stop_, 200) == WAIT_TIMEOUT) {
            if (GetTickCount64() - last_.load() > idleMs_) {
                timedOut_ = true;
                transfer_->Cancel();
                break;
            }
        }
    }
    IWiaTransfer* transfer_;
    std::atomic<ULONGLONG>& last_;
    ULONGLONG idleMs_;
    HANDLE stop_;
    std::atomic<bool> timedOut_{ false };
    std::thread thread_;
};

class WiaBackend : public IScanBackend {
public:
    std::string Name() const override { return "wia"; }

    std::vector<DeviceInfo> ListDevices() override {
        ComInit com;
        if (FAILED(com.hr)) throw ApiError(500, "com_init_failed", "Could not initialise COM.", com.hr);
        auto mgr = CreateManager();
        return EnumerateCore(mgr.Get());
    }

    ScanResult Scan(const ScanParams& p, const fs::path& outDir) override {
        ComInit com;
        if (FAILED(com.hr)) throw ApiError(500, "com_init_failed", "Could not initialise COM.", com.hr);
        ScanResult result;
        auto mgr = CreateManager();

        // ---- resolve device ----
        std::string devId = p.deviceId;
        if (devId == "auto") {
            auto devs = EnumerateCore(mgr.Get());
            devId.clear();
            for (auto& d : devs) if (d.kind == "scanner") { devId = d.id; break; }
            if (devId.empty()) throw ApiError(404, "no_scanner", "No WIA scanner was found.");
        }
        result.device = devId;

        Bstr bId(Utf8ToWide(devId));
        ComPtr<IWiaItem2> root;
        HRESULT hr = mgr->CreateDevice(0, bId.b, root.GetAddressOf());
        if (FAILED(hr) || !root) throw MapWiaError(hr, "Opening the scanner", 404, "device_not_found");

        // ---- choose scan item (flatbed / feeder) ----
        std::vector<ComPtr<IWiaItem2>> items;
        CollectItems(root.Get(), 0, items);
        if (items.empty()) throw ApiError(502, "no_scan_item", "The device exposes no scannable items.");

        const bool wantFeeder = (p.source == "adf" || p.source == "duplex");
        ComPtr<IWiaItem2> item;
        if (p.source != "auto") {
            const std::string want = wantFeeder ? "feeder" : "flatbed";
            for (auto& it : items)
                if (ToLower(WideToUtf8(ItemName(it.Get()))).find(want) != std::string::npos) { item = it; break; }
        } else {
            for (auto& it : items)
                if (ToLower(WideToUtf8(ItemName(it.Get()))).find("flatbed") != std::string::npos) { item = it; break; }
        }
        const bool nameMatched = static_cast<bool>(item);
        if (!item) {
            if (p.source == "auto" || items.size() == 1) item = items.front();
            else throw ApiError(422, "source_not_available", "The requested source is not available on this device.");
        }

        ComPtr<IWiaPropertyStorage> ps;
        if (FAILED(item.As(&ps)) || !ps) throw ApiError(502, "no_properties", "Cannot access item properties.");

        auto hard = [&](PROPID id, LONG v, const char* name) {
            HRESULT h = WriteLong(ps.Get(), id, v);
            if (FAILED(h)) throw ApiError(422, "unsupported_setting", std::string("The device rejected setting '") + name + "'.", h);
        };
        auto soft = [&](PROPID id, LONG v, const char* name) {
            HRESULT h = WriteLong(ps.Get(), id, v);
            if (FAILED(h)) result.warnings.push_back(std::string("Setting '") + name + "' was not applied by the device (" + HrHex(h) + ").");
        };

        // ---- paper source ----
        if (p.source != "auto") {
            const LONG handling = (p.source == "flatbed") ? 0x2 : (p.source == "adf") ? 0x1 : 0x5;  // FLATBED / FEEDER / FEEDER|DUPLEX
            HRESULT h = WriteLong(ps.Get(), WIA_IPS_DOCUMENT_HANDLING_SELECT, handling);
            if (FAILED(h)) {
                ComPtr<IWiaPropertyStorage> rps;
                if (SUCCEEDED(root.As(&rps)) && rps) h = WriteLong(rps.Get(), WIA_IPS_DOCUMENT_HANDLING_SELECT, handling);
            }
            if (FAILED(h)) {
                if (p.source == "duplex") throw ApiError(422, "duplex_not_supported", "Duplex scanning is not supported by this device.", h);
                if (p.source == "adf" && !nameMatched) throw ApiError(422, "source_not_available", "No document feeder available.", h);
                result.warnings.push_back("Could not select paper source explicitly (" + HrHex(h) + ").");
            }
            if (wantFeeder) soft(WIA_IPS_PAGES, p.pages, "pages");
        }

        // ---- image settings (order matters: format/resolution before area) ----
        const GUID* fmt = &kFmtPng;
        if (p.format == "jpeg") fmt = &kFmtJpeg;
        else if (p.format == "bmp") fmt = &kFmtBmp;
        else if (p.format == "tiff") fmt = &kFmtTiff;
        hr = WriteGuid(ps.Get(), WIA_IPA_FORMAT, *fmt);
        if (FAILED(hr)) throw ApiError(422, "unsupported_format", "This device/driver cannot output '" + p.format + "'. Try bmp or tiff.", hr);
        soft(WIA_IPA_TYMED, TYMED_FILE, "transfer medium");

        hard(WIA_IPS_XRES, p.dpi, "resolution");
        hard(WIA_IPS_YRES, p.dpi, "resolution");

        LONG dataType = WIA_DATA_COLOR, depth = 24;
        if (p.mode == "gray") { dataType = WIA_DATA_GRAYSCALE; depth = 8; }
        else if (p.mode == "bw") { dataType = WIA_DATA_THRESHOLD; depth = 1; }
        hard(WIA_IPA_DATATYPE, dataType, "data type");
        soft(WIA_IPA_DEPTH, depth, "bit depth");

        if (p.hasArea) {
            auto px = [&](double mm) { return static_cast<LONG>(std::lround(mm / 25.4 * p.dpi)); };
            soft(WIA_IPS_XPOS, 0, "area reset");
            soft(WIA_IPS_YPOS, 0, "area reset");
            hard(WIA_IPS_XEXTENT, std::max<LONG>(1, px(p.area[2])), "area width");
            hard(WIA_IPS_YEXTENT, std::max<LONG>(1, px(p.area[3])), "area height");
            hard(WIA_IPS_XPOS, px(p.area[0]), "area left");
            hard(WIA_IPS_YPOS, px(p.area[1]), "area top");
        }
        if (p.brightness) soft(WIA_IPS_BRIGHTNESS, *p.brightness, "brightness");
        if (p.contrast)   soft(WIA_IPS_CONTRAST, *p.contrast, "contrast");
        if (p.threshold)  soft(WIA_IPS_THRESHOLD, *p.threshold, "threshold");

        // ---- transfer ----
        ComPtr<IWiaTransfer> xfer;
        if (FAILED(item.As(&xfer)) || !xfer) throw ApiError(502, "transfer_unsupported", "The device does not support WIA 2.0 transfers.");
        const int cap = std::min(p.pages > 0 ? p.pages : p.maxPagesCap, p.maxPagesCap);
        ComPtr<ScanCallback> cb;
        cb.Attach(new ScanCallback(outDir, ExtensionFor(p.format), cap));

        Watchdog wd(xfer.Get(), cb->lastActivity, static_cast<ULONGLONG>(p.timeoutSec) * 1000ULL);
        hr = xfer->Download(0, cb.Get());
        wd.Stop();

        auto pages = cb->Pages();
        if (wd.TimedOut())
            throw ApiError(504, "timeout", "The scanner showed no activity for " + std::to_string(p.timeoutSec) + " seconds; scan aborted.", hr);

        const bool capped = cb->Capped();
        if (!capped && hr != S_OK && hr != S_FALSE) {
            const bool feederEmptyAfterPages = (static_cast<unsigned long>(hr) == 0x80210003UL) && !pages.empty();
            if (!feederEmptyAfterPages) throw MapWiaError(hr, "Scanning");
        } else if (hr == S_FALSE && !pages.empty()) {
            result.warnings.push_back("The transfer ended early.");
        }
        if (pages.empty()) {
            HRESULT e = cb->ErrorStatus();
            if (FAILED(e)) throw MapWiaError(e, "Scanning");
            throw ApiError(502, "no_pages", "The scanner returned no image data.", hr);
        }
        if (capped) result.warnings.push_back("Stopped at the configured maximum page count.");
        result.pages = std::move(pages);
        return result;
    }
};

}  // namespace wia

// -----------------------------------------------------------------------------
//  HTTP primitives
// -----------------------------------------------------------------------------
struct HttpRequest {
    std::string method, path, rawQuery;
    std::map<std::string, std::string> query;     // keys lower-cased
    std::map<std::string, std::string> headers;   // keys lower-cased
    std::string body;
    const std::string& Header(const std::string& k) const {
        static const std::string empty;
        auto it = headers.find(k);
        return it == headers.end() ? empty : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    fs::path bodyFile;
    bool useFile = false;
    std::vector<std::pair<std::string, std::string>> headers;
};

static const char* ReasonPhrase(int s) {
    switch (s) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Internal Server Error";
    }
}

static std::string UrlDecode(std::string_view s, bool plusAsSpace) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            auto hv = [](char h) { return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10; };
            o += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else if (c == '+' && plusAsSpace) {
            o += ' ';
        } else {
            o += c;
        }
    }
    return o;
}

static std::map<std::string, std::string> ParseQuery(std::string_view q) {
    std::map<std::string, std::string> m;
    size_t start = 0;
    while (start <= q.size()) {
        size_t amp = q.find('&', start);
        std::string_view pair = q.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string k = ToLower(UrlDecode(pair.substr(0, eq), true));
            std::string v = eq == std::string_view::npos ? "" : UrlDecode(pair.substr(eq + 1), true);
            m.emplace(std::move(k), std::move(v));   // first value wins
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return m;
}

static bool SendAll(SOCKET s, const char* data, size_t len) {
    while (len > 0) {
        int n = send(s, data, static_cast<int>(std::min<size_t>(len, 1u << 20)), 0);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Returns false on failure; err==0 means "drop connection silently".
static bool ReadRequest(SOCKET s, HttpRequest& req, int& err) {
    err = 0;
    std::string buf;
    char tmp[4096];
    size_t headerEnd;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHeaderBytes) { err = 431; return false; }
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    std::string head = buf.substr(0, headerEnd);
    std::string rest = buf.substr(headerEnd + 4);

    auto lines = Split(head, '\n');
    if (lines.empty()) { err = 400; return false; }
    std::string reqLine = Trim(lines[0]);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = reqLine.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) { err = 400; return false; }
    req.method = reqLine.substr(0, sp1);
    std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version = reqLine.substr(sp2 + 1);
    if (version.rfind("HTTP/1.", 0) != 0 || target.empty() || target[0] != '/') { err = 400; return false; }
    for (char& c : req.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    size_t hash = target.find('#');
    if (hash != std::string::npos) target.resize(hash);
    size_t qm = target.find('?');
    req.path = UrlDecode(target.substr(0, qm), false);
    if (qm != std::string::npos) {
        req.rawQuery = target.substr(qm + 1);
        req.query = ParseQuery(req.rawQuery);
    }

    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) { err = 400; return false; }
        req.headers[ToLower(Trim(line.substr(0, c)))] = Trim(line.substr(c + 1));
    }

    if (!req.Header("transfer-encoding").empty()) { err = 501; return false; }
    size_t contentLength = 0;
    const std::string& cl = req.Header("content-length");
    if (!cl.empty()) {
        unsigned long long v = 0;
        auto r = std::from_chars(cl.data(), cl.data() + cl.size(), v);
        if (r.ec != std::errc() || r.ptr != cl.data() + cl.size()) { err = 400; return false; }
        if (v > kMaxBodyBytes) { err = 413; return false; }
        contentLength = static_cast<size_t>(v);
    }
    req.body = rest;
    while (req.body.size() < contentLength) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        req.body.append(tmp, static_cast<size_t>(n));
    }
    if (req.body.size() > contentLength) req.body.resize(contentLength);
    return true;
}

static bool WriteResponse(SOCKET s, const HttpResponse& r) {
    uintmax_t length = r.body.size();
    std::error_code ec;
    if (r.useFile) {
        length = fs::file_size(r.bodyFile, ec);
        if (ec) length = 0;
    }
    std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + ReasonPhrase(r.status) + "\r\n";
    if (r.status != 204) h += "Content-Type: " + r.contentType + "\r\n";
    h += "Content-Length: " + std::to_string(r.status == 204 ? 0 : length) + "\r\n";
    h += "Connection: close\r\n";
    h += "Cache-Control: no-store\r\n";
    h += "X-Content-Type-Options: nosniff\r\n";
    h += std::string("Server: ScannerAPIGateway/") + kVersion + "\r\n";
    for (auto& kv : r.headers) h += kv.first + ": " + kv.second + "\r\n";
    h += "\r\n";
    if (!SendAll(s, h.data(), h.size())) return false;
    if (r.status == 204) return true;
    if (!r.useFile) return r.body.empty() || SendAll(s, r.body.data(), r.body.size());

    std::ifstream in(r.bodyFile, std::ios::binary);
    std::vector<char> chunk(64 * 1024);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        std::streamsize got = in.gcount();
        if (got > 0 && !SendAll(s, chunk.data(), static_cast<size_t>(got))) return false;
    }
    return true;
}

static HttpResponse JsonResponse(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

static HttpResponse ErrorResponse(int status, const std::string& code, const std::string& message, HRESULT hr = S_OK) {
    std::string b = "{\"error\":{\"code\":" + JStr(code) + ",\"message\":" + JStr(message);
    if (FAILED(hr)) b += ",\"hresult\":" + JStr(HrHex(hr));
    b += "}}";
    return JsonResponse(status, std::move(b));
}

// -----------------------------------------------------------------------------
//  Parameter parsing
// -----------------------------------------------------------------------------
static int ParseIntParam(const std::string& name, const std::string& v, int lo, int hi) {
    int x = 0;
    auto r = std::from_chars(v.data(), v.data() + v.size(), x);
    if (v.empty() || r.ec != std::errc() || r.ptr != v.data() + v.size() || x < lo || x > hi)
        throw ApiError(400, "invalid_parameter", "'" + name + "' must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
    return x;
}

static double ParseDoubleParam(const std::string& name, const std::string& v, double lo, double hi) {
    double x = 0;
    std::string t = Trim(v);
    auto r = std::from_chars(t.data(), t.data() + t.size(), x);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size() || !(x >= lo && x <= hi))
        throw ApiError(400, "invalid_parameter", "'" + name + "' contains an invalid number.");
    return x;
}

static ScanParams ParseScanParams(const std::map<std::string, std::string>& q, int maxPagesCap, std::vector<std::string>& warnings) {
    static const std::set<std::string> known = { "device", "mode", "dpi", "source", "format", "area",
                                                 "brightness", "contrast", "threshold", "timeout", "pages", "return" };
    for (auto& kv : q)
        if (!known.count(kv.first)) warnings.push_back("Ignored unknown parameter '" + kv.first + "'.");

    auto get = [&](const char* k) -> const std::string* {
        auto it = q.find(k);
        return (it == q.end() || it->second.empty()) ? nullptr : &it->second;
    };
    ScanParams p;
    p.maxPagesCap = maxPagesCap;

    if (auto v = get("device")) {
        std::string d = Trim(*v);
        if (ToLower(d).rfind("wia://", 0) == 0) d = d.substr(6);
        if (d.size() > 256) throw ApiError(400, "invalid_parameter", "'device' is too long.");
        for (unsigned char c : d) if (c < 0x20) throw ApiError(400, "invalid_parameter", "'device' contains control characters.");
        p.deviceId = ToLower(d) == "auto" ? "auto" : d;
    }
    if (auto v = get("mode")) {
        std::string m = ToLower(*v);
        if (m == "grey") m = "gray";
        if (m != "color" && m != "gray" && m != "bw") throw ApiError(400, "invalid_parameter", "'mode' must be color, gray or bw.");
        p.mode = m;
    }
    if (auto v = get("dpi")) p.dpi = ParseIntParam("dpi", *v, 75, 1200);
    if (auto v = get("source")) {
        std::string s = ToLower(*v);
        if (s != "auto" && s != "flatbed" && s != "adf" && s != "duplex") throw ApiError(400, "invalid_parameter", "'source' must be auto, flatbed, adf or duplex.");
        p.source = s;
    }
    if (auto v = get("format")) {
        std::string f = ToLower(*v);
        if (f == "jpg") f = "jpeg";
        if (f == "tif") f = "tiff";
        if (f != "png" && f != "jpeg" && f != "bmp" && f != "tiff") throw ApiError(400, "invalid_parameter", "'format' must be png, jpeg, bmp or tiff.");
        p.format = f;
    }
    if (auto v = get("area")) {
        auto parts = Split(*v, ',');
        if (parts.size() != 4) throw ApiError(400, "invalid_parameter", "'area' must be left,top,width,height in millimetres.");
        for (int i = 0; i < 4; ++i) p.area[i] = ParseDoubleParam("area", parts[static_cast<size_t>(i)], 0.0, 2000.0);
        if (p.area[2] <= 0 || p.area[3] <= 0) throw ApiError(400, "invalid_parameter", "'area' width and height must be greater than zero.");
        p.hasArea = true;
    }
    if (auto v = get("brightness")) p.brightness = ParseIntParam("brightness", *v, -1000, 1000);
    if (auto v = get("contrast"))   p.contrast   = ParseIntParam("contrast", *v, -1000, 1000);
    if (auto v = get("threshold"))  p.threshold  = ParseIntParam("threshold", *v, 0, 255);
    if (auto v = get("pages"))      p.pages      = ParseIntParam("pages", *v, 0, 1000);
    if (auto v = get("timeout"))    p.timeoutSec = ParseIntParam("timeout", *v, 5, 600);
    if (auto v = get("return")) {
        std::string r = ToLower(*v);
        if (r != "json" && r != "image") throw ApiError(400, "invalid_parameter", "'return' must be json or image.");
        p.returnImage = (r == "image");
    }
    return p;
}

// -----------------------------------------------------------------------------
//  Spool helpers
// -----------------------------------------------------------------------------
static std::vector<fs::path> ListPages(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && it->path().filename().wstring().rfind(L"page_", 0) == 0) v.push_back(it->path());
    }
    std::sort(v.begin(), v.end());
    return v;
}

static std::string BuildScanJson(const std::string& id, const fs::path& dir, const std::string& extra) {
    auto pages = ListPages(dir);
    std::ostringstream o;
    o << "{\"scan_id\":" << JStr(id) << ",\"page_count\":" << pages.size() << ",\"pages\":[";
    for (size_t i = 0; i < pages.size(); ++i) {
        std::error_code ec;
        uintmax_t sz = fs::file_size(pages[i], ec);
        if (i) o << ',';
        o << "{\"index\":" << (i + 1)
          << ",\"content_type\":" << JStr(ContentTypeForExt(pages[i].extension().wstring()))
          << ",\"bytes\":" << (ec ? 0 : sz)
          << ",\"url\":" << JStr("/v1/scans/" + id + "/pages/" + std::to_string(i + 1)) << "}";
    }
    o << "]" << extra << "}";
    return o.str();
}

// -----------------------------------------------------------------------------
//  Server
// -----------------------------------------------------------------------------
class Server {
public:
    explicit Server(Config cfg) : cfg_(std::move(cfg)) {
        backend_ = std::make_unique<wia::WiaBackend>();   // register further backends here
    }

    int Run(HANDLE stopEvent) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LogError("WSAStartup failed."); return 3; }

        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) { LogError("socket() failed."); WSACleanup(); return 3; }
        BOOL excl = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // loopback only, by design
        addr.sin_port = htons(static_cast<u_short>(cfg_.port));
        if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR || listen(ls, 16) == SOCKET_ERROR) {
            LogError("Cannot listen on 127.0.0.1:" + std::to_string(cfg_.port) + " (WSA error " + std::to_string(WSAGetLastError()) + "). Is another instance or program using the port?");
            closesocket(ls);
            WSACleanup();
            return 2;
        }
        LogInfo(std::string(kProductName) + " " + kVersion + " listening on http://127.0.0.1:" + std::to_string(cfg_.port));
        PurgeExpired();

        auto lastTick = std::chrono::steady_clock::now();
        while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            timeval tv{ 0, 500000 };
            int r = select(0, &rf, nullptr, nullptr, &tv);
            if (r > 0) {
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                if (active_ >= kMaxConnections) {
                    HttpResponse busy = ErrorResponse(503, "too_many_connections", "Too many concurrent connections.");
                    WriteResponse(c, busy);
                    closesocket(c);
                    continue;
                }
                ++active_;
                try {
                    std::thread([this, c] { Serve(c); --active_; }).detach();
                } catch (...) {
                    --active_;
                    closesocket(c);
                }
            }
            if (std::chrono::steady_clock::now() - lastTick > std::chrono::minutes(5)) {
                lastTick = std::chrono::steady_clock::now();
                PurgeExpired();
            }
        }

        LogInfo("Shutdown requested.");
        closesocket(ls);
        for (int i = 0; i < 50 && active_ > 0; ++i) Sleep(100);   // up to 5 s for in-flight requests
        WSACleanup();
        LogInfo("Stopped.");
        return 0;
    }

private:
    // ---- connection ----
    void Serve(SOCKET s) {
        try {
            DWORD rcv = 10000, snd = 60000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof rcv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof snd);

            auto t0 = std::chrono::steady_clock::now();
            HttpRequest req;
            HttpResponse res;
            int err = 0;
            if (!ReadRequest(s, req, err)) {
                if (err == 0) { closesocket(s); return; }
                res = ErrorResponse(err, err == 413 ? "payload_too_large" : err == 431 ? "headers_too_large" : err == 501 ? "not_implemented" : "bad_request", "The request could not be processed.");
            } else {
                res = Handle(req);
            }
            WriteResponse(s, res);
            shutdown(s, SD_SEND);
            closesocket(s);

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            LogInfo(req.method + " " + req.path + " -> " + std::to_string(res.status) + " (" + std::to_string(ms) + " ms)");
        } catch (const std::exception& e) {
            LogError(std::string("Connection handler failed: ") + e.what());
            closesocket(s);
        } catch (...) {
            LogError("Connection handler failed with an unknown exception.");
            closesocket(s);
        }
    }

    // ---- security ----
    bool HostAllowed(const std::string& hostHeader) const {
        std::string h = ToLower(hostHeader);
        std::string p = ":" + std::to_string(cfg_.port);
        return h == "127.0.0.1" + p || h == "localhost" + p || h == "[::1]" + p;   // blocks DNS-rebinding
    }
    bool OriginAllowed(const std::string& origin) const {
        if (cfg_.anyOrigin) return true;
        std::string o = ToLower(origin);
        while (!o.empty() && o.back() == '/') o.pop_back();
        return std::find(cfg_.origins.begin(), cfg_.origins.end(), o) != cfg_.origins.end();
    }
    void Authorize(const HttpRequest& req) const {
        std::string given;
        const std::string& a = req.Header("authorization");
        if (a.size() > 7 && ToLower(a.substr(0, 7)) == "bearer ") given = Trim(a.substr(7));
        else given = req.Header("x-api-key");
        if (!ConstTimeEquals(given, cfg_.token))
            throw ApiError(401, "unauthorized", "Missing or invalid API token.");
    }
    static void ApplyCors(HttpResponse& r, const std::string& origin) {
        r.headers.emplace_back("Access-Control-Allow-Origin", origin);
        r.headers.emplace_back("Vary", "Origin");
        r.headers.emplace_back("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        r.headers.emplace_back("Access-Control-Allow-Headers", "Authorization, X-Api-Key, Content-Type");
        r.headers.emplace_back("Access-Control-Expose-Headers", "X-Scan-Id, X-Page-Count, Content-Disposition");
        r.headers.emplace_back("Access-Control-Allow-Private-Network", "true");
        r.headers.emplace_back("Access-Control-Max-Age", "600");
    }

    // ---- request handling ----
    HttpResponse Handle(const HttpRequest& req) {
        HttpResponse res;
        const std::string origin = req.Header("origin");
        bool cors = false;
        try {
            if (!HostAllowed(req.Header("host"))) throw ApiError(421, "invalid_host", "Unexpected Host header.");
            if (!origin.empty()) {
                if (!OriginAllowed(origin)) throw ApiError(403, "forbidden_origin", "This origin is not allowed. Add it to allowed_origins in gateway.ini.");
                cors = true;
            }
            if (req.method == "OPTIONS") res.status = 204;
            else res = Route(req);
        } catch (const ApiError& e) {
            res = ErrorResponse(e.status, e.code, e.what(), e.hr);
        } catch (const std::exception& e) {
            LogError(std::string("Unhandled error: ") + e.what());
            res = ErrorResponse(500, "internal_error", "Unexpected internal error.");
        }
        if (cors) ApplyCors(res, origin);
        return res;
    }

    HttpResponse Route(const HttpRequest& req) {
        const std::string& p = req.path;

        if (p == "/" || p == "/v1/health") {
            if (req.method != "GET") throw ApiError(405, "method_not_allowed", "Use GET.");
            return JsonResponse(200, std::string("{\"name\":") + JStr(kProductName) + ",\"vendor\":" + JStr(kVendor) +
                                     ",\"copyright\":" + JStr(kCopyright) + ",\"version\":" + JStr(kVersion) +
                                     ",\"api\":\"v1\",\"backend\":" + JStr(backend_->Name()) +
                                     ",\"status\":" + (scanning_ ? "\"scanning\"" : "\"ready\"") + ",\"auth\":\"token\"}");
        }

        Authorize(req);

        if (p == "/v1/devices") {
            if (req.method != "GET") throw ApiError(405, "method_not_allowed", "Use GET.");
            auto devs = backend_->ListDevices();
            std::string b = "{\"devices\":[";
            for (size_t i = 0; i < devs.size(); ++i) {
                const auto& d = devs[i];
                if (i) b += ',';
                b += "{\"id\":" + JStr(d.id) + ",\"name\":" + JStr(d.name) + ",\"description\":" + JStr(d.description) +
                     ",\"manufacturer\":" + JStr(d.manufacturer) + ",\"kind\":" + JStr(d.kind) + ",\"backend\":" + JStr(d.backend) + "}";
            }
            b += "]}";
            return JsonResponse(200, std::move(b));
        }

        if (p == "/v1/scan") return HandleScan(req);

        const std::string prefix = "/v1/scans/";
        if (p.rfind(prefix, 0) == 0) {
            auto parts = Split(p.substr(prefix.size()), '/');
            if (parts.empty() || !IsHexId(parts[0])) throw ApiError(404, "not_found", "Unknown scan.");
            const std::string& id = parts[0];
            fs::path dir = cfg_.spoolDir / Utf8ToWide(id);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) throw ApiError(404, "scan_not_found", "Scan not found or expired.");

            if (parts.size() == 1) {
                if (req.method == "GET") return JsonResponse(200, BuildScanJson(id, dir, ""));
                if (req.method == "DELETE") {
                    fs::remove_all(dir, ec);
                    HttpResponse r; r.status = 204; return r;
                }
                throw ApiError(405, "method_not_allowed", "Use GET or DELETE.");
            }
            if (parts.size() == 3 && parts[1] == "pages") {
                if (req.method != "GET") throw ApiError(405, "method_not_allowed", "Use GET.");
                auto pages = ListPages(dir);
                int n = 0;
                auto r = std::from_chars(parts[2].data(), parts[2].data() + parts[2].size(), n);
                if (r.ec != std::errc() || r.ptr != parts[2].data() + parts[2].size() || n < 1 || static_cast<size_t>(n) > pages.size())
                    throw ApiError(404, "page_not_found", "Page not found.");
                const fs::path& file = pages[static_cast<size_t>(n - 1)];
                HttpResponse out;
                out.useFile = true;
                out.bodyFile = file;
                out.contentType = ContentTypeForExt(file.extension().wstring());
                out.headers.emplace_back("Content-Disposition", "inline; filename=\"scan_" + id.substr(0, 8) + "_p" + std::to_string(n) + WideToUtf8(file.extension().wstring()) + "\"");
                out.headers.emplace_back("X-Scan-Id", id);
                out.headers.emplace_back("X-Page-Count", std::to_string(pages.size()));
                return out;
            }
        }
        throw ApiError(404, "not_found", "Unknown endpoint.");
    }

    HttpResponse HandleScan(const HttpRequest& req) {
        if (req.method != "POST") throw ApiError(405, "method_not_allowed", "Use POST.");

        std::map<std::string, std::string> q = req.query;
        if (!req.body.empty() && ToLower(req.Header("content-type")).find("application/x-www-form-urlencoded") != std::string::npos)
            for (auto& kv : ParseQuery(req.body)) q.emplace(kv.first, kv.second);

        std::vector<std::string> warnings;
        ScanParams p = ParseScanParams(q, cfg_.maxPages, warnings);

        std::unique_lock<std::mutex> lk(scanMutex_, std::try_to_lock);
        if (!lk.owns_lock()) throw ApiError(409, "busy", "Another scan is already in progress.");
        PurgeExpired();

        struct ScanFlag {
            std::atomic<bool>& f;
            explicit ScanFlag(std::atomic<bool>& x) : f(x) { f = true; }
            ~ScanFlag() { f = false; }
        } flag(scanning_);

        const std::string id = RandomHex(16);
        const fs::path dir = cfg_.spoolDir / Utf8ToWide(id);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) throw ApiError(500, "spool_error", "Cannot create the spool directory.");

        LogInfo("Scan " + id.substr(0, 8) + " started: mode=" + p.mode + " dpi=" + std::to_string(p.dpi) + " source=" + p.source + " format=" + p.format);
        ScanResult r;
        try {
            r = backend_->Scan(p, dir);
        } catch (...) {
            fs::remove_all(dir, ec);
            throw;
        }
        r.warnings.insert(r.warnings.begin(), warnings.begin(), warnings.end());

        std::string extra = ",\"device\":" + JStr(r.device) + ",\"mode\":" + JStr(p.mode) + ",\"dpi\":" + std::to_string(p.dpi) +
                            ",\"source\":" + JStr(p.source) + ",\"format\":" + JStr(p.format) +
                            ",\"expires_in_minutes\":" + std::to_string(cfg_.retentionMin) + ",\"warnings\":[";
        for (size_t i = 0; i < r.warnings.size(); ++i) { if (i) extra += ','; extra += JStr(r.warnings[i]); }
        extra += "]";

        LogInfo("Scan " + id.substr(0, 8) + " finished: " + std::to_string(r.pages.size()) + " page(s).");

        if (p.returnImage) {
            HttpResponse out;
            out.useFile = true;
            out.bodyFile = r.pages.front();
            out.contentType = ContentTypeForExt(r.pages.front().extension().wstring());
            out.headers.emplace_back("X-Scan-Id", id);
            out.headers.emplace_back("X-Page-Count", std::to_string(r.pages.size()));
            return out;
        }
        HttpResponse out = JsonResponse(201, BuildScanJson(id, dir, extra));
        out.headers.emplace_back("Location", "/v1/scans/" + id);
        return out;
    }

    void PurgeExpired() {
        if (scanning_) return;
        std::error_code ec;
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::minutes(cfg_.retentionMin);
        for (fs::directory_iterator it(cfg_.spoolDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            auto t = fs::last_write_time(it->path(), e2);
            if (!e2 && t < cutoff) fs::remove_all(it->path(), e2);
        }
    }

    Config cfg_;
    std::unique_ptr<IScanBackend> backend_;
    std::mutex scanMutex_;
    std::atomic<bool> scanning_{ false };
    std::atomic<int> active_{ 0 };
};

// -----------------------------------------------------------------------------
//  Process entry
// -----------------------------------------------------------------------------
static bool AttachParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
}

static int SetStartup(bool enable) {
    if (enable) {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 1;
        std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\"";
        LSTATUS st = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName, REG_SZ, cmd.c_str(),
                                     static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        return st == ERROR_SUCCESS ? 0 : 1;
    }
    LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);   // never show error UI

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool console = false;
    int exitNow = -1;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--console") console = true;
        else if (a == L"--stop") {
            HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
            if (h) { SetEvent(h); CloseHandle(h); exitNow = 0; } else exitNow = 1;
        }
        else if (a == L"--install-startup") exitNow = SetStartup(true);
        else if (a == L"--remove-startup")  exitNow = SetStartup(false);
        else if (a == L"--version" || a == L"--help" || a == L"-h" || a == L"/?") {
            AttachParentConsole();
            std::printf("%s %s\n%s\n\nUsage: ScannerAPIGateway.exe [--console] [--stop] [--install-startup] [--remove-startup] [--version]\n"
                        "Config/log: %s\n", kProductName, kVersion, kCopyright, WideToUtf8(DataDirectory().wstring()).c_str());
            exitNow = 0;
        }
    }
    if (argv) LocalFree(argv);
    if (exitNow >= 0) return exitNow;
    if (console) AttachParentConsole();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already running: stay silent

    int rc = 0;
    try {
        Config cfg = LoadConfig();
        g_log.Init(cfg.dataDir / L"gateway.log", console);
        HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
        if (!stopEvent) { LogError("Cannot create stop event."); return 3; }
        {
            Server server(cfg);
            rc = server.Run(stopEvent);
        }
        CloseHandle(stopEvent);
    } catch (const std::exception& e) {
        LogError(std::string("Fatal: ") + e.what());
        rc = 4;
    } catch (...) {
        LogError("Fatal: unknown exception.");
        rc = 4;
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return rc;
}
