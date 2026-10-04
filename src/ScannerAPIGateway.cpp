// =============================================================================
//  Scanner API Gateway by RatioJuris
//  (c) 2026 RatioJuris
//
//  One windowless Windows x64 command-line exe that drives every kind of
//  scanner through ONE set of parameters and ONE JSON contract:
//
//    wia    Windows Image Acquisition 2.0     (built into Windows)
//    twain  TWAIN 2.x via the 64-bit DSM      (twaindsm.dll, system or beside the exe)
//    escl   eSCL / AirScan network scanners   (mDNS discovery + HTTP via WinHTTP)
//
//  Your application launches the exe, reads ONE JSON object from stdout and
//  picks up the image files from disk.
//
//  Commands
//    ScannerAPIGateway.exe list  [--backends wia,twain,escl]
//    ScannerAPIGateway.exe scan  --out <dir\ | file.ext> [options]
//    ScannerAPIGateway.exe version | help
//
//  Options for "scan" (identical for every backend)
//    --device <id|auto>     id from "list", e.g. wia:{GUID}\0000, twain:<name>,
//                           escl:http://192.168.1.50:80/eSCL      (default auto)
//    --backends <list>      restrict backends                      (default wia,twain,escl)
//    --mode <color|gray|bw>                                        (default color)
//    --dpi <75..1200>                                              (default 300)
//    --source <auto|flatbed|adf|duplex>                            (default auto)
//    --format <png|jpeg|bmp|tiff>   (default: from --out extension, else png)
//    --quality <1..100>     JPEG quality when the gateway encodes  (default 90)
//    --out <path>           directory (trailing \ or no extension) or file path
//    --area <l,t,w,h>       crop box in millimetres                (default full)
//    --brightness <-1000..1000>   hardware, WIA/TWAIN only         (optional)
//    --contrast   <-1000..1000>   hardware, WIA/TWAIN only         (optional)
//    --threshold  <0..255>        bw mode                          (default 128 for eSCL)
//    --blank-skip <0.0..1.0>      drop pages whose dark-pixel ratio is <= value (optional)
//    --pages <0..1000>      0 = everything the feeder holds        (default 0)
//    --timeout <5..600>     idle seconds before abort              (default 60)
//    --verbose              progress on stderr
//
//  Contract
//    stdout : exactly one JSON object, UTF-8 ({"ok":true,...} / {"ok":false,...})
//    stderr : only with --verbose
//    exit   : 0 ok | 1 failure | 2 bad arguments | 3 no scanner/device |
//             4 timeout | 5 scanner state (jam, empty, cover, busy...) |
//             6 backend/scanner unavailable (offline, comms, service, no DSM)
//
//  Architecture
//    IScanBackend implementations only ACQUIRE raw pages to temp files.
//    A shared pipeline (WIC) then does blank-page removal, mode/threshold
//    fixes, format conversion and final naming, so behaviour is the same
//    everywhere. Add a new scanner technology = add one IScanBackend.
//
//  TWAIN note: the TWAIN structures below are re-declared from the public
//  TWAIN 2.x specification; no TWAIN header or DLL is redistributed here.
// =============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <wincodec.h>
#include <io.h>
#include <fcntl.h>
#include <wia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "wiaguid.lib")
#pragma comment(lib, "uuid.lib")

static_assert(sizeof(void*) == 8, "Scanner API Gateway is a 64-bit only application.");

#ifndef WIA_IPA_FIRST
#define WIA_IPA_FIRST 2
#endif

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

static constexpr char kProductName[] = "Scanner API Gateway by RatioJuris";
static constexpr char kVendor[]      = "RatioJuris";
static constexpr char kCopyright[]   = "(c) 2026 RatioJuris";
static constexpr char kVersion[]     = "2.0.0";
static constexpr int  kContractVersion = 1;
static constexpr wchar_t kScanMutexName[] = L"Local\\RatioJuris.ScannerAPIGateway.Scan";
static constexpr int kMaxPages = 1000;
static constexpr int kEsclDiscoverMs = 2000;

// WIA image format GUIDs (stable, published values).
static const GUID kFmtBmp  = { 0xb96b3cab, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtJpeg = { 0xb96b3cae, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtPng  = { 0xb96b3caf, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID kFmtTiff = { 0xb96b3cb1, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

// -----------------------------------------------------------------------------
//  Utilities
// -----------------------------------------------------------------------------
static bool g_verbose = false;
static void Vlog(const std::string& m) {
    if (g_verbose) { std::fputs(("[gateway] " + m + "\n").c_str(), stderr); std::fflush(stderr); }
}

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
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
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

static std::optional<int> ToInt(const std::string& s) {
    std::string t = Trim(s);
    int v = 0;
    auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size()) return std::nullopt;
    return v;
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

static void EmitJson(const std::string& json) {
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static std::wstring Pad4(size_t n) {
    wchar_t b[24];
    swprintf_s(b, L"%04zu", n);
    return b;
}

// -----------------------------------------------------------------------------
//  Errors and exit codes
// -----------------------------------------------------------------------------
class ScanError : public std::runtime_error {
public:
    ScanError(std::string code, const std::string& msg, HRESULT hr = S_OK)
        : std::runtime_error(msg), code(std::move(code)), hr(hr) {}
    std::string code;
    HRESULT hr;
};

static int ExitCodeFor(const std::string& code) {
    static const std::set<std::string> usage = { "invalid_argument", "unknown_option", "missing_value", "usage" };
    static const std::set<std::string> noDevice = { "no_scanner", "device_not_found" };
    static const std::set<std::string> state = { "paper_empty", "paper_jam", "paper_problem", "cover_open", "device_busy",
                                                 "device_locked", "warming_up", "user_intervention", "multi_feed", "lamp_off", "busy" };
    static const std::set<std::string> unavailable = { "offline", "device_communication", "wia_unavailable",
                                                       "twain_unavailable", "escl_unavailable" };
    if (usage.count(code)) return 2;
    if (noDevice.count(code)) return 3;
    if (code == "timeout") return 4;
    if (state.count(code)) return 5;
    if (unavailable.count(code)) return 6;
    return 1;
}

static ScanError MapWiaError(HRESULT hr, const std::string& context, const char* defCode = "scan_failed") {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80210002UL: return ScanError("paper_jam", "Paper jam detected.", hr);
        case 0x80210003UL: return ScanError("paper_empty", "No paper in the document feeder.", hr);
        case 0x80210004UL: return ScanError("paper_problem", "Paper problem reported by the scanner.", hr);
        case 0x80210005UL: return ScanError("offline", "Scanner is offline or disconnected.", hr);
        case 0x80210006UL: return ScanError("device_busy", "Scanner is busy.", hr);
        case 0x80210007UL: return ScanError("warming_up", "Scanner is warming up.", hr);
        case 0x80210008UL: return ScanError("user_intervention", "Scanner needs user intervention.", hr);
        case 0x8021000AUL: return ScanError("device_communication", "Cannot communicate with the scanner.", hr);
        case 0x8021000DUL: return ScanError("device_locked", "Scanner is locked by another application.", hr);
        case 0x80210016UL: return ScanError("cover_open", "Scanner cover is open.", hr);
        case 0x80210017UL: return ScanError("lamp_off", "Scanner lamp is off.", hr);
        case 0x80210020UL: return ScanError("multi_feed", "Multiple sheets were fed at once.", hr);
        default:           return ScanError(defCode, context + " failed (" + HrHex(hr) + ").", hr);
    }
}

struct ComInit {
    HRESULT hr;
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) : hr(CoInitializeEx(nullptr, model)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
};

// Serialises scans across processes so two launches never fight over one device.
class ScanLock {
public:
    ScanLock() {
        h_ = CreateMutexW(nullptr, FALSE, kScanMutexName);
        if (!h_) return;
        if (WaitForSingleObject(h_, 30000) == WAIT_TIMEOUT) {
            CloseHandle(h_);
            h_ = nullptr;
            throw ScanError("busy", "Another scan is in progress (waited 30 s).");
        }
    }
    ~ScanLock() { if (h_) { ReleaseMutex(h_); CloseHandle(h_); } }
    ScanLock(const ScanLock&) = delete;
    ScanLock& operator=(const ScanLock&) = delete;
private:
    HANDLE h_ = nullptr;
};

// -----------------------------------------------------------------------------
//  Shared model + backend interface
// -----------------------------------------------------------------------------
struct DeviceInfo {
    std::string id;          // full id with backend prefix, e.g. "twain:Fujitsu fi-7160"
    std::string nativeId;    // id as the backend understands it
    std::string name, description, manufacturer, kind, backend;
};

struct BackendStatus {
    std::string name;
    bool available = true;
    std::string message;
};

struct ScanParams {
    std::string deviceId = "auto";
    std::string mode = "color";        // color | gray | bw
    int dpi = 300;
    std::string source = "auto";       // auto | flatbed | adf | duplex
    std::string format;                // png | jpeg | bmp | tiff ("" = decide from --out)
    int quality = 90;                  // JPEG quality when the gateway encodes
    bool hasArea = false;
    double area[4] = { 0, 0, 0, 0 };   // left, top, width, height (mm)
    std::optional<int> brightness, contrast, threshold;
    double blankSkip = 0.0;            // 0 = off
    int pages = 0;                     // 0 = all
    int timeoutSec = 60;
};

enum class PostMode { None, Gray, Bw };

// A page as acquired by a backend (temp file in some container format).
struct RawPage {
    fs::path file;
    std::string container;             // png | jpeg | bmp | tiff
    PostMode post = PostMode::None;    // fix-up the shared pipeline must apply
};

using PageNamer = std::function<fs::path(size_t /*1-based*/)>;

class IScanBackend {
public:
    virtual ~IScanBackend() = default;
    virtual std::string Name() const = 0;
    virtual BackendStatus ListDevices(std::vector<DeviceInfo>& out) = 0;   // never throws for "unavailable"
    virtual std::vector<RawPage> Scan(const ScanParams& p, const std::string& nativeId,
                                      const PageNamer& tempNamer, std::vector<std::string>& warnings) = 0;
};
using BackendList = std::vector<std::unique_ptr<IScanBackend>>;

static std::wstring ExtensionFor(const std::string& fmt) {
    if (fmt == "jpeg") return L".jpg";
    if (fmt == "bmp")  return L".bmp";
    if (fmt == "tiff") return L".tif";
    return L".png";
}
static const char* ContentTypeFor(const std::string& fmt) {
    if (fmt == "jpeg") return "image/jpeg";
    if (fmt == "bmp")  return "image/bmp";
    if (fmt == "tiff") return "image/tiff";
    return "image/png";
}

// -----------------------------------------------------------------------------
//  Shared image pipeline (Windows Imaging Component)
// -----------------------------------------------------------------------------
namespace img {

static ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> f;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.GetAddressOf()));
    if (FAILED(hr) || !f) throw ScanError("convert_failed", "Windows Imaging Component is unavailable.", hr);
    return f;
}

static const GUID& ContainerFor(const std::string& fmt) {
    if (fmt == "jpeg") return GUID_ContainerFormatJpeg;
    if (fmt == "bmp")  return GUID_ContainerFormatBmp;
    if (fmt == "tiff") return GUID_ContainerFormatTiff;
    return GUID_ContainerFormatPng;
}

static void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw ScanError("convert_failed", std::string("Image processing failed at ") + what + " (" + HrHex(hr) + ").", hr);
}

static ComPtr<IWICBitmapFrameDecode> OpenFrame(IWICImagingFactory* f, const fs::path& file) {
    ComPtr<IWICBitmapDecoder> dec;
    Check(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.GetAddressOf()), "decode");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(dec->GetFrame(0, frame.GetAddressOf()), "frame");
    return frame;
}

// True if the share of dark pixels (central area) is <= ratio.
static bool IsBlank(const fs::path& file, double ratio) {
    auto f = Factory();
    auto frame = OpenFrame(f.Get(), file);
    UINT w = 0, h = 0;
    Check(frame->GetSize(&w, &h), "size");
    ComPtr<IWICBitmapSource> src;
    Check(frame.As(&src), "source");
    ComPtr<IWICFormatConverter> conv;
    Check(f->CreateFormatConverter(conv.GetAddressOf()), "converter");
    Check(conv->Initialize(src.Get(), GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "gray");

    UINT m = std::max<UINT>(1, std::min(w, h) * 3 / 100);   // ignore scanner edge shadows
    WICRect rc = { 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
    if (w > 2 * m + 2 && h > 2 * m + 2) rc = { static_cast<INT>(m), static_cast<INT>(m), static_cast<INT>(w - 2 * m), static_cast<INT>(h - 2 * m) };
    std::vector<BYTE> buf(static_cast<size_t>(rc.Width) * static_cast<size_t>(rc.Height));
    if (buf.empty()) return false;
    Check(conv->CopyPixels(&rc, static_cast<UINT>(rc.Width), static_cast<UINT>(buf.size()), buf.data()), "pixels");
    size_t dark = 0;
    for (BYTE b : buf) if (b < 128) ++dark;
    return (static_cast<double>(dark) / static_cast<double>(buf.size())) <= ratio;
}

// Decode any supported container and re-encode as dstFormat, applying mode fix-ups.
static void ConvertFile(const fs::path& src, const fs::path& dst, const std::string& dstFormat,
                        int quality, int dpi, PostMode post, int bwThreshold) {
    auto f = Factory();
    auto frame = OpenFrame(f.Get(), src);
    UINT w = 0, h = 0;
    Check(frame->GetSize(&w, &h), "size");
    ComPtr<IWICBitmapSource> source;
    Check(frame.As(&source), "source");

    WICPixelFormatGUID srcPf;
    Check(frame->GetPixelFormat(&srcPf), "pixel format");
    GUID want = srcPf;

    const bool idx1 = IsEqualGUID(srcPf, GUID_WICPixelFormat1bppIndexed) != FALSE;
    const bool indexed = idx1 || IsEqualGUID(srcPf, GUID_WICPixelFormat2bppIndexed) || IsEqualGUID(srcPf, GUID_WICPixelFormat4bppIndexed) ||
                         IsEqualGUID(srcPf, GUID_WICPixelFormat8bppIndexed);
    if (indexed) {
        ComPtr<IWICPalette> pal;
        BOOL isBw = FALSE, isGray = FALSE;
        if (SUCCEEDED(f->CreatePalette(pal.GetAddressOf())) && SUCCEEDED(frame->CopyPalette(pal.Get()))) {
            pal->IsBlackWhite(&isBw);
            pal->IsGrayscale(&isGray);
        }
        want = (idx1 && isBw) ? GUID_WICPixelFormatBlackWhite : (isBw || isGray) ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR;
    }

    if (post == PostMode::Bw) {
        ComPtr<IWICFormatConverter> gc;
        Check(f->CreateFormatConverter(gc.GetAddressOf()), "converter");
        Check(gc->Initialize(source.Get(), GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "gray");
        std::vector<BYTE> gray(static_cast<size_t>(w) * h);
        Check(gc->CopyPixels(nullptr, w, static_cast<UINT>(gray.size()), gray.data()), "pixels");
        const UINT stride1 = (w + 7) / 8;
        std::vector<BYTE> bits(static_cast<size_t>(stride1) * h, 0);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (gray[static_cast<size_t>(y) * w + x] >= bwThreshold) bits[static_cast<size_t>(y) * stride1 + x / 8] |= static_cast<BYTE>(0x80 >> (x % 8));
        ComPtr<IWICBitmap> bmp;
        Check(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormatBlackWhite, stride1, static_cast<UINT>(bits.size()), bits.data(), bmp.GetAddressOf()), "bitmap");
        Check(bmp.As(&source), "source");
        srcPf = GUID_WICPixelFormatBlackWhite;
        want = GUID_WICPixelFormatBlackWhite;
    } else if (post == PostMode::Gray) {
        want = GUID_WICPixelFormat8bppGray;
    }

    ComPtr<IWICStream> stream;
    Check(f->CreateStream(stream.GetAddressOf()), "stream");
    Check(stream->InitializeFromFilename(dst.c_str(), GENERIC_WRITE), "output file");
    ComPtr<IWICBitmapEncoder> enc;
    Check(f->CreateEncoder(ContainerFor(dstFormat), nullptr, enc.GetAddressOf()), "encoder");
    Check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder init");
    ComPtr<IWICBitmapFrameEncode> fe;
    ComPtr<IPropertyBag2> props;
    Check(enc->CreateNewFrame(fe.GetAddressOf(), props.GetAddressOf()), "frame encoder");
    if (dstFormat == "jpeg" && props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = static_cast<float>(quality) / 100.0f;
        props->Write(1, &opt, &v);
    }
    Check(fe->Initialize(props.Get()), "frame init");
    Check(fe->SetSize(w, h), "set size");
    fe->SetResolution(static_cast<double>(dpi), static_cast<double>(dpi));
    WICPixelFormatGUID outPf = want;
    Check(fe->SetPixelFormat(&outPf), "pixel format");

    ComPtr<IWICBitmapSource> toWrite = source;
    if (!IsEqualGUID(outPf, srcPf)) {
        ComPtr<IWICFormatConverter> conv;
        Check(f->CreateFormatConverter(conv.GetAddressOf()), "converter");
        const WICBitmapPaletteType pal = IsEqualGUID(outPf, GUID_WICPixelFormatBlackWhite) ? WICBitmapPaletteTypeFixedBW : WICBitmapPaletteTypeCustom;
        Check(conv->Initialize(source.Get(), outPf, WICBitmapDitherTypeNone, nullptr, 0.0, pal), "convert");
        Check(conv.As(&toWrite), "source");
    }
    Check(fe->WriteSource(toWrite.Get(), nullptr), "write");
    Check(fe->Commit(), "commit frame");
    Check(enc->Commit(), "commit");
}

// Blank-page removal, fix-ups, conversion and final naming - identical for all backends.
static std::vector<fs::path> FinalizePages(std::vector<RawPage>& raw, const PageNamer& finalName, const ScanParams& p,
                                           std::vector<std::string>& warnings, int& skipped) {
    ComInit com;
    std::vector<fs::path> done;
    auto cleanup = [&] {
        std::error_code ec;
        for (auto& r : raw) fs::remove(r.file, ec);
        for (auto& d : done) fs::remove(d, ec);
    };
    try {
        for (auto& r : raw) {
            std::error_code ec;
            bool blank = false;
            if (p.blankSkip > 0.0) {
                try { blank = IsBlank(r.file, p.blankSkip); }
                catch (const ScanError& e) { warnings.push_back(std::string("Blank-page detection skipped: ") + e.what()); }
            }
            if (blank) { fs::remove(r.file, ec); ++skipped; continue; }

            const fs::path dst = finalName(done.size() + 1);
            if (r.container == p.format && r.post == PostMode::None) {
                if (!MoveFileExW(r.file.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
                    throw ScanError("output_error", "Cannot write the output file.", HRESULT_FROM_WIN32(GetLastError()));
            } else {
                ConvertFile(r.file, dst, p.format, p.quality, p.dpi, r.post, p.threshold.value_or(128));
                fs::remove(r.file, ec);
            }
            done.push_back(dst);
        }
    } catch (...) {
        cleanup();
        throw;
    }
    return done;
}

}  // namespace img

// -----------------------------------------------------------------------------
//  Backend 1: WIA 2.0
// -----------------------------------------------------------------------------
namespace wiabe {

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
        throw ScanError("wia_unavailable", "Windows Image Acquisition service is unavailable (" + HrHex(hr) + ").", hr);
    return m;
}

static std::vector<DeviceInfo> EnumerateCore(IWiaDevMgr2* mgr) {
    std::vector<DeviceInfo> out;
    ComPtr<IEnumWIA_DEV_INFO> en;
    HRESULT hr = mgr->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, en.GetAddressOf());
    if (FAILED(hr)) throw MapWiaError(hr, "Enumerating devices", "wia_unavailable");
    for (;;) {
        ComPtr<IWiaPropertyStorage> ps;
        ULONG got = 0;
        if (en->Next(1, ps.ReleaseAndGetAddressOf(), &got) != S_OK || got != 1) break;
        DeviceInfo d;
        d.nativeId     = WideToUtf8(ReadStr(ps.Get(), WIA_DIP_DEV_ID).value_or(L""));
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
        d.id = "wia:" + d.nativeId;
        if (!d.nativeId.empty()) out.push_back(std::move(d));
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

class ScanCallback : public IWiaTransferCallback {
public:
    ScanCallback(PageNamer namer, int maxPages) : namer_(std::move(namer)), maxPages_(maxPages) { Touch(); }

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
        fs::path file = namer_(pages_.size() + 1);
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
    PageNamer namer_;
    int maxPages_;
    std::vector<fs::path> pages_;
    bool capped_ = false;
    HRESULT errorStatus_ = S_OK;
};

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

    BackendStatus ListDevices(std::vector<DeviceInfo>& out) override {
        BackendStatus st; st.name = "wia";
        try {
            ComInit com;
            if (!com.ok()) throw ScanError("wia_unavailable", "Could not initialise COM.", com.hr);
            auto mgr = CreateManager();
            auto devs = EnumerateCore(mgr.Get());
            out.insert(out.end(), devs.begin(), devs.end());
        } catch (const ScanError& e) {
            st.available = false;
            st.message = e.what();
        }
        return st;
    }

    std::vector<RawPage> Scan(const ScanParams& p, const std::string& nativeId, const PageNamer& tempNamer,
                              std::vector<std::string>& warnings) override {
        ComInit com;
        if (!com.ok()) throw ScanError("wia_unavailable", "Could not initialise COM.", com.hr);
        auto mgr = CreateManager();

        Bstr bId(Utf8ToWide(nativeId));
        ComPtr<IWiaItem2> root;
        HRESULT hr = mgr->CreateDevice(0, bId.b, root.GetAddressOf());
        if (FAILED(hr) || !root) throw MapWiaError(hr, "Opening the scanner", "device_not_found");

        std::vector<ComPtr<IWiaItem2>> items;
        CollectItems(root.Get(), 0, items);
        if (items.empty()) throw ScanError("no_scan_item", "The device exposes no scannable items.");

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
            else throw ScanError("source_not_available", "The requested paper source is not available on this device.");
        }

        ComPtr<IWiaPropertyStorage> ps;
        if (FAILED(item.As(&ps)) || !ps) throw ScanError("no_properties", "Cannot access the scan item's properties.");

        auto hard = [&](PROPID id, LONG v, const char* name) {
            HRESULT h = WriteLong(ps.Get(), id, v);
            if (FAILED(h)) throw ScanError("unsupported_setting", std::string("The device rejected setting '") + name + "'.", h);
        };
        auto soft = [&](PROPID id, LONG v, const char* name) {
            HRESULT h = WriteLong(ps.Get(), id, v);
            if (FAILED(h)) warnings.push_back(std::string("Setting '") + name + "' was not applied by the device (" + HrHex(h) + ").");
        };

        if (p.source != "auto") {
            const LONG handling = (p.source == "flatbed") ? 0x2 : (p.source == "adf") ? 0x1 : 0x5;  // FLATBED / FEEDER / FEEDER|DUPLEX
            HRESULT h = WriteLong(ps.Get(), WIA_IPS_DOCUMENT_HANDLING_SELECT, handling);
            if (FAILED(h)) {
                ComPtr<IWiaPropertyStorage> rps;
                if (SUCCEEDED(root.As(&rps)) && rps) h = WriteLong(rps.Get(), WIA_IPS_DOCUMENT_HANDLING_SELECT, handling);
            }
            if (FAILED(h)) {
                if (p.source == "duplex") throw ScanError("source_not_available", "Duplex scanning is not supported by this device.", h);
                if (p.source == "adf" && !nameMatched) throw ScanError("source_not_available", "No document feeder available.", h);
                warnings.push_back("Could not select the paper source explicitly (" + HrHex(h) + ").");
            }
            if (wantFeeder) soft(WIA_IPS_PAGES, p.pages, "pages");
        }

        // Output format: ask the driver for the requested one, fall back to BMP and convert.
        std::string container = p.format;
        const GUID* fmt = &kFmtPng;
        if (p.format == "jpeg") fmt = &kFmtJpeg;
        else if (p.format == "bmp") fmt = &kFmtBmp;
        else if (p.format == "tiff") fmt = &kFmtTiff;
        hr = WriteGuid(ps.Get(), WIA_IPA_FORMAT, *fmt);
        if (FAILED(hr)) {
            HRESULT h2 = WriteGuid(ps.Get(), WIA_IPA_FORMAT, kFmtBmp);
            if (FAILED(h2)) throw ScanError("unsupported_format", "This driver cannot output '" + p.format + "' or BMP.", hr);
            container = "bmp";
            warnings.push_back("Driver cannot output " + p.format + "; scanned as BMP and converted.");
        }
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

        ComPtr<IWiaTransfer> xfer;
        if (FAILED(item.As(&xfer)) || !xfer) throw ScanError("transfer_unsupported", "The device does not support WIA 2.0 transfers.");
        const int cap = p.pages > 0 ? std::min(p.pages, kMaxPages) : kMaxPages;
        ComPtr<ScanCallback> cb;
        cb.Attach(new ScanCallback(tempNamer, cap));

        Vlog("WIA: scanning...");
        bool timedOut = false;
        {
            Watchdog wd(xfer.Get(), cb->lastActivity, static_cast<ULONGLONG>(p.timeoutSec) * 1000ULL);
            hr = xfer->Download(0, cb.Get());
            wd.Stop();
            timedOut = wd.TimedOut();
        }

        auto pages = cb->Pages();
        try {
            if (timedOut)
                throw ScanError("timeout", "The scanner showed no activity for " + std::to_string(p.timeoutSec) + " seconds; scan aborted.", hr);
            const bool capped = cb->Capped();
            if (!capped && hr != S_OK && hr != S_FALSE) {
                const bool feederEmptyAfterPages = (static_cast<unsigned long>(hr) == 0x80210003UL) && !pages.empty();
                if (!feederEmptyAfterPages) throw MapWiaError(hr, "Scanning");
            } else if (hr == S_FALSE && !pages.empty()) {
                warnings.push_back("The transfer ended early.");
            }
            if (pages.empty()) {
                HRESULT e = cb->ErrorStatus();
                if (FAILED(e)) throw MapWiaError(e, "Scanning");
                throw ScanError("no_pages", "The scanner returned no image data.", hr);
            }
            if (capped) warnings.push_back("Stopped at the maximum page count.");
        } catch (...) {
            for (auto& f : pages) { std::error_code ec; fs::remove(f, ec); }
            throw;
        }
        std::vector<RawPage> raw;
        for (auto& f : pages) raw.push_back({ f, container, PostMode::None });
        return raw;
    }
};

}  // namespace wiabe

// -----------------------------------------------------------------------------
//  Backend 2: TWAIN 2.x (64-bit DSM). Structures per the public TWAIN spec.
// -----------------------------------------------------------------------------
namespace twainbe {

namespace tw {
using UINT16 = unsigned short;
using INT16 = short;
using UINT32 = unsigned int;

#pragma pack(push, 2)
struct Version { UINT16 MajorNum, MinorNum, Language, Country; char Info[34]; };
struct Identity {
    UINT32 Id; Version Ver; UINT16 ProtocolMajor, ProtocolMinor; UINT32 SupportedGroups;
    char Manufacturer[34]; char ProductFamily[34]; char ProductName[34];
};
struct Capability { UINT16 Cap; UINT16 ConType; HANDLE hContainer; };
struct OneValue { UINT16 ItemType; UINT32 Item; };
struct Fix32 { INT16 Whole; UINT16 Frac; };
struct Frame { Fix32 Left, Top, Right, Bottom; };
struct ImageLayout { Frame frame; UINT32 DocumentNumber, PageNumber, FrameNumber; };
struct PendingXfers { UINT16 Count; UINT32 EOJ; };
struct UserInterface { UINT16 ShowUI; UINT16 ModalUI; HANDLE hParent; };
struct Event { void* pEvent; UINT16 TWMessage; };
struct Status { UINT16 ConditionCode; UINT16 Data; };
#pragma pack(pop)

constexpr UINT32 DG_CONTROL = 0x0001, DG_IMAGE = 0x0002;
constexpr UINT32 DF_DSM2 = 0x10000000, DF_APP2 = 0x20000000;
constexpr UINT16 DAT_CAPABILITY = 0x0001, DAT_EVENT = 0x0002, DAT_IDENTITY = 0x0003, DAT_PARENT = 0x0004,
                 DAT_PENDINGXFERS = 0x0005, DAT_STATUS = 0x0008, DAT_USERINTERFACE = 0x0009,
                 DAT_IMAGELAYOUT = 0x0102, DAT_IMAGENATIVEXFER = 0x0104;
constexpr UINT16 MSG_NULL = 0x0000, MSG_GET = 0x0001, MSG_GETCURRENT = 0x0002, MSG_GETFIRST = 0x0004, MSG_GETNEXT = 0x0005,
                 MSG_SET = 0x0006, MSG_RESET = 0x0007, MSG_XFERREADY = 0x0101, MSG_CLOSEDSREQ = 0x0102, MSG_CLOSEDSOK = 0x0103,
                 MSG_OPENDSM = 0x0301, MSG_CLOSEDSM = 0x0302, MSG_OPENDS = 0x0401, MSG_CLOSEDS = 0x0402,
                 MSG_DISABLEDS = 0x0501, MSG_ENABLEDS = 0x0502, MSG_PROCESSEVENT = 0x0601, MSG_ENDXFER = 0x0701;
constexpr UINT16 RC_SUCCESS = 0, RC_FAILURE = 1, RC_CHECKSTATUS = 2, RC_CANCEL = 3, RC_DSEVENT = 4, RC_NOTDSEVENT = 5,
                 RC_XFERDONE = 6, RC_ENDOFLIST = 7, RC_BUSY = 10, RC_SCANNERLOCKED = 11;
constexpr UINT16 CC_NODS = 3, CC_MAXCONNECTIONS = 4, CC_DENIED = 16, CC_PAPERJAM = 20, CC_PAPERDOUBLEFEED = 21, CC_CHECKDEVICEONLINE = 23;
constexpr UINT16 TWON_ONEVALUE = 5, TWON_DONTCARE16 = 0xFFFF;
constexpr UINT16 TWTY_INT16 = 1, TWTY_UINT16 = 4, TWTY_BOOL = 6, TWTY_FIX32 = 7;
constexpr UINT16 CAP_XFERCOUNT = 0x0001, ICAP_PIXELTYPE = 0x0101, ICAP_UNITS = 0x0102, ICAP_XFERMECH = 0x0103,
                 CAP_FEEDERENABLED = 0x1002, CAP_FEEDERLOADED = 0x1003, CAP_AUTOFEED = 0x1007,
                 CAP_DUPLEXENABLED = 0x1013, ICAP_BRIGHTNESS = 0x1101, ICAP_CONTRAST = 0x1103,
                 ICAP_XRESOLUTION = 0x1118, ICAP_YRESOLUTION = 0x1119, ICAP_THRESHOLD = 0x1123;
constexpr UINT16 TWPT_BW = 0, TWPT_GRAY = 1, TWPT_RGB = 2, TWUN_INCHES = 0, TWSX_NATIVE = 0;
}  // namespace tw

using DsmEntryProc = tw::UINT16(WINAPI*)(tw::Identity*, tw::Identity*, tw::UINT32, tw::UINT16, tw::UINT16, void*);

static ScanError MapTwainError(tw::UINT16 rc, tw::UINT16 cc, const std::string& ctx) {
    const std::string detail = " (TWRC=" + std::to_string(rc) + ", TWCC=" + std::to_string(cc) + ")";
    if (rc == tw::RC_BUSY) return ScanError("device_busy", "Scanner is busy." + detail);
    if (rc == tw::RC_SCANNERLOCKED) return ScanError("device_locked", "Scanner is locked." + detail);
    switch (cc) {
        case tw::CC_PAPERJAM:          return ScanError("paper_jam", "Paper jam detected." + detail);
        case tw::CC_PAPERDOUBLEFEED:   return ScanError("multi_feed", "Multiple sheets were fed at once." + detail);
        case tw::CC_CHECKDEVICEONLINE: return ScanError("offline", "Scanner is offline." + detail);
        case tw::CC_NODS:              return ScanError("device_not_found", "TWAIN data source not found." + detail);
        case tw::CC_DENIED:            return ScanError("device_locked", "Scanner denied the request." + detail);
        case tw::CC_MAXCONNECTIONS:    return ScanError("device_busy", "Scanner is already in use." + detail);
        default:                       return ScanError("scan_failed", ctx + " failed" + detail);
    }
}

static void WriteDibAsBmp(HANDLE h, const fs::path& file) {
    const SIZE_T total = GlobalSize(h);
    void* mem = GlobalLock(h);
    if (!mem || total < sizeof(BITMAPINFOHEADER)) throw ScanError("scan_failed", "The scanner returned an invalid image.");
    struct Unlock { HANDLE h; ~Unlock() { GlobalUnlock(h); } } unlock{ h };

    const auto* bih = static_cast<const BITMAPINFOHEADER*>(mem);
    DWORD colors = bih->biClrUsed;
    if (colors == 0 && bih->biBitCount <= 8) colors = 1u << bih->biBitCount;
    DWORD extra = colors * 4;
    if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) extra += 12;
    const DWORD headers = bih->biSize + extra;
    SIZE_T dibBytes = total;
    if (bih->biSizeImage != 0 && headers + bih->biSizeImage <= total) dibBytes = headers + bih->biSizeImage;

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER)) + headers;
    bfh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dibBytes);
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&bfh), sizeof bfh);
    out.write(static_cast<const char*>(mem), static_cast<std::streamsize>(dibBytes));
    out.flush();
    if (!out) throw ScanError("output_error", "Cannot write the temporary image file.");
}

class Session {
public:
    ~Session() { Teardown(); }

    void Load() {
        dll_ = LoadLibraryExW(L"twaindsm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) {
            wchar_t exe[MAX_PATH];
            DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                fs::path p = fs::path(std::wstring(exe, n)).parent_path() / L"twaindsm.dll";
                dll_ = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
        }
        if (!dll_) throw ScanError("twain_unavailable", "TWAIN Data Source Manager (64-bit twaindsm.dll) not found. Install a TWAIN driver or place twaindsm.dll next to the exe.");
        entry_ = reinterpret_cast<DsmEntryProc>(GetProcAddress(dll_, "DSM_Entry"));
        if (!entry_) throw ScanError("twain_unavailable", "twaindsm.dll does not export DSM_Entry.");
    }

    void MakeWindow() {
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);   // never shown
    }

    void OpenManager() {
        std::memset(&app_, 0, sizeof app_);
        app_.Ver.MajorNum = 1; app_.Ver.MinorNum = 0; app_.Ver.Language = 13; app_.Ver.Country = 1;   // English (USA)
        strcpy_s(app_.Ver.Info, "2.0.0");
        app_.ProtocolMajor = 2; app_.ProtocolMinor = 3;
        app_.SupportedGroups = tw::DF_APP2 | tw::DG_CONTROL | tw::DG_IMAGE;
        strcpy_s(app_.Manufacturer, "RatioJuris");
        strcpy_s(app_.ProductFamily, "Scanner API Gateway");
        strcpy_s(app_.ProductName, "Scanner API Gateway");
        tw::UINT16 rc = Call(nullptr, tw::DG_CONTROL, tw::DAT_PARENT, tw::MSG_OPENDSM, &hwnd_);
        if (rc != tw::RC_SUCCESS) throw ScanError("twain_unavailable", "Could not open the TWAIN Data Source Manager (TWRC=" + std::to_string(rc) + ").");
        state_ = 3;
    }

    std::vector<tw::Identity> Sources() {
        std::vector<tw::Identity> out;
        tw::Identity id;
        std::memset(&id, 0, sizeof id);
        tw::UINT16 rc = Call(nullptr, tw::DG_CONTROL, tw::DAT_IDENTITY, tw::MSG_GETFIRST, &id);
        while (rc == tw::RC_SUCCESS && out.size() < 64) {
            out.push_back(id);
            std::memset(&id, 0, sizeof id);
            rc = Call(nullptr, tw::DG_CONTROL, tw::DAT_IDENTITY, tw::MSG_GETNEXT, &id);
        }
        return out;
    }

    void OpenSource(const tw::Identity& chosen) {
        tw::Identity sel = chosen;
        tw::UINT16 rc = Call(nullptr, tw::DG_CONTROL, tw::DAT_IDENTITY, tw::MSG_OPENDS, &sel);
        if (rc != tw::RC_SUCCESS) throw MapTwainError(rc, Condition(nullptr), "Opening the scanner");
        ds_ = sel;
        state_ = 4;
    }

    std::vector<RawPage> Acquire(const ScanParams& p, const PageNamer& tempNamer, std::vector<std::string>& warnings) {
        auto fix = [](double v) {
            tw::Fix32 f;
            long l = static_cast<long>(std::lround(v * 65536.0));
            f.Whole = static_cast<tw::INT16>(l >> 16);
            f.Frac = static_cast<tw::UINT16>(l & 0xFFFF);
            return f;
        };
        auto fixBits = [&](double v) { tw::Fix32 f = fix(v); tw::UINT32 b = 0; std::memcpy(&b, &f, 4); return b; };
        auto cap = [&](const char* name, tw::UINT16 id, tw::UINT16 type, tw::UINT32 val, bool hardFail) {
            tw::UINT16 rc = SetCapOne(id, type, val);
            if (rc == tw::RC_SUCCESS) return true;
            if (rc == tw::RC_CHECKSTATUS) { warnings.push_back(std::string("Setting '") + name + "' was adjusted by the device."); return true; }
            const std::string msg = std::string("The scanner rejected setting '") + name + "' (TWRC=" + std::to_string(rc) + ", TWCC=" + std::to_string(Condition(&ds_)) + ").";
            if (hardFail) throw ScanError("unsupported_setting", msg);
            warnings.push_back(msg);
            return false;
        };

        cap("transfer mechanism", tw::ICAP_XFERMECH, tw::TWTY_UINT16, tw::TWSX_NATIVE, false);
        cap("units", tw::ICAP_UNITS, tw::TWTY_UINT16, tw::TWUN_INCHES, false);

        const bool feeder = (p.source == "adf" || p.source == "duplex");
        if (feeder) {
            if (!cap("feeder", tw::CAP_FEEDERENABLED, tw::TWTY_BOOL, 1, false)) throw ScanError("source_not_available", "No document feeder available.");
            auto loaded = GetCapBool(tw::CAP_FEEDERLOADED);
            if (loaded.has_value() && !*loaded) throw ScanError("paper_empty", "No paper in the document feeder.");
            cap("auto feed", tw::CAP_AUTOFEED, tw::TWTY_BOOL, 1, false);
            if (p.source == "duplex") {
                if (!cap("duplex", tw::CAP_DUPLEXENABLED, tw::TWTY_BOOL, 1, false)) throw ScanError("source_not_available", "Duplex scanning is not supported by this device.");
            } else {
                cap("duplex", tw::CAP_DUPLEXENABLED, tw::TWTY_BOOL, 0, false);
            }
            const int count = p.pages > 0 ? p.pages : -1;
            cap("page count", tw::CAP_XFERCOUNT, tw::TWTY_INT16, static_cast<tw::UINT32>(static_cast<tw::UINT16>(static_cast<tw::INT16>(count))), false);
        } else if (p.source == "flatbed") {
            cap("feeder", tw::CAP_FEEDERENABLED, tw::TWTY_BOOL, 0, false);
            cap("page count", tw::CAP_XFERCOUNT, tw::TWTY_INT16, 1, false);
        } else if (p.pages > 0) {
            cap("page count", tw::CAP_XFERCOUNT, tw::TWTY_INT16, static_cast<tw::UINT32>(p.pages), false);
        }

        const tw::UINT16 pixelType = p.mode == "bw" ? tw::TWPT_BW : p.mode == "gray" ? tw::TWPT_GRAY : tw::TWPT_RGB;
        cap("data type", tw::ICAP_PIXELTYPE, tw::TWTY_UINT16, pixelType, true);
        cap("resolution", tw::ICAP_XRESOLUTION, tw::TWTY_FIX32, fixBits(p.dpi), true);
        cap("resolution", tw::ICAP_YRESOLUTION, tw::TWTY_FIX32, fixBits(p.dpi), true);
        if (p.brightness) cap("brightness", tw::ICAP_BRIGHTNESS, tw::TWTY_FIX32, fixBits(*p.brightness), false);
        if (p.contrast)   cap("contrast", tw::ICAP_CONTRAST, tw::TWTY_FIX32, fixBits(*p.contrast), false);
        if (p.threshold)  cap("threshold", tw::ICAP_THRESHOLD, tw::TWTY_FIX32, fixBits(*p.threshold), false);

        if (p.hasArea) {
            tw::ImageLayout lay;
            std::memset(&lay, 0, sizeof lay);
            Call(&ds_, tw::DG_IMAGE, tw::DAT_IMAGELAYOUT, tw::MSG_GET, &lay);
            const double l = p.area[0] / 25.4, t = p.area[1] / 25.4;
            lay.frame.Left = fix(l);
            lay.frame.Top = fix(t);
            lay.frame.Right = fix(l + p.area[2] / 25.4);
            lay.frame.Bottom = fix(t + p.area[3] / 25.4);
            tw::UINT16 rc = Call(&ds_, tw::DG_IMAGE, tw::DAT_IMAGELAYOUT, tw::MSG_SET, &lay);
            if (rc == tw::RC_CHECKSTATUS) warnings.push_back("The scan area was adjusted by the device.");
            else if (rc != tw::RC_SUCCESS) throw ScanError("unsupported_setting", "The scanner rejected the scan area.");
        }

        // Enable the source without any UI.
        tw::UserInterface ui;
        std::memset(&ui, 0, sizeof ui);
        ui.hParent = hwnd_;
        tw::UINT16 rc = Call(&ds_, tw::DG_CONTROL, tw::DAT_USERINTERFACE, tw::MSG_ENABLEDS, &ui);
        if (rc != tw::RC_SUCCESS && rc != tw::RC_CHECKSTATUS) throw MapTwainError(rc, Condition(&ds_), "Starting the scan (the driver may require its own UI)");
        state_ = 5;

        Vlog("TWAIN: waiting for the scanner...");
        if (!PumpUntilReady(p.timeoutSec)) throw ScanError("scan_failed", "The scanner closed without delivering an image.");
        state_ = 6;

        std::vector<RawPage> pages;
        auto discard = [&] { std::error_code ec; for (auto& r : pages) fs::remove(r.file, ec); };
        try {
            for (;;) {
                HANDLE hDib = nullptr;
                rc = Call(&ds_, tw::DG_IMAGE, tw::DAT_IMAGENATIVEXFER, tw::MSG_GET, &hDib);
                if (rc == tw::RC_XFERDONE && hDib) {
                    state_ = 7;
                    fs::path file = tempNamer(pages.size() + 1);
                    struct Free { HANDLE h; ~Free() { GlobalFree(h); } } freeDib{ hDib };
                    WriteDibAsBmp(hDib, file);
                    pages.push_back({ file, "bmp", PostMode::None });
                } else if (rc == tw::RC_CANCEL) {
                    warnings.push_back("The scan was cancelled by the scanner driver.");
                    break;
                } else {
                    throw MapTwainError(rc, Condition(&ds_), "Transferring the image");
                }

                tw::PendingXfers pend;
                std::memset(&pend, 0, sizeof pend);
                rc = Call(&ds_, tw::DG_CONTROL, tw::DAT_PENDINGXFERS, tw::MSG_ENDXFER, &pend);
                if (rc != tw::RC_SUCCESS) throw MapTwainError(rc, Condition(&ds_), "Finishing the transfer");
                state_ = pend.Count == 0 ? 5 : 6;
                if (pend.Count == 0) break;
                if ((p.pages > 0 && static_cast<int>(pages.size()) >= p.pages) || static_cast<int>(pages.size()) >= kMaxPages) {
                    Call(&ds_, tw::DG_CONTROL, tw::DAT_PENDINGXFERS, tw::MSG_RESET, &pend);
                    state_ = 5;
                    break;
                }
            }
        } catch (...) {
            discard();
            throw;
        }
        if (pages.empty()) throw ScanError("no_pages", "The scanner returned no image data.");
        return pages;
    }

private:
    tw::UINT16 Call(tw::Identity* dest, tw::UINT32 dg, tw::UINT16 dat, tw::UINT16 msg, void* data) {
        return entry_(&app_, dest, dg, dat, msg, data);
    }

    tw::UINT16 Condition(tw::Identity* dest) {
        tw::Status st;
        std::memset(&st, 0, sizeof st);
        if (entry_(&app_, dest, tw::DG_CONTROL, tw::DAT_STATUS, tw::MSG_GET, &st) == tw::RC_SUCCESS) return st.ConditionCode;
        return 0xFFFF;
    }

    tw::UINT16 SetCapOne(tw::UINT16 cap, tw::UINT16 itemType, tw::UINT32 value) {
        HANDLE h = GlobalAlloc(GHND, sizeof(tw::OneValue));
        if (!h) return tw::RC_FAILURE;
        auto* ov = static_cast<tw::OneValue*>(GlobalLock(h));
        ov->ItemType = itemType;
        ov->Item = value;
        GlobalUnlock(h);
        tw::Capability c;
        c.Cap = cap; c.ConType = tw::TWON_ONEVALUE; c.hContainer = h;
        tw::UINT16 rc = Call(&ds_, tw::DG_CONTROL, tw::DAT_CAPABILITY, tw::MSG_SET, &c);
        GlobalFree(h);
        return rc;
    }

    std::optional<bool> GetCapBool(tw::UINT16 cap) {
        tw::Capability c;
        c.Cap = cap; c.ConType = tw::TWON_DONTCARE16; c.hContainer = nullptr;
        if (Call(&ds_, tw::DG_CONTROL, tw::DAT_CAPABILITY, tw::MSG_GETCURRENT, &c) != tw::RC_SUCCESS || !c.hContainer) return std::nullopt;
        std::optional<bool> result;
        if (c.ConType == tw::TWON_ONEVALUE) {
            auto* ov = static_cast<tw::OneValue*>(GlobalLock(c.hContainer));
            if (ov) { result = ov->Item != 0; GlobalUnlock(c.hContainer); }
        }
        GlobalFree(c.hContainer);
        return result;
    }

    // Windows TWAIN needs a message pump; every message goes to the source first.
    bool PumpUntilReady(int idleSec) {
        const ULONGLONG idleMs = static_cast<ULONGLONG>(idleSec) * 1000ULL;
        ULONGLONG last = GetTickCount64();
        MSG msg;
        for (;;) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                last = GetTickCount64();
                tw::Event ev;
                ev.pEvent = &msg;
                ev.TWMessage = tw::MSG_NULL;
                tw::UINT16 rc = Call(&ds_, tw::DG_CONTROL, tw::DAT_EVENT, tw::MSG_PROCESSEVENT, &ev);
                if (rc == tw::RC_DSEVENT) {
                    if (ev.TWMessage == tw::MSG_XFERREADY) return true;
                    if (ev.TWMessage == tw::MSG_CLOSEDSREQ || ev.TWMessage == tw::MSG_CLOSEDSOK) return false;
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (GetTickCount64() - last > idleMs)
                throw ScanError("timeout", "The scanner showed no activity for " + std::to_string(idleSec) + " seconds; scan aborted.");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
    }

    void Teardown() noexcept {
        if (entry_) {
            tw::PendingXfers px;
            std::memset(&px, 0, sizeof px);
            if (state_ == 7) { Call(&ds_, tw::DG_CONTROL, tw::DAT_PENDINGXFERS, tw::MSG_ENDXFER, &px); state_ = px.Count ? 6 : 5; }
            if (state_ == 6) { Call(&ds_, tw::DG_CONTROL, tw::DAT_PENDINGXFERS, tw::MSG_RESET, &px); state_ = 5; }
            if (state_ == 5) {
                tw::UserInterface ui;
                std::memset(&ui, 0, sizeof ui);
                ui.hParent = hwnd_;
                Call(&ds_, tw::DG_CONTROL, tw::DAT_USERINTERFACE, tw::MSG_DISABLEDS, &ui);
                state_ = 4;
            }
            if (state_ == 4) { Call(nullptr, tw::DG_CONTROL, tw::DAT_IDENTITY, tw::MSG_CLOSEDS, &ds_); state_ = 3; }
            if (state_ == 3) { Call(nullptr, tw::DG_CONTROL, tw::DAT_PARENT, tw::MSG_CLOSEDSM, &hwnd_); state_ = 2; }
        }
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (dll_) { FreeLibrary(dll_); dll_ = nullptr; }
    }

    HMODULE dll_ = nullptr;
    DsmEntryProc entry_ = nullptr;
    HWND hwnd_ = nullptr;
    tw::Identity app_ = {};
    tw::Identity ds_ = {};
    int state_ = 1;
};

class TwainBackend : public IScanBackend {
public:
    std::string Name() const override { return "twain"; }

    BackendStatus ListDevices(std::vector<DeviceInfo>& out) override {
        BackendStatus st; st.name = "twain";
        try {
            ComInit com(COINIT_APARTMENTTHREADED);
            Session s;
            s.Load();
            s.MakeWindow();
            s.OpenManager();
            for (auto& id : s.Sources()) {
                DeviceInfo d;
                d.nativeId = id.ProductName;
                d.id = "twain:" + d.nativeId;
                d.name = id.ProductName;
                d.description = id.ProductFamily;
                d.manufacturer = id.Manufacturer;
                d.kind = "scanner";
                d.backend = "twain";
                out.push_back(std::move(d));
            }
        } catch (const ScanError& e) {
            st.available = false;
            st.message = e.what();
        }
        return st;
    }

    std::vector<RawPage> Scan(const ScanParams& p, const std::string& nativeId, const PageNamer& tempNamer,
                              std::vector<std::string>& warnings) override {
        ComInit com(COINIT_APARTMENTTHREADED);
        Session s;
        s.Load();
        s.MakeWindow();
        s.OpenManager();
        auto sources = s.Sources();
        const tw::Identity* chosen = nullptr;
        for (auto& id : sources) if (ToLower(id.ProductName) == ToLower(nativeId)) { chosen = &id; break; }
        if (!chosen) throw ScanError("device_not_found", "TWAIN source '" + nativeId + "' was not found.");
        s.OpenSource(*chosen);
        return s.Acquire(p, tempNamer, warnings);
    }
};

}  // namespace twainbe

// -----------------------------------------------------------------------------
//  Backend 3: eSCL / AirScan (network). mDNS discovery + HTTP via WinHTTP.
// -----------------------------------------------------------------------------
namespace esclbe {

// ---- tiny XML helpers (namespace prefixes ignored) ----
namespace xml {
static std::string Local(std::string tag) {
    size_t c = tag.rfind(':');
    return c == std::string::npos ? tag : tag.substr(c + 1);
}
static size_t FindOpen(const std::string& x, const std::string& name, size_t from) {
    for (size_t p = x.find('<', from); p != std::string::npos; p = x.find('<', p + 1)) {
        if (p + 1 >= x.size()) break;
        char c = x[p + 1];
        if (c == '/' || c == '?' || c == '!') continue;
        size_t q = p + 1;
        while (q < x.size() && !std::strchr(" \t\r\n/>", x[q])) ++q;
        if (Local(x.substr(p + 1, q - p - 1)) == name) return p;
    }
    return std::string::npos;
}
static std::optional<std::string> Text(const std::string& x, const std::string& name, size_t from = 0) {
    size_t p = FindOpen(x, name, from);
    if (p == std::string::npos) return std::nullopt;
    size_t gt = x.find('>', p);
    if (gt == std::string::npos) return std::nullopt;
    if (x[gt - 1] == '/') return std::string();
    const size_t start = gt + 1;
    for (size_t c = x.find("</", start); c != std::string::npos; c = x.find("</", c + 1)) {
        size_t q = x.find('>', c);
        if (q == std::string::npos) break;
        if (Local(x.substr(c + 2, q - c - 2)) == name) return Trim(x.substr(start, c - start));
    }
    return std::nullopt;
}
}  // namespace xml

// ---- WinHTTP ----
struct Url {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 80;
    std::wstring path;
};

static Url CrackUrl(const std::string& s, bool defaultPath) {
    std::wstring w = Utf8ToWide(s);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = { 0 }, path[1024] = { 0 };
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) throw ScanError("invalid_argument", "Invalid eSCL device URL '" + s + "'.");
    Url u;
    u.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    u.host = host;
    u.port = uc.nPort;
    u.path = path;
    while (!u.path.empty() && u.path.back() == L'/') u.path.pop_back();
    if (u.path.empty() && defaultPath) u.path = L"/eSCL";
    return u;
}

static Url ParseNative(const std::string& native) {
    std::string n = Trim(native);
    if (ToLower(n).rfind("http://", 0) != 0 && ToLower(n).rfind("https://", 0) != 0) {
        while (n.rfind("//", 0) == 0) n.erase(0, 2);
        n = "http://" + n;
    }
    return CrackUrl(n, true);
}

struct HttpResult {
    DWORD status = 0;
    std::wstring location;
    std::string body;
};

class Http {
public:
    Http() {
        session_ = WinHttpOpen(L"ScannerAPIGateway/2.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) throw ScanError("escl_unavailable", "WinHTTP is unavailable.");
    }
    ~Http() { if (session_) WinHttpCloseHandle(session_); }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResult Request(const Url& u, const wchar_t* method, const std::wstring& path, const std::string* body, int recvTimeoutMs) {
        Handle conn(WinHttpConnect(session_, u.host.c_str(), u.port, 0));
        if (!conn.h) Fail();
        Handle req(WinHttpOpenRequest(conn.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      u.https ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h) Fail();
        if (u.https) {   // LAN devices use self-signed certificates
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(req.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
        }
        WinHttpSetTimeouts(req.h, 5000, 5000, 15000, recvTimeoutMs);

        BOOL ok;
        if (body) {
            static const wchar_t hdr[] = L"Content-Type: text/xml\r\n";
            ok = WinHttpSendRequest(req.h, hdr, static_cast<DWORD>(-1L), const_cast<char*>(body->data()),
                                    static_cast<DWORD>(body->size()), static_cast<DWORD>(body->size()), 0);
        } else {
            ok = WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }
        if (!ok || !WinHttpReceiveResponse(req.h, nullptr)) Fail();

        HttpResult r;
        DWORD size = sizeof(DWORD);
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t loc[1024] = { 0 };
        size = sizeof loc - sizeof(wchar_t);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &size, WINHTTP_NO_HEADER_INDEX)) r.location = loc;

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail)) Fail();
            if (avail == 0) break;
            std::vector<char> buf(avail);
            DWORD got = 0;
            if (!WinHttpReadData(req.h, buf.data(), avail, &got)) Fail();
            if (got == 0) break;
            r.body.append(buf.data(), got);
        }
        return r;
    }

private:
    struct Handle {
        HINTERNET h;
        explicit Handle(HINTERNET x) : h(x) {}
        ~Handle() { if (h) WinHttpCloseHandle(h); }
    };
    [[noreturn]] static void Fail() {
        DWORD e = GetLastError();
        if (e == ERROR_WINHTTP_TIMEOUT) throw ScanError("timeout", "The network scanner did not respond in time.");
        throw ScanError("device_communication", "HTTP request to the network scanner failed (error " + std::to_string(e) + ").");
    }
    HINTERNET session_ = nullptr;
};

// ---- mDNS discovery (_uscan._tcp / _uscans._tcp) ----
struct Found {
    std::string name, note, ip, rs;
    unsigned port = 80;
    bool tls = false;
};

struct MdnsState {
    std::map<std::string, std::pair<std::string, bool>> ptr;                 // instance(lower) -> (original, tls)
    std::map<std::string, std::pair<std::string, uint16_t>> srv;             // instance(lower) -> (target host lower, port)
    std::map<std::string, std::map<std::string, std::string>> txt;           // instance(lower) -> key/values
    std::map<std::string, std::string> a;                                     // host(lower) -> ipv4
    std::map<std::string, std::string> sender;                                // instance(lower) -> responder ip
};

static bool ReadName(const uint8_t* d, size_t n, size_t pos, std::string& out, size_t& endPos) {
    out.clear();
    bool jumped = false;
    int hops = 0;
    size_t p = pos;
    for (;;) {
        if (p >= n) return false;
        uint8_t len = d[p];
        if (len == 0) { if (!jumped) endPos = p + 1; return true; }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= n) return false;
            size_t off = (static_cast<size_t>(len & 0x3F) << 8) | d[p + 1];
            if (!jumped) endPos = p + 2;
            jumped = true;
            if (off >= n || ++hops > 32) return false;
            p = off;
            continue;
        }
        if (len > 63 || p + 1 + len > n) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(d) + p + 1, len);
        p += 1 + len;
    }
}

static void ParsePacket(const uint8_t* d, size_t n, const std::string& from, MdnsState& st) {
    if (n < 12) return;
    auto rd16 = [&](size_t p) { return static_cast<uint16_t>((d[p] << 8) | d[p + 1]); };
    const size_t qd = rd16(4), records = static_cast<size_t>(rd16(6)) + rd16(8) + rd16(10);
    size_t pos = 12;
    std::string name;
    size_t end = 0;
    for (size_t i = 0; i < qd; ++i) {
        if (!ReadName(d, n, pos, name, end)) return;
        pos = end + 4;
        if (pos > n) return;
    }
    for (size_t i = 0; i < records; ++i) {
        std::string owner;
        if (!ReadName(d, n, pos, owner, end)) return;
        pos = end;
        if (pos + 10 > n) return;
        const uint16_t type = rd16(pos), rdlen = rd16(pos + 8);
        pos += 10;
        if (pos + rdlen > n) return;
        const size_t rdata = pos;
        const std::string ownerLow = ToLower(owner);
        if (type == 12) {            // PTR
            std::string target; size_t e2 = 0;
            if (ReadName(d, n, rdata, target, e2) && (ownerLow == "_uscan._tcp.local" || ownerLow == "_uscans._tcp.local")) {
                st.ptr[ToLower(target)] = { target, ownerLow == "_uscans._tcp.local" };
                st.sender.emplace(ToLower(target), from);
            }
        } else if (type == 33 && rdlen >= 6) {   // SRV
            std::string target; size_t e2 = 0;
            if (ReadName(d, n, rdata + 6, target, e2)) {
                st.srv[ownerLow] = { ToLower(target), rd16(rdata + 4) };
                st.sender.emplace(ownerLow, from);
            }
        } else if (type == 16) {     // TXT
            size_t p = rdata;
            while (p < rdata + rdlen) {
                size_t l = d[p++];
                if (p + l > rdata + rdlen) break;
                std::string kv(reinterpret_cast<const char*>(d) + p, l);
                p += l;
                size_t eq = kv.find('=');
                if (eq != std::string::npos) st.txt[ownerLow][ToLower(kv.substr(0, eq))] = kv.substr(eq + 1);
            }
        } else if (type == 1 && rdlen == 4) {    // A
            char ip[INET_ADDRSTRLEN] = { 0 };
            in_addr ia;
            std::memcpy(&ia, d + rdata, 4);
            inet_ntop(AF_INET, &ia, ip, sizeof ip);
            st.a[ownerLow] = ip;
        }
        pos += rdlen;
    }
}

static std::vector<std::string> LocalIPv4() {
    std::vector<std::string> out;
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (size == 0) return out;
    std::vector<BYTE> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR) return out;
    for (; aa; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
            out.push_back(ip);
        }
    }
    return out;
}

static std::vector<Found> Discover(int waitMs) {
    std::vector<Found> result;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return result;
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } guard;

    std::vector<uint8_t> query = { 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0 };
    for (const char* svc : { "_uscan._tcp.local", "_uscans._tcp.local" }) {
        for (const std::string& label : Split(svc, '.')) {
            query.push_back(static_cast<uint8_t>(label.size()));
            query.insert(query.end(), label.begin(), label.end());
        }
        query.push_back(0);
        query.push_back(0); query.push_back(12);       // PTR
        query.push_back(0x80); query.push_back(0x01);  // IN + unicast-response
    }

    std::vector<SOCKET> socks;
    for (const std::string& ip : LocalIPv4()) {
        if (socks.size() >= 32) break;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        a.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); continue; }
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a.sin_addr), sizeof a.sin_addr);
        int ttl = 255;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
        socks.push_back(s);
    }
    if (socks.empty()) return result;

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    auto sendQuery = [&] {
        for (SOCKET s : socks) sendto(s, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
    };
    sendQuery();

    MdnsState st;
    const auto t0 = std::chrono::steady_clock::now();
    bool resent = false;
    for (;;) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= waitMs) break;
        fd_set rf;
        FD_ZERO(&rf);
        for (SOCKET s : socks) FD_SET(s, &rf);
        timeval tv{ 0, 100000 };
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (r > 0) {
            for (SOCKET s : socks) {
                if (!FD_ISSET(s, &rf)) continue;
                uint8_t buf[9000];
                sockaddr_in from = {};
                int fl = sizeof from;
                int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (n > 0) {
                    char ip[INET_ADDRSTRLEN] = { 0 };
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                    ParsePacket(buf, static_cast<size_t>(n), ip, st);
                }
            }
        }
        if (!resent && elapsed > 700) { sendQuery(); resent = true; }
    }
    for (SOCKET s : socks) closesocket(s);

    for (auto& kv : st.ptr) {
        auto srv = st.srv.find(kv.first);
        if (srv == st.srv.end()) continue;
        Found f;
        auto aIt = st.a.find(srv->second.first);
        if (aIt != st.a.end()) f.ip = aIt->second;
        else { auto sd = st.sender.find(kv.first); if (sd != st.sender.end()) f.ip = sd->second; }
        if (f.ip.empty()) continue;
        f.port = srv->second.second;
        f.tls = kv.second.second;
        auto& txt = st.txt[kv.first];
        std::string rs = txt.count("rs") ? txt["rs"] : "eSCL";
        while (!rs.empty() && rs.front() == '/') rs.erase(0, 1);
        f.rs = "/" + (rs.empty() ? std::string("eSCL") : rs);
        f.name = txt.count("ty") ? txt["ty"] : Split(kv.second.first, '.')[0];
        f.note = txt.count("note") ? txt["note"] : "";
        result.push_back(std::move(f));
    }
    return result;
}

static std::wstring JobPath(const std::wstring& location) {
    std::wstring path = location;
    if (location.rfind(L"http", 0) == 0) {
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256] = { 0 }, up[1024] = { 0 };
        uc.lpszHostName = host; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = up;    uc.dwUrlPathLength = 1023;
        if (WinHttpCrackUrl(location.c_str(), 0, 0, &uc)) path = up;
    }
    while (!path.empty() && path.back() == L'/') path.pop_back();
    return path;
}

class EsclBackend : public IScanBackend {
public:
    std::string Name() const override { return "escl"; }

    BackendStatus ListDevices(std::vector<DeviceInfo>& out) override {
        BackendStatus st; st.name = "escl";
        try {
            std::set<std::string> seen;
            for (auto& f : Discover(kEsclDiscoverMs)) {
                DeviceInfo d;
                d.nativeId = std::string(f.tls ? "https://" : "http://") + f.ip + ":" + std::to_string(f.port) + f.rs;
                if (!seen.insert(d.nativeId).second) continue;
                d.id = "escl:" + d.nativeId;
                d.name = f.name;
                d.description = f.note;
                d.kind = "scanner";
                d.backend = "escl";
                out.push_back(std::move(d));
            }
        } catch (const std::exception& e) {
            st.available = false;
            st.message = e.what();
        }
        return st;
    }

    std::vector<RawPage> Scan(const ScanParams& p, const std::string& nativeId, const PageNamer& tempNamer,
                              std::vector<std::string>& warnings) override {
        const Url u = ParseNative(nativeId);
        Http http;
        const int recvMs = p.timeoutSec * 1000;

        HttpResult caps = http.Request(u, L"GET", u.path + L"/ScannerCapabilities", nullptr, std::max(recvMs, 15000));
        if (caps.status != 200) throw ScanError("device_communication", "The scanner did not return its capabilities (HTTP " + std::to_string(caps.status) + ").");
        const std::string& cx = caps.body;

        // ---- paper source ----
        const bool hasPlaten = xml::FindOpen(cx, "Platen", 0) != std::string::npos;
        const bool hasAdf = xml::FindOpen(cx, "Adf", 0) != std::string::npos;
        const bool wantFeeder = (p.source == "adf" || p.source == "duplex");
        std::string input = "Platen", region = "PlatenInputCaps";
        if (wantFeeder || (p.source == "auto" && !hasPlaten && hasAdf)) {
            if (!hasAdf) throw ScanError("source_not_available", "No document feeder available.");
            input = "Feeder";
            if (p.source == "duplex") {
                if (xml::FindOpen(cx, "AdfDuplexInputCaps", 0) == std::string::npos) throw ScanError("source_not_available", "Duplex scanning is not supported by this device.");
                region = "AdfDuplexInputCaps";
            } else {
                region = "AdfSimplexInputCaps";
            }
        } else if (p.source == "flatbed" && !hasPlaten && hasAdf) {
            throw ScanError("source_not_available", "No flatbed available on this device.");
        }
        std::string caps_region = xml::Text(cx, region).value_or(xml::Text(cx, input == "Platen" ? "Platen" : "Adf").value_or(cx));

        // ---- geometry (300ths of an inch) ----
        int maxW = ToInt(xml::Text(caps_region, "MaxWidth").value_or("")).value_or(2550);
        int maxH = ToInt(xml::Text(caps_region, "MaxHeight").value_or("")).value_or(3507);
        int xo = 0, yo = 0, wid = maxW, hgt = maxH;
        if (p.hasArea) {
            auto u300 = [](double mm) { return static_cast<int>(std::lround(mm / 25.4 * 300.0)); };
            xo = u300(p.area[0]); yo = u300(p.area[1]); wid = u300(p.area[2]); hgt = u300(p.area[3]);
            if (xo + wid > maxW || yo + hgt > maxH)
                throw ScanError("unsupported_setting", "The scan area exceeds the scanner's maximum (" + std::to_string(maxW * 254 / 3000) + " x " + std::to_string(maxH * 254 / 3000) + " mm).");
        }

        // ---- resolution: snap to a supported discrete value ----
        int dpi = p.dpi;
        std::vector<int> supported;
        for (size_t pos = 0; (pos = xml::FindOpen(caps_region, "XResolution", pos)) != std::string::npos; ++pos) {
            auto t = xml::Text(caps_region, "XResolution", pos);
            if (t && t->find('<') == std::string::npos) if (auto v = ToInt(*t)) supported.push_back(*v);
        }
        if (!supported.empty() && std::find(supported.begin(), supported.end(), dpi) == supported.end()) {
            int best = supported.front();
            for (int v : supported) if (std::abs(v - dpi) < std::abs(best - dpi)) best = v;
            warnings.push_back("Resolution " + std::to_string(dpi) + " dpi is not supported; used " + std::to_string(best) + " dpi.");
            dpi = best;
        }

        // ---- colour mode: gray/bw are made by the shared pipeline if the device lacks them ----
        const bool hasGray = cx.find("Grayscale8") != std::string::npos;
        std::string color = "RGB24";
        PostMode post = PostMode::None;
        if (p.mode == "gray") { if (hasGray) color = "Grayscale8"; else post = PostMode::Gray; }
        else if (p.mode == "bw") { color = hasGray ? "Grayscale8" : "RGB24"; post = PostMode::Bw; }

        // ---- document format: JPEG preferred, PNG fallback; PDF-only devices are not usable ----
        std::string mime = "image/jpeg", container = "jpeg";
        if (cx.find("image/jpeg") == std::string::npos) {
            if (cx.find("image/png") != std::string::npos) { mime = "image/png"; container = "png"; }
            else if (cx.find("application/pdf") != std::string::npos) throw ScanError("unsupported_format", "This scanner can only deliver PDF over eSCL.");
        }
        if (p.brightness || p.contrast) warnings.push_back("Brightness/contrast are hardware settings and are not available over eSCL; ignored.");

        std::ostringstream s;
        s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">"
          << "<pwg:Version>2.6</pwg:Version>"
          << "<pwg:ScanRegions><pwg:ScanRegion><pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>"
          << "<pwg:Height>" << hgt << "</pwg:Height><pwg:Width>" << wid << "</pwg:Width>"
          << "<pwg:XOffset>" << xo << "</pwg:XOffset><pwg:YOffset>" << yo << "</pwg:YOffset></pwg:ScanRegion></pwg:ScanRegions>"
          << "<pwg:InputSource>" << input << "</pwg:InputSource>"
          << "<scan:ColorMode>" << color << "</scan:ColorMode>"
          << "<scan:XResolution>" << dpi << "</scan:XResolution><scan:YResolution>" << dpi << "</scan:YResolution>"
          << "<pwg:DocumentFormat>" << mime << "</pwg:DocumentFormat>";
        if (p.source == "duplex") s << "<scan:Duplex>true</scan:Duplex>";
        s << "</scan:ScanSettings>";
        const std::string settings = s.str();

        Vlog("eSCL: creating scan job...");
        HttpResult job = http.Request(u, L"POST", u.path + L"/ScanJobs", &settings, 30000);
        if ((job.status != 201 && job.status != 200) || job.location.empty()) throw JobFailure(http, u, job.status, "Creating the scan job");
        const std::wstring jobPath = JobPath(job.location);

        auto deleteJob = [&] { try { http.Request(u, L"DELETE", jobPath, nullptr, 5000); } catch (...) {} };
        std::vector<RawPage> pages;
        auto discard = [&] { std::error_code ec; for (auto& r : pages) fs::remove(r.file, ec); };
        try {
            auto lastOk = std::chrono::steady_clock::now();
            for (;;) {
                if ((p.pages > 0 && static_cast<int>(pages.size()) >= p.pages) || static_cast<int>(pages.size()) >= kMaxPages) break;
                HttpResult r = http.Request(u, L"GET", jobPath + L"/NextDocument", nullptr, recvMs);
                if (r.status == 200 && !r.body.empty()) {
                    fs::path file = tempNamer(pages.size() + 1);
                    std::ofstream out(file, std::ios::binary);
                    out.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
                    out.flush();
                    if (!out) throw ScanError("output_error", "Cannot write the temporary image file.");
                    pages.push_back({ file, container, post });
                    lastOk = std::chrono::steady_clock::now();
                    continue;
                }
                if (r.status == 404 || r.status == 410) break;
                if (r.status == 503) {
                    if (std::chrono::steady_clock::now() - lastOk > std::chrono::seconds(p.timeoutSec))
                        throw ScanError("timeout", "The scanner stayed busy for " + std::to_string(p.timeoutSec) + " seconds.");
                    Sleep(500);
                    continue;
                }
                throw JobFailure(http, u, r.status, "Retrieving the scanned page");
            }
            if (pages.empty()) throw JobFailure(http, u, 404, "Scanning");
        } catch (...) {
            deleteJob();
            discard();
            throw;
        }
        deleteJob();
        return pages;
    }

private:
    // Turn an HTTP failure into the most specific error, using the scanner's own status.
    static ScanError JobFailure(Http& http, const Url& u, DWORD status, const std::string& ctx) {
        try {
            HttpResult st = http.Request(u, L"GET", u.path + L"/ScannerStatus", nullptr, 8000);
            if (st.status == 200) {
                const std::string adf = xml::Text(st.body, "AdfState").value_or("");
                if (adf.find("Empty") != std::string::npos) return ScanError("paper_empty", "No paper in the document feeder.");
                if (adf.find("Jam") != std::string::npos) return ScanError("paper_jam", "Paper jam detected.");
                if (adf.find("Door") != std::string::npos) return ScanError("cover_open", "Scanner cover or door is open.");
                if (adf.find("Hung") != std::string::npos) return ScanError("paper_problem", "Paper problem reported by the scanner.");
            }
        } catch (...) {}
        if (status == 503 || status == 409) return ScanError("device_busy", "The scanner is busy (HTTP " + std::to_string(status) + ").");
        if (status == 400 || status == 415) return ScanError("unsupported_setting", "The scanner rejected the scan settings (HTTP " + std::to_string(status) + ").");
        if (status == 404) return ScanError("no_pages", "The scanner returned no image data.");
        return ScanError("scan_failed", ctx + " failed (HTTP " + std::to_string(status) + ").");
    }
};

}  // namespace esclbe

// -----------------------------------------------------------------------------
//  Command line
// -----------------------------------------------------------------------------
struct Options {
    std::string command;      // scan | list | version | help
    bool explicitHelp = false;
    ScanParams p;
    std::wstring out;
    std::set<std::string> backends = { "wia", "twain", "escl" };
};

static int ParseIntArg(const std::string& name, const std::string& v, int lo, int hi) {
    auto x = ToInt(v);
    if (!x || *x < lo || *x > hi)
        throw ScanError("invalid_argument", "--" + name + " must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
    return *x;
}

static double ParseDoubleArg(const std::string& name, const std::string& v, double lo, double hi) {
    double x = 0;
    std::string t = Trim(v);
    auto r = std::from_chars(t.data(), t.data() + t.size(), x);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size() || !(x >= lo && x <= hi))
        throw ScanError("invalid_argument", "--" + name + " contains an invalid number.");
    return x;
}

static Options ParseArgs(int argc, wchar_t** argv) {
    static const std::set<std::string> valueOpts = { "device", "backends", "mode", "dpi", "source", "format", "quality", "out", "area",
                                                     "brightness", "contrast", "threshold", "blank-skip", "timeout", "pages" };
    Options o;
    bool anyOption = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--help" || a == L"-h" || a == L"-help" || a == L"/?") { o.command = "help"; o.explicitHelp = true; continue; }
        if (a == L"--version") { o.command = "version"; continue; }
        if (a == L"--list")    { o.command = "list"; continue; }
        if (a == L"--verbose") { g_verbose = true; continue; }

        if (a.rfind(L"--", 0) != 0) {
            std::string w = ToLower(WideToUtf8(a));
            if (w == "scan" || w == "list" || w == "version" || w == "help") {
                o.command = w;
                if (w == "help") o.explicitHelp = true;
                continue;
            }
            throw ScanError("usage", "Unexpected argument '" + WideToUtf8(a) + "'. Run with 'help' for usage.");
        }

        std::wstring key = a.substr(2), val;
        bool hasVal = false;
        size_t eq = key.find(L'=');
        if (eq != std::wstring::npos) { val = key.substr(eq + 1); key.resize(eq); hasVal = true; }
        const std::string k = ToLower(WideToUtf8(key));
        if (!valueOpts.count(k)) throw ScanError("unknown_option", "Unknown option '--" + k + "'.");
        if (!hasVal) {
            if (i + 1 >= argc) throw ScanError("missing_value", "Option '--" + k + "' requires a value.");
            val = argv[++i];
        }
        anyOption = true;
        const std::string v = WideToUtf8(val);

        if (k == "device") {
            std::string d = Trim(v);
            if (d.empty() || d.size() > 512) throw ScanError("invalid_argument", "--device is empty or too long.");
            o.p.deviceId = ToLower(d) == "auto" ? "auto" : d;
        } else if (k == "backends") {
            std::set<std::string> b;
            for (auto& part : Split(ToLower(v), ',')) {
                std::string t = Trim(part);
                if (t != "wia" && t != "twain" && t != "escl") throw ScanError("invalid_argument", "--backends accepts wia, twain, escl (comma separated).");
                b.insert(t);
            }
            o.backends = b;
        } else if (k == "mode") {
            std::string m = ToLower(v);
            if (m == "grey") m = "gray";
            if (m != "color" && m != "gray" && m != "bw") throw ScanError("invalid_argument", "--mode must be color, gray or bw.");
            o.p.mode = m;
        } else if (k == "dpi") {
            o.p.dpi = ParseIntArg(k, v, 75, 1200);
        } else if (k == "source") {
            std::string s = ToLower(v);
            if (s != "auto" && s != "flatbed" && s != "adf" && s != "duplex") throw ScanError("invalid_argument", "--source must be auto, flatbed, adf or duplex.");
            o.p.source = s;
        } else if (k == "format") {
            std::string f = ToLower(v);
            if (f == "jpg") f = "jpeg";
            if (f == "tif") f = "tiff";
            if (f != "png" && f != "jpeg" && f != "bmp" && f != "tiff") throw ScanError("invalid_argument", "--format must be png, jpeg, bmp or tiff.");
            o.p.format = f;
        } else if (k == "quality") {
            o.p.quality = ParseIntArg(k, v, 1, 100);
        } else if (k == "out") {
            o.out = val;
        } else if (k == "area") {
            auto parts = Split(v, ',');
            if (parts.size() != 4) throw ScanError("invalid_argument", "--area must be left,top,width,height in millimetres.");
            for (size_t n = 0; n < 4; ++n) o.p.area[n] = ParseDoubleArg(k, parts[n], 0.0, 2000.0);
            if (o.p.area[2] <= 0 || o.p.area[3] <= 0) throw ScanError("invalid_argument", "--area width and height must be greater than zero.");
            o.p.hasArea = true;
        } else if (k == "brightness") { o.p.brightness = ParseIntArg(k, v, -1000, 1000); }
        else if (k == "contrast")     { o.p.contrast   = ParseIntArg(k, v, -1000, 1000); }
        else if (k == "threshold")    { o.p.threshold  = ParseIntArg(k, v, 0, 255); }
        else if (k == "blank-skip")   { o.p.blankSkip  = ParseDoubleArg(k, v, 0.0, 1.0); }
        else if (k == "pages")        { o.p.pages      = ParseIntArg(k, v, 0, kMaxPages); }
        else if (k == "timeout")      { o.p.timeoutSec = ParseIntArg(k, v, 5, 600); }
    }
    if (o.command.empty()) o.command = (argc > 1 && anyOption) ? "scan" : "help";
    return o;
}

static const char* HelpText() {
    return
        "Scanner API Gateway by RatioJuris\n"
        "(c) 2026 RatioJuris\n"
        "\n"
        "USAGE\n"
        "  ScannerAPIGateway.exe list [--backends wia,twain,escl]\n"
        "  ScannerAPIGateway.exe scan --out <dir\\ | file.ext> [options]\n"
        "  ScannerAPIGateway.exe version | help\n"
        "\n"
        "Backends: wia (Windows), twain (64-bit DSM), escl (network AirScan). Same options for all.\n"
        "\n"
        "OPTIONS (scan)\n"
        "  --device <id|auto>   id from 'list' (wia:..., twain:..., escl:http://ip:port/eSCL)\n"
        "  --backends <list>    restrict backends                       default: wia,twain,escl\n"
        "  --mode <color|gray|bw>                                       default: color\n"
        "  --dpi <75..1200>                                             default: 300\n"
        "  --source <auto|flatbed|adf|duplex>                           default: auto\n"
        "  --format <png|jpeg|bmp|tiff>   default: from --out extension, else png\n"
        "  --quality <1..100>   JPEG quality when the gateway encodes   default: 90\n"
        "  --out <path>         directory (trailing \\ or no extension) or file path\n"
        "  --area <l,t,w,h>     crop box in millimetres                 default: full\n"
        "  --brightness <-1000..1000>   hardware (WIA/TWAIN)\n"
        "  --contrast   <-1000..1000>   hardware (WIA/TWAIN)\n"
        "  --threshold  <0..255>        bw mode\n"
        "  --blank-skip <0.0..1.0>      drop pages whose dark-pixel ratio is <= value\n"
        "  --pages <0..1000>    0 = everything the feeder holds         default: 0\n"
        "  --timeout <5..600>   idle seconds before abort               default: 60\n"
        "  --verbose            progress on stderr\n"
        "\n"
        "OUTPUT\n"
        "  stdout: one JSON object. Exit codes: 0 ok, 1 failure, 2 bad arguments,\n"
        "  3 no scanner, 4 timeout, 5 scanner state (jam/empty/cover/busy), 6 backend unavailable.\n";
}

struct OutputPlan {
    PageNamer finalName;
    fs::path dir;
};

static OutputPlan PlanOutput(Options& o) {
    auto extToFormat = [](const std::wstring& e) -> std::string {
        std::string s = ToLower(WideToUtf8(e));
        if (s == ".png") return "png";
        if (s == ".jpg" || s == ".jpeg") return "jpeg";
        if (s == ".bmp") return "bmp";
        if (s == ".tif" || s == ".tiff") return "tiff";
        return "";
    };

    std::error_code ec;
    fs::path target;
    bool dirMode = true;
    if (o.out.empty()) {
        target = fs::current_path(ec);
    } else {
        const wchar_t last = o.out.back();
        target = fs::absolute(fs::path(o.out), ec);
        const bool endsWithSep = (last == L'\\' || last == L'/');
        if (endsWithSep || fs::is_directory(target, ec) || !target.has_extension()) {
            dirMode = true;
        } else {
            dirMode = false;
            const std::string fromExt = extToFormat(target.extension().wstring());
            if (fromExt.empty())
                throw ScanError("invalid_argument", "--out has an unsupported extension. Use .png, .jpg, .bmp or .tif (or end the path with \\ for a directory).");
            if (!o.p.format.empty() && o.p.format != fromExt)
                throw ScanError("invalid_argument", "--format '" + o.p.format + "' conflicts with the --out extension.");
            o.p.format = fromExt;
        }
    }
    if (o.p.format.empty()) o.p.format = "png";
    const std::wstring ext = ExtensionFor(o.p.format);

    OutputPlan plan;
    if (dirMode) {
        fs::create_directories(target, ec);
        if (ec || !fs::is_directory(target, ec)) throw ScanError("output_error", "Cannot create the output directory.");
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t stamp[48];
        swprintf_s(stamp, L"SCAN_%04d%02d%02d_%02d%02d%02d_%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        const std::wstring base = stamp;
        plan.dir = target;
        plan.finalName = [target, base, ext](size_t n) { return target / (base + L"_p" + Pad4(n) + ext); };
        return plan;
    }

    fs::path parent = target.parent_path();
    fs::create_directories(parent, ec);
    if (ec || !fs::is_directory(parent, ec)) throw ScanError("output_error", "Cannot create the output directory.");
    const std::wstring stem = target.stem().wstring();
    plan.dir = parent;
    plan.finalName = [parent, target, stem, ext](size_t n) { return n == 1 ? target : parent / (stem + L"_p" + Pad4(n) + ext); };
    return plan;
}

// -----------------------------------------------------------------------------
//  Device resolution (the same rules for every backend)
// -----------------------------------------------------------------------------
struct Resolved {
    IScanBackend* backend = nullptr;
    std::string nativeId, fullId;
};

static std::string StripSlashes(std::string s) {
    while (s.rfind("//", 0) == 0) s.erase(0, 2);
    return s;
}

static Resolved ResolveDevice(const std::string& id, BackendList& bes) {
    const std::string low = ToLower(id);

    // explicit prefix: wia: / twain: / escl:
    for (const char* known : { "wia", "twain", "escl" }) {
        const std::string pre = std::string(known) + ":";
        if (low.rfind(pre, 0) != 0) continue;
        for (auto& b : bes) {
            if (b->Name() != known) continue;
            std::string rest = id.substr(pre.size());
            if (b->Name() != "escl") rest = StripSlashes(rest);
            return { b.get(), rest, b->Name() + ":" + rest };
        }
        throw ScanError("invalid_argument", std::string("Backend '") + known + "' is disabled by --backends.");
    }

    std::string summary;
    for (auto& b : bes) {
        std::vector<DeviceInfo> devs;
        BackendStatus st = b->ListDevices(devs);
        summary += (summary.empty() ? "" : "; ") + b->Name() + ": " + (st.available ? std::to_string(devs.size()) + " device(s)" : "unavailable (" + st.message + ")");
        for (auto& d : devs) {
            if (id == "auto") {
                if (d.kind == "scanner") return { b.get(), d.nativeId, d.id };
            } else if (ToLower(d.nativeId) == low || ToLower(d.name) == low || ToLower(d.id) == low) {
                return { b.get(), d.nativeId, d.id };
            }
        }
    }
    if (id == "auto") throw ScanError("no_scanner", "No scanner was found (" + summary + ").");
    throw ScanError("device_not_found", "Device '" + id + "' was not found (" + summary + ").");
}

// -----------------------------------------------------------------------------
//  Commands
// -----------------------------------------------------------------------------
static int CmdVersion() {
    EmitJson(std::string("{\"ok\":true,\"contract_version\":") + std::to_string(kContractVersion) + ",\"command\":\"version\",\"name\":" + JStr(kProductName) +
             ",\"vendor\":" + JStr(kVendor) + ",\"version\":" + JStr(kVersion) + ",\"copyright\":" + JStr(kCopyright) +
             ",\"backends\":[\"wia\",\"twain\",\"escl\"],\"arch\":\"x64\"}");
    return 0;
}

static int CmdList(BackendList& bes) {
    std::vector<DeviceInfo> devs;
    std::vector<BackendStatus> statuses;
    for (auto& b : bes) statuses.push_back(b->ListDevices(devs));

    std::string j = "{\"ok\":true,\"contract_version\":" + std::to_string(kContractVersion) + ",\"command\":\"list\",\"devices\":[";
    for (size_t i = 0; i < devs.size(); ++i) {
        const auto& d = devs[i];
        if (i) j += ',';
        j += "{\"id\":" + JStr(d.id) + ",\"name\":" + JStr(d.name) + ",\"description\":" + JStr(d.description) +
             ",\"manufacturer\":" + JStr(d.manufacturer) + ",\"kind\":" + JStr(d.kind) + ",\"backend\":" + JStr(d.backend) + "}";
    }
    j += "],\"backends\":[";
    for (size_t i = 0; i < statuses.size(); ++i) {
        if (i) j += ',';
        j += "{\"name\":" + JStr(statuses[i].name) + ",\"available\":" + (statuses[i].available ? "true" : "false");
        if (!statuses[i].message.empty()) j += ",\"message\":" + JStr(statuses[i].message);
        j += "}";
    }
    j += "]}";
    EmitJson(j);
    return 0;
}

static int CmdScan(BackendList& bes, Options& o) {
    const auto t0 = std::chrono::steady_clock::now();
    OutputPlan plan = PlanOutput(o);
    std::vector<std::string> warnings;

    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    PageNamer tempNamer = [dir = plan.dir, token](size_t n) { return dir / (L".sagtmp_" + token + L"_" + Pad4(n) + L".part"); };

    Resolved dev;
    std::vector<RawPage> raw;
    {
        ScanLock lock;
        dev = ResolveDevice(o.p.deviceId, bes);
        Vlog("Using " + dev.fullId);
        raw = dev.backend->Scan(o.p, dev.nativeId, tempNamer, warnings);
    }

    int skipped = 0;
    std::vector<fs::path> pages = img::FinalizePages(raw, plan.finalName, o.p, warnings, skipped);
    if (skipped > 0) warnings.push_back(std::to_string(skipped) + " blank page(s) were discarded.");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::ostringstream j;
    j << "{\"ok\":true,\"contract_version\":" << kContractVersion << ",\"command\":\"scan\",\"backend\":" << JStr(dev.backend->Name())
      << ",\"device\":" << JStr(dev.fullId)
      << ",\"settings\":{\"mode\":" << JStr(o.p.mode) << ",\"dpi\":" << o.p.dpi << ",\"source\":" << JStr(o.p.source)
      << ",\"format\":" << JStr(o.p.format) << "},\"page_count\":" << pages.size() << ",\"skipped_blank_pages\":" << skipped << ",\"pages\":[";
    for (size_t i = 0; i < pages.size(); ++i) {
        std::error_code ec;
        uintmax_t sz = fs::file_size(pages[i], ec);
        if (i) j << ',';
        j << "{\"index\":" << (i + 1) << ",\"path\":" << JStr(WideToUtf8(pages[i].wstring()))
          << ",\"content_type\":" << JStr(ContentTypeFor(o.p.format)) << ",\"bytes\":" << (ec ? 0 : sz) << "}";
    }
    j << "],\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i) { if (i) j << ','; j << JStr(warnings[i]); }
    j << "],\"elapsed_ms\":" << ms << "}";
    EmitJson(j.str());
    return 0;
}

static int EmitError(const std::string& code, const std::string& message, HRESULT hr) {
    std::string b = "{\"ok\":false,\"contract_version\":" + std::to_string(kContractVersion) + ",\"error\":{\"code\":" + JStr(code) + ",\"message\":" + JStr(message);
    if (FAILED(hr)) b += ",\"hresult\":" + JStr(HrHex(hr));
    b += "}}";
    EmitJson(b);
    if (g_verbose) std::fputs(("[gateway] error: " + code + ": " + message + "\n").c_str(), stderr);
    return ExitCodeFor(code);
}

// If started with a console of our own (parent is a GUI app) but stdout is
// redirected, drop the console so nothing ever flashes on screen.
static void DetachStrayConsole() {
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) != 1) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) FreeConsole();
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _setmode(_fileno(stdout), _O_BINARY);
    DetachStrayConsole();

    try {
        Options o = ParseArgs(argc, argv);
        if (o.command == "help") {
            std::fputs(HelpText(), stdout);
            std::fflush(stdout);
            return o.explicitHelp ? 0 : 2;
        }
        if (o.command == "version") return CmdVersion();

        BackendList bes;
        if (o.backends.count("wia"))   bes.push_back(std::make_unique<wiabe::WiaBackend>());
        if (o.backends.count("twain")) bes.push_back(std::make_unique<twainbe::TwainBackend>());
        if (o.backends.count("escl"))  bes.push_back(std::make_unique<esclbe::EsclBackend>());

        if (o.command == "list") return CmdList(bes);
        return CmdScan(bes, o);
    } catch (const ScanError& e) {
        return EmitError(e.code, e.what(), e.hr);
    } catch (const std::exception& e) {
        return EmitError("internal_error", e.what(), S_OK);
    } catch (...) {
        return EmitError("internal_error", "Unknown fatal error.", S_OK);
    }
}
