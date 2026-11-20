// =============================================================================
//  Scanner API Gateway by RatioJuris - POSIX build (Linux / macOS)
//  © 2026 RatioJuris
//  Version 1.2.0
//
//  This is the wire-compatible counterpart of ScannerAPIGateway.cpp. It keeps
//  the same CLI, the same single-JSON-object stdout contract and the same exit
//  codes. WIA and TWAIN do not exist off Windows, so this build exposes the
//  escl (AirScan) backend only; use a Windows build for WIA/TWAIN hardware.
//
//  Dependencies (installed by the CI workflow):
//    libcurl, plus the vendored header-only stb_image / stb_image_write.
// =============================================================================

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

static constexpr char kProductName[] = "Scanner API Gateway by RatioJuris";
static constexpr char kVendor[]      = "RatioJuris";
static constexpr char kCopyright[]   = "© 2026 RatioJuris";
static constexpr char kVersion[]     = "1.2.0";
static constexpr char kVersionShort[] = "1.2";
static constexpr int  kContractVersion = 1;
static constexpr int  kMaxPages = 1000;
static constexpr int  kEsclDiscoverMs = 2000;

static const char* ArchName() {
#if defined(__x86_64__) || defined(__amd64__)
    return "x64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#else
    return "x86";
#endif
}

// -----------------------------------------------------------------------------
//  Utilities
// -----------------------------------------------------------------------------
static bool g_verbose = false;
static void Vlog(const std::string& m) {
    if (g_verbose) { std::fprintf(stderr, "[gateway] %s\n", m.c_str()); std::fflush(stderr); }
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
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
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

static std::string Pad4(size_t n) {
    char b[24];
    std::snprintf(b, sizeof b, "%04zu", n);
    return b;
}

// -----------------------------------------------------------------------------
//  Errors and exit codes (identical to the Windows build)
// -----------------------------------------------------------------------------
class ScanError : public std::runtime_error {
public:
    ScanError(std::string code, const std::string& msg) : std::runtime_error(msg), code(std::move(code)) {}
    std::string code;
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

class ScanLock {
public:
    ScanLock() {
        fd_ = ::open("/tmp/ScannerAPIGateway.lock", O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return;  // best effort; scanning still works without the lock
        if (::flock(fd_, LOCK_EX) != 0) { ::close(fd_); fd_ = -1; throw ScanError("busy", "Another scan is in progress."); }
    }
    ~ScanLock() { if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); } }
    ScanLock(const ScanLock&) = delete;
    ScanLock& operator=(const ScanLock&) = delete;
private:
    int fd_ = -1;
};

// -----------------------------------------------------------------------------
//  Shared model + backend interface
// -----------------------------------------------------------------------------
struct DeviceInfo {
    std::string id, nativeId, name, description, manufacturer, kind, backend;
};
struct BackendStatus {
    std::string name;
    bool available = true;
    std::string message;
};
struct ScanParams {
    std::string deviceId = "auto";
    std::string mode = "color";
    int dpi = 300;
    std::string source = "auto";
    std::string format;
    int quality = 90;
    bool hasArea = false;
    double area[4] = { 0, 0, 0, 0 };
    std::optional<int> brightness, contrast, threshold;
    double blankSkip = 0.0;
    int pages = 0;
    int timeoutSec = 60;
};
enum class PostMode { None, Gray, Bw };
struct RawPage {
    fs::path file;
    std::string container;
    PostMode post = PostMode::None;
};
using PageNamer = std::function<fs::path(size_t)>;

class IScanBackend {
public:
    virtual ~IScanBackend() = default;
    virtual std::string Name() const = 0;
    virtual BackendStatus ListDevices(std::vector<DeviceInfo>& out) = 0;
    virtual std::vector<RawPage> Scan(const ScanParams& p, const std::string& nativeId,
                                      const PageNamer& tempNamer, std::vector<std::string>& warnings) = 0;
};
using BackendList = std::vector<std::unique_ptr<IScanBackend>>;

static std::string ExtensionFor(const std::string& fmt) {
    if (fmt == "jpeg") return ".jpg";
    if (fmt == "bmp")  return ".bmp";
    if (fmt == "tiff") return ".tif";
    return ".png";
}
static const char* ContentTypeFor(const std::string& fmt) {
    if (fmt == "jpeg") return "image/jpeg";
    if (fmt == "bmp")  return "image/bmp";
    if (fmt == "tiff") return "image/tiff";
    return "image/png";
}

// -----------------------------------------------------------------------------
//  Image pipeline (stb + a small baseline TIFF writer)
// -----------------------------------------------------------------------------
namespace img {

static void Put16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
}
static void Put32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

// Uncompressed, single-strip, little-endian baseline TIFF. Enough for scanned
// 8-bit gray and 24-bit RGB pages without pulling in libtiff.
static void WriteTiff(const fs::path& file, const std::vector<unsigned char>& px, int w, int h, int channels) {
    const uint32_t dataOff = 8;
    uint32_t ifdOff = dataOff + static_cast<uint32_t>(px.size());
    if (ifdOff & 1u) ++ifdOff;
    const int entries = 10;
    const uint32_t bpsOff = ifdOff + 2 + static_cast<uint32_t>(entries) * 12 + 4;

    std::string out;
    out.reserve(ifdOff + 64);
    out += "II";
    Put16(out, 42);
    Put32(out, ifdOff);
    out.append(reinterpret_cast<const char*>(px.data()), px.size());
    while (out.size() < ifdOff) out.push_back('\0');

    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        Put16(out, tag); Put16(out, type); Put32(out, count); Put32(out, value);
    };
    Put16(out, static_cast<uint16_t>(entries));
    entry(256, 4, 1, static_cast<uint32_t>(w));                 // ImageWidth
    entry(257, 4, 1, static_cast<uint32_t>(h));                 // ImageLength
    entry(258, 3, static_cast<uint32_t>(channels), channels == 1 ? 8u : bpsOff);
    entry(259, 3, 1, 1);                                        // Compression = none
    entry(262, 3, 1, channels == 1 ? 1u : 2u);                  // BlackIsZero / RGB
    entry(273, 4, 1, dataOff);                                  // StripOffsets
    entry(277, 3, 1, static_cast<uint32_t>(channels));          // SamplesPerPixel
    entry(278, 4, 1, static_cast<uint32_t>(h));                 // RowsPerStrip
    entry(279, 4, 1, static_cast<uint32_t>(px.size()));         // StripByteCounts
    entry(284, 3, 1, 1);                                        // PlanarConfiguration
    Put32(out, 0);                                              // no next IFD
    if (channels > 1) for (int i = 0; i < channels; ++i) Put16(out, 8);

    std::ofstream f(file, std::ios::binary);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    f.flush();
    if (!f) throw ScanError("output_error", "Cannot write the TIFF output file.");
}

static void WriteImage(const fs::path& file, const std::string& fmt, const std::vector<unsigned char>& px,
                       int w, int h, int channels, int quality) {
    int ok = 0;
    if (fmt == "png")       ok = stbi_write_png(file.c_str(), w, h, channels, px.data(), w * channels);
    else if (fmt == "jpeg") ok = stbi_write_jpg(file.c_str(), w, h, channels, px.data(), quality);
    else if (fmt == "bmp")  ok = stbi_write_bmp(file.c_str(), w, h, channels, px.data());
    else if (fmt == "tiff") { WriteTiff(file, px, w, h, channels); ok = 1; }
    else throw ScanError("unsupported_format", "Unsupported output format '" + fmt + "'.");
    if (!ok) throw ScanError("convert_failed", "Cannot encode the image as " + fmt + ".");
}

static std::vector<fs::path> FinalizePages(std::vector<RawPage>& raw, const PageNamer& finalName, const ScanParams& p,
                                           std::vector<std::string>& warnings, int& skipped) {
    std::vector<fs::path> done;
    auto cleanup = [&] {
        std::error_code ec;
        for (auto& r : raw) fs::remove(r.file, ec);
        for (auto& d : done) fs::remove(d, ec);
    };
    try {
        for (auto& r : raw) {
            bool blank = false;
            if (p.blankSkip > 0.0) {
                int w = 0, h = 0, c = 0;
                unsigned char* gray = stbi_load(r.file.c_str(), &w, &h, &c, 1);
                if (gray && w > 0 && h > 0) {
                    const size_t m = std::max<size_t>(1, static_cast<size_t>(std::min(w, h)) * 3 / 100);
                    size_t dark = 0, total = 0;
                    for (int y = static_cast<int>(m); y < h - static_cast<int>(m); ++y)
                        for (int x = static_cast<int>(m); x < w - static_cast<int>(m); ++x) {
                            if (gray[static_cast<size_t>(y) * w + x] < 128) ++dark;
                            ++total;
                        }
                    if (total) blank = (static_cast<double>(dark) / static_cast<double>(total)) <= p.blankSkip;
                    stbi_image_free(gray);
                } else {
                    if (gray) stbi_image_free(gray);
                    warnings.push_back("Blank-page detection skipped: cannot decode the page.");
                }
            }
            if (blank) {
                std::error_code ec;
                fs::remove(r.file, ec);
                ++skipped;
                continue;
            }

            const fs::path dst = finalName(done.size() + 1);
            if (r.container == p.format && r.post == PostMode::None) {
                std::error_code ec;
                fs::rename(r.file, dst, ec);
                if (ec) {
                    fs::copy_file(r.file, dst, fs::copy_options::overwrite_existing, ec);
                    if (ec) throw ScanError("output_error", "Cannot write the output file.");
                    fs::remove(r.file, ec);
                }
            } else {
                const int req = (r.post == PostMode::None) ? 0 : 1;
                int w = 0, h = 0, c = 0;
                unsigned char* decoded = stbi_load(r.file.c_str(), &w, &h, &c, req);
                if (!decoded) throw ScanError("convert_failed", "Cannot decode the scanned page.");
                const int channels = req == 1 ? 1 : c;
                std::vector<unsigned char> px(decoded, decoded + static_cast<size_t>(w) * h * channels);
                stbi_image_free(decoded);
                if (r.post == PostMode::Bw) {
                    const int th = p.threshold.value_or(128);
                    for (auto& v : px) v = (v >= th) ? 255 : 0;
                }
                WriteImage(dst, p.format, px, w, h, channels, p.quality);
                std::error_code ec;
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
//  eSCL / AirScan backend
// -----------------------------------------------------------------------------
namespace escl {

// ---- tiny XML helpers (namespace prefixes ignored) ----
static std::string XmlLocal(std::string tag) {
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
        if (XmlLocal(x.substr(p + 1, q - p - 1)) == name) return p;
    }
    return std::string::npos;
}
static std::optional<std::string> XmlText(const std::string& x, const std::string& name, size_t from = 0) {
    size_t p = FindOpen(x, name, from);
    if (p == std::string::npos) return std::nullopt;
    size_t gt = x.find('>', p);
    if (gt == std::string::npos) return std::nullopt;
    if (gt > 0 && x[gt - 1] == '/') return std::string();
    const size_t start = gt + 1;
    for (size_t c = x.find("</", start); c != std::string::npos; c = x.find("</", c + 1)) {
        size_t q = x.find('>', c);
        if (q == std::string::npos) break;
        if (XmlLocal(x.substr(c + 2, q - c - 2)) == name) return Trim(x.substr(start, c - start));
    }
    return std::nullopt;
}

// ---- URLs ----
struct Url { std::string scheme = "http", host, port = "80", path = "/eSCL"; };

static Url ParseUrl(const std::string& in) {
    std::string n = Trim(in);
    if (n.rfind("http://", 0) != 0 && n.rfind("https://", 0) != 0) {
        while (n.rfind("//", 0) == 0) n.erase(0, 2);
        n = "http://" + n;
    }
    size_t sep = n.find("://");
    Url u;
    u.scheme = n.substr(0, sep);
    const size_t start = sep + 3;
    size_t slash = n.find('/', start);
    std::string authority = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    u.path = slash == std::string::npos ? "/eSCL" : n.substr(slash);
    while (u.path.size() > 1 && u.path.back() == '/') u.path.pop_back();
    if (u.path.empty() || u.path == "/") u.path = "/eSCL";
    size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        u.host = authority;
        u.port = u.scheme == "https" ? "443" : "80";
    } else {
        u.host = authority.substr(0, colon);
        u.port = authority.substr(colon + 1);
    }
    return u;
}
static std::string Origin(const Url& u) { return u.scheme + "://" + u.host + ":" + u.port; }
static std::string JobPath(const std::string& location) {
    std::string p = location;
    size_t sep = p.find("://");
    if (sep != std::string::npos) {
        size_t slash = p.find('/', sep + 3);
        p = slash == std::string::npos ? "/" : p.substr(slash);
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// ---- HTTP (libcurl) ----
struct HttpResult { long status = 0; std::string location, body; };

class Http {
public:
    Http() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ = curl_easy_init();
        if (!curl_) { curl_global_cleanup(); throw ScanError("escl_unavailable", "libcurl is unavailable."); }
        const std::string ua = std::string("ScannerAPIGateway/") + kVersionShort;
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, ua.c_str());
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);   // LAN devices use self-signed certs
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    }
    ~Http() {
        if (curl_) curl_easy_cleanup(curl_);
        curl_global_cleanup();
    }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResult Request(const std::string& url, const std::string& method, const std::string* body, int timeoutSec) {
        HttpResult r;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, static_cast<long>(std::max(5, timeoutSec)));
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Http::WriteCb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &Http::HeaderCb);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &r);

        curl_slist* headers = nullptr;
        if (body) {
            headers = curl_slist_append(headers, "Content-Type: text/xml");
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body->data());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
        } else {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L);
        }

        CURLcode rc = curl_easy_perform(curl_);
        if (headers) curl_slist_free_all(headers);
        if (rc != CURLE_OK) {
            if (rc == CURLE_OPERATION_TIMEDOUT) throw ScanError("timeout", "The network scanner did not respond in time.");
            throw ScanError("device_communication", std::string("HTTP request failed: ") + curl_easy_strerror(rc));
        }
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
        return r;
    }

private:
    static size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* s = static_cast<std::string*>(userdata);
        s->append(ptr, size * nmemb);
        return size * nmemb;
    }
    static size_t HeaderCb(char* buffer, size_t size, size_t n, void* userdata) {
        auto* r = static_cast<HttpResult*>(userdata);
        std::string line(buffer, size * n);
        std::string low = ToLower(line);
        if (low.rfind("http/", 0) == 0) {
            std::istringstream is(line);
            std::string proto;
            long code = 0;
            if (is >> proto >> code) r->status = code;
        } else if (low.rfind("location:", 0) == 0) {
            size_t colon = line.find(':');
            std::string v = Trim(line.substr(colon + 1));
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
            r->location = v;
        }
        return size * n;
    }
    CURL* curl_ = nullptr;
};

// ---- mDNS discovery (_uscan._tcp / _uscans._tcp) ----
struct Found { std::string name, note, ip, rs; unsigned port = 80; bool tls = false; };
struct MdnsState {
    std::map<std::string, std::pair<std::string, bool>> ptr;
    std::map<std::string, std::pair<std::string, uint16_t>> srv;
    std::map<std::string, std::map<std::string, std::string>> txt;
    std::map<std::string, std::string> a;
    std::map<std::string, std::string> sender;
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
        if (type == 12) {
            std::string target; size_t e2 = 0;
            if (ReadName(d, n, rdata, target, e2) &&
                (ownerLow == "_uscan._tcp.local" || ownerLow == "_uscans._tcp.local")) {
                st.ptr[ToLower(target)] = { target, ownerLow == "_uscans._tcp.local" };
                st.sender.emplace(ToLower(target), from);
            }
        } else if (type == 33 && rdlen >= 6) {
            std::string target; size_t e2 = 0;
            if (ReadName(d, n, rdata + 6, target, e2)) {
                st.srv[ownerLow] = { ToLower(target), rd16(rdata + 4) };
                st.sender.emplace(ownerLow, from);
            }
        } else if (type == 16) {
            size_t p = rdata;
            while (p < rdata + rdlen) {
                size_t l = d[p++];
                if (p + l > rdata + rdlen) break;
                std::string kv(reinterpret_cast<const char*>(d) + p, l);
                p += l;
                size_t eq = kv.find('=');
                if (eq != std::string::npos) st.txt[ownerLow][ToLower(kv.substr(0, eq))] = kv.substr(eq + 1);
            }
        } else if (type == 1 && rdlen == 4) {
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
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* cur = ifa; cur; cur = cur->ifa_next) {
        if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET) continue;
        if (!(cur->ifa_flags & IFF_UP)) continue;
        char ip[INET_ADDRSTRLEN] = { 0 };
        sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(cur->ifa_addr);
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip)) out.push_back(ip);
    }
    freeifaddrs(ifa);
    return out;
}

static std::vector<Found> Discover(int waitMs) {
    std::vector<Found> result;
    std::vector<int> socks;
    for (const std::string& ip : LocalIPv4()) {
        if (socks.size() >= 32) break;
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) continue;
        int on = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        a.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(s); continue; }
        ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a.sin_addr, sizeof a.sin_addr);
        int ttl = 255;
        ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        socks.push_back(s);
    }
    if (socks.empty()) return result;

    std::vector<uint8_t> query = { 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0 };
    for (const char* svc : { "_uscan._tcp.local", "_uscans._tcp.local" }) {
        for (const std::string& label : Split(svc, '.')) {
            query.push_back(static_cast<uint8_t>(label.size()));
            query.insert(query.end(), label.begin(), label.end());
        }
        query.push_back(0);
        query.push_back(0); query.push_back(12);
        query.push_back(0x80); query.push_back(0x01);
    }
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    auto sendQuery = [&] {
        for (int s : socks)
            ::sendto(s, reinterpret_cast<const char*>(query.data()), query.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof dst);
    };
    sendQuery();

    MdnsState st;
    const auto t0 = std::chrono::steady_clock::now();
    bool resent = false;
    for (;;) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= waitMs) break;
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int s : socks) { FD_SET(s, &rf); maxfd = std::max(maxfd, s); }
        timeval tv{ 0, 100000 };
        int r = ::select(maxfd + 1, &rf, nullptr, nullptr, &tv);
        if (r > 0) {
            for (int s : socks) {
                if (!FD_ISSET(s, &rf)) continue;
                uint8_t buf[9000];
                sockaddr_in from = {};
                socklen_t fl = sizeof from;
                ssize_t n = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                       reinterpret_cast<sockaddr*>(&from), &fl);
                if (n > 0) {
                    char ip[INET_ADDRSTRLEN] = { 0 };
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                    ParsePacket(buf, static_cast<size_t>(n), ip, st);
                }
            }
        }
        if (!resent && elapsed > 700) { sendQuery(); resent = true; }
    }
    for (int s : socks) ::close(s);

    for (auto& kv : st.ptr) {
        auto srv = st.srv.find(kv.first);
        if (srv == st.srv.end()) continue;
        Found f;
        auto aIt = st.a.find(srv->second.first);
        if (aIt != st.a.end()) f.ip = aIt->second;
        else {
            auto sd = st.sender.find(kv.first);
            if (sd != st.sender.end()) f.ip = sd->second;
        }
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

static ScanError JobFailure(Http& http, const Url& u, long status, const std::string& ctx) {
    try {
        HttpResult st = http.Request(Origin(u) + u.path + "/ScannerStatus", "GET", nullptr, 8);
        if (st.status == 200) {
            const std::string adf = XmlText(st.body, "AdfState").value_or("");
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

class EsclBackend : public IScanBackend {
public:
    std::string Name() const override { return "escl"; }

    BackendStatus ListDevices(std::vector<DeviceInfo>& out) override {
        BackendStatus st;
        st.name = "escl";
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
        const Url u = ParseUrl(nativeId);
        Http http;

        HttpResult caps = http.Request(Origin(u) + u.path + "/ScannerCapabilities", "GET", nullptr, p.timeoutSec);
        if (caps.status != 200)
            throw ScanError("device_communication", "The scanner did not return its capabilities (HTTP " + std::to_string(caps.status) + ").");
        const std::string& cx = caps.body;

        const bool hasPlaten = FindOpen(cx, "Platen", 0) != std::string::npos;
        const bool hasAdf = FindOpen(cx, "Adf", 0) != std::string::npos;
        const bool wantFeeder = (p.source == "adf" || p.source == "duplex");
        std::string input = "Platen", region = "PlatenInputCaps";
        if (wantFeeder || (p.source == "auto" && !hasPlaten && hasAdf)) {
            if (!hasAdf) throw ScanError("source_not_available", "No document feeder available.");
            input = "Feeder";
            if (p.source == "duplex") {
                if (FindOpen(cx, "AdfDuplexInputCaps", 0) == std::string::npos)
                    throw ScanError("source_not_available", "Duplex scanning is not supported by this device.");
                region = "AdfDuplexInputCaps";
            } else region = "AdfSimplexInputCaps";
        } else if (p.source == "flatbed" && !hasPlaten && hasAdf) {
            throw ScanError("source_not_available", "No flatbed available on this device.");
        }
        std::string caps_region = XmlText(cx, region).value_or(XmlText(cx, input == "Platen" ? "Platen" : "Adf").value_or(cx));

        int maxW = ToInt(XmlText(caps_region, "MaxWidth").value_or("")).value_or(2550);
        int maxH = ToInt(XmlText(caps_region, "MaxHeight").value_or("")).value_or(3507);
        int xo = 0, yo = 0, wid = maxW, hgt = maxH;
        if (p.hasArea) {
            auto u300 = [](double mm) { return static_cast<int>(std::lround(mm / 25.4 * 300.0)); };
            xo = u300(p.area[0]); yo = u300(p.area[1]); wid = u300(p.area[2]); hgt = u300(p.area[3]);
            if (xo + wid > maxW || yo + hgt > maxH)
                throw ScanError("unsupported_setting", "The scan area exceeds the scanner's maximum.");
        }

        int dpi = p.dpi;
        std::vector<int> supported;
        for (size_t pos = 0; (pos = FindOpen(caps_region, "XResolution", pos)) != std::string::npos; ++pos) {
            auto t = XmlText(caps_region, "XResolution", pos);
            if (t && t->find('<') == std::string::npos) if (auto v = ToInt(*t)) supported.push_back(*v);
        }
        if (!supported.empty() && std::find(supported.begin(), supported.end(), dpi) == supported.end()) {
            int best = supported.front();
            for (int v : supported) if (std::abs(v - dpi) < std::abs(best - dpi)) best = v;
            warnings.push_back("Resolution " + std::to_string(dpi) + " dpi is not supported; used " + std::to_string(best) + " dpi.");
            dpi = best;
        }

        const bool hasGray = cx.find("Grayscale8") != std::string::npos;
        std::string color = "RGB24";
        PostMode post = PostMode::None;
        if (p.mode == "gray") { if (hasGray) color = "Grayscale8"; else post = PostMode::Gray; }
        else if (p.mode == "bw") { color = hasGray ? "Grayscale8" : "RGB24"; post = PostMode::Bw; }

        std::string mime = "image/jpeg", container = "jpeg";
        if (cx.find("image/jpeg") == std::string::npos) {
            if (cx.find("image/png") != std::string::npos) { mime = "image/png"; container = "png"; }
            else if (cx.find("application/pdf") != std::string::npos)
                throw ScanError("unsupported_format", "This scanner can only deliver PDF over eSCL.");
        }
        if (p.brightness || p.contrast)
            warnings.push_back("Brightness/contrast are hardware settings and are not available over eSCL; ignored.");

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
        HttpResult job = http.Request(Origin(u) + u.path + "/ScanJobs", "POST", &settings, 30);
        if ((job.status != 201 && job.status != 200) || job.location.empty())
            throw JobFailure(http, u, job.status, "Creating the scan job");
        const std::string jobPath = JobPath(job.location);

        auto deleteJob = [&] { try { http.Request(Origin(u) + jobPath, "DELETE", nullptr, 5); } catch (...) {} };
        std::vector<RawPage> pages;
        auto discard = [&] { std::error_code ec; for (auto& r : pages) fs::remove(r.file, ec); };
        try {
            auto lastOk = std::chrono::steady_clock::now();
            for (;;) {
                if ((p.pages > 0 && static_cast<int>(pages.size()) >= p.pages) ||
                    static_cast<int>(pages.size()) >= kMaxPages) break;
                HttpResult r = http.Request(Origin(u) + jobPath + "/NextDocument", "GET", nullptr, p.timeoutSec);
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
                    ::usleep(500000);
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
};

}  // namespace escl

// -----------------------------------------------------------------------------
//  Command line
// -----------------------------------------------------------------------------
struct Options {
    std::string command;
    bool explicitHelp = false;
    bool backendsExplicit = false;
    ScanParams p;
    std::string out;
    std::set<std::string> backends = { "wia", "twain", "escl" };
};

static int ParseIntArg(const std::string& name, const std::string& v, int lo, int hi) {
    auto x = ToInt(v);
    if (!x || *x < lo || *x > hi)
        throw ScanError("invalid_argument", "--" + name + " must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
    return *x;
}
static double ParseDoubleArg(const std::string& name, const std::string& v, double lo, double hi) {
    const std::string t = Trim(v);
    if (t.empty()) throw ScanError("invalid_argument", "--" + name + " contains an invalid number.");
    char* end = nullptr;
    errno = 0;
    double x = std::strtod(t.c_str(), &end);
    if (errno == ERANGE || end != t.c_str() + t.size() || !(x >= lo && x <= hi))
        throw ScanError("invalid_argument", "--" + name + " contains an invalid number.");
    return x;
}

static Options ParseArgs(int argc, char** argv) {
    static const std::set<std::string> valueOpts = { "device", "backends", "mode", "dpi", "source", "format", "quality", "out", "area",
                                                      "brightness", "contrast", "threshold", "blank-skip", "timeout", "pages" };
    Options o;
    bool anyOption = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h" || a == "-help" || a == "/?") { o.command = "help"; o.explicitHelp = true; continue; }
        if (a == "--version") { o.command = "version"; continue; }
        if (a == "--list")    { o.command = "list"; continue; }
        if (a == "--verbose") { g_verbose = true; continue; }

        if (a.rfind("--", 0) != 0) {
            std::string w = ToLower(a);
            if (w == "scan" || w == "list" || w == "version" || w == "help") {
                o.command = w;
                if (w == "help") o.explicitHelp = true;
                continue;
            }
            throw ScanError("usage", "Unexpected argument '" + a + "'. Run with 'help' for usage.");
        }

        std::string key = a.substr(2), val;
        bool hasVal = false;
        size_t eq = key.find('=');
        if (eq != std::string::npos) { val = key.substr(eq + 1); key.resize(eq); hasVal = true; }
        const std::string k = ToLower(key);
        if (!valueOpts.count(k)) throw ScanError("unknown_option", "Unknown option '--" + k + "'.");
        if (!hasVal) {
            if (i + 1 >= argc || std::string_view(argv[i + 1]).substr(0, 2) == "--")
                throw ScanError("missing_value", "Option '--" + k + "' requires a value.");
            val = argv[++i];
        }
        anyOption = true;

        if (k == "device") {
            std::string d = Trim(val);
            if (d.empty() || d.size() > 512) throw ScanError("invalid_argument", "--device is empty or too long.");
            o.p.deviceId = ToLower(d) == "auto" ? "auto" : d;
        } else if (k == "backends") {
            o.backendsExplicit = true;
            std::set<std::string> b;
            for (auto& part : Split(ToLower(val), ',')) {
                std::string t = Trim(part);
                if (t != "wia" && t != "twain" && t != "escl") throw ScanError("invalid_argument", "--backends accepts wia, twain, escl (comma separated).");
                b.insert(t);
            }
            o.backends = b;
        } else if (k == "mode") {
            std::string m = ToLower(val);
            if (m == "grey") m = "gray";
            if (m != "color" && m != "gray" && m != "bw") throw ScanError("invalid_argument", "--mode must be color, gray or bw.");
            o.p.mode = m;
        } else if (k == "dpi") o.p.dpi = ParseIntArg(k, val, 75, 1200);
        else if (k == "source") {
            std::string s = ToLower(val);
            if (s != "auto" && s != "flatbed" && s != "adf" && s != "duplex") throw ScanError("invalid_argument", "--source must be auto, flatbed, adf or duplex.");
            o.p.source = s;
        } else if (k == "format") {
            std::string f = ToLower(val);
            if (f == "jpg") f = "jpeg";
            if (f == "tif") f = "tiff";
            if (f != "png" && f != "jpeg" && f != "bmp" && f != "tiff") throw ScanError("invalid_argument", "--format must be png, jpeg, bmp or tiff.");
            o.p.format = f;
        } else if (k == "quality") o.p.quality = ParseIntArg(k, val, 1, 100);
        else if (k == "out") o.out = val;
        else if (k == "area") {
            auto parts = Split(val, ',');
            if (parts.size() != 4) throw ScanError("invalid_argument", "--area must be left,top,width,height in millimetres.");
            for (size_t n = 0; n < 4; ++n) o.p.area[n] = ParseDoubleArg(k, parts[n], 0.0, 2000.0);
            if (o.p.area[2] <= 0 || o.p.area[3] <= 0) throw ScanError("invalid_argument", "--area width and height must be greater than zero.");
            o.p.hasArea = true;
        }
        else if (k == "brightness") { o.p.brightness = ParseIntArg(k, val, -1000, 1000); }
        else if (k == "contrast")   { o.p.contrast   = ParseIntArg(k, val, -1000, 1000); }
        else if (k == "threshold")  { o.p.threshold  = ParseIntArg(k, val, 0, 255); }
        else if (k == "blank-skip") { o.p.blankSkip  = ParseDoubleArg(k, val, 0.0, 1.0); }
        else if (k == "pages")      { o.p.pages      = ParseIntArg(k, val, 0, kMaxPages); }
        else if (k == "timeout")    { o.p.timeoutSec = ParseIntArg(k, val, 5, 600); }
    }
    if (o.command.empty()) o.command = (argc > 1 && anyOption) ? "scan" : "help";
    return o;
}

static std::string HelpText() {
    return std::string("Scanner API Gateway v") + kVersion + " by RatioJuris\n"
        "© 2026 RatioJuris\n"
        "\n"
        "USAGE\n"
        "  ScannerAPIGateway list [--backends wia,twain,escl]\n"
        "  ScannerAPIGateway scan --out <dir/ | file.ext> [options]\n"
        "  ScannerAPIGateway version | help\n"
        "\n"
        "Backends: wia (Windows), twain (Windows), escl (network AirScan).\n"
        "This build supports escl only; use a Windows build for WIA/TWAIN scanners.\n"
        "\n"
        "OPTIONS (scan)\n"
        "  --device <id|auto>   id from 'list' (wia:..., twain:..., escl:http://ip:port/eSCL)\n"
        "  --backends <list>    restrict backends                       default: wia,twain,escl\n"
        "  --mode <color|gray|bw>                                       default: color\n"
        "  --dpi <75..1200>                                             default: 300\n"
        "  --source <auto|flatbed|adf|duplex>                           default: auto\n"
        "  --format <png|jpeg|bmp|tiff>   default: from --out extension, else png\n"
        "  --quality <1..100>   JPEG quality when the gateway encodes   default: 90\n"
        "  --out <path>         directory (trailing / or no extension) or file path\n"
        "  --area <l,t,w,h>     crop box in millimetres                 default: full\n"
        "  --brightness <-1000..1000>   hardware (WIA/TWAIN only)\n"
        "  --contrast   <-1000..1000>   hardware (WIA/TWAIN only)\n"
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
    auto extToFormat = [](const std::string& e) -> std::string {
        std::string s = ToLower(e);
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
        target = fs::absolute(fs::path(o.out), ec);
        const char last = o.out.empty() ? '\0' : o.out.back();
        if (last == '\\' || last == '/' || fs::is_directory(target, ec) || !target.has_extension()) {
            dirMode = true;
        } else {
            dirMode = false;
            const std::string fromExt = extToFormat(target.extension().string());
            if (fromExt.empty())
                throw ScanError("invalid_argument", "--out has an unsupported extension. Use .png, .jpg, .bmp or .tif (or end the path with / for a directory).");
            if (!o.p.format.empty() && o.p.format != fromExt)
                throw ScanError("invalid_argument", "--format '" + o.p.format + "' conflicts with the --out extension.");
            o.p.format = fromExt;
        }
    }
    if (o.p.format.empty()) o.p.format = "png";
    const std::string ext = ExtensionFor(o.p.format);

    OutputPlan plan;
    if (dirMode) {
        fs::create_directories(target, ec);
        if (ec || !fs::is_directory(target, ec)) throw ScanError("output_error", "Cannot create the output directory.");
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        ::localtime_r(&now, &tm);
        char stamp[64];
        std::snprintf(stamp, sizeof stamp, "SCAN_%04d%02d%02d_%02d%02d%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        const std::string base = stamp;
        plan.dir = target;
        plan.finalName = [target, base, ext](size_t n) { return target / (base + "_p" + Pad4(n) + ext); };
        return plan;
    }
    fs::path parent = target.parent_path();
    fs::create_directories(parent, ec);
    if (ec || !fs::is_directory(parent, ec)) throw ScanError("output_error", "Cannot create the output directory.");
    const std::string stem = target.stem().string();
    plan.dir = parent;
    plan.finalName = [parent, target, stem, ext](size_t n) { return n == 1 ? target : parent / (stem + "_p" + Pad4(n) + ext); };
    return plan;
}

struct Resolved {
    IScanBackend* backend = nullptr;
    std::string nativeId, fullId;
};

static Resolved ResolveDevice(const std::string& id, BackendList& bes) {
    const std::string low = ToLower(id);
    for (const char* known : { "wia", "twain", "escl" }) {
        const std::string pre = std::string(known) + ":";
        if (low.rfind(pre, 0) != 0) continue;
        for (auto& b : bes) {
            if (b->Name() != known) continue;
            std::string rest = id.substr(pre.size());
            return { b.get(), rest, b->Name() + ":" + rest };
        }
        throw ScanError("invalid_argument", std::string("Backend '") + known + "' is not available in this build or disabled by --backends.");
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
    EmitJson(std::string("{\"ok\":true,\"contract_version\":") + std::to_string(kContractVersion) +
             ",\"command\":\"version\",\"name\":" + JStr(kProductName) +
             ",\"vendor\":" + JStr(kVendor) + ",\"version\":" + JStr(kVersion) +
             ",\"copyright\":" + JStr(kCopyright) +
             ",\"backends\":[\"escl\"],\"arch\":" + JStr(ArchName()) + "}");
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

    const std::string token = std::to_string(::getpid()) + "_" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    PageNamer tempNamer = [dir = plan.dir, token](size_t n) {
        return dir / (".sagtmp_" + token + "_" + Pad4(n) + ".part");
    };

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
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::ostringstream j;
    j << "{\"ok\":true,\"contract_version\":" << kContractVersion << ",\"command\":\"scan\",\"backend\":" << JStr(dev.backend->Name())
      << ",\"device\":" << JStr(dev.fullId)
      << ",\"settings\":{\"mode\":" << JStr(o.p.mode) << ",\"dpi\":" << o.p.dpi << ",\"source\":" << JStr(o.p.source)
      << ",\"format\":" << JStr(o.p.format) << "},\"page_count\":" << pages.size() << ",\"skipped_blank_pages\":" << skipped << ",\"pages\":[";
    for (size_t i = 0; i < pages.size(); ++i) {
        std::error_code ec;
        std::uintmax_t sz = fs::file_size(pages[i], ec);
        if (i) j << ',';
        j << "{\"index\":" << (i + 1) << ",\"path\":" << JStr(pages[i].string())
          << ",\"content_type\":" << JStr(ContentTypeFor(o.p.format)) << ",\"bytes\":" << (ec ? 0 : sz) << "}";
    }
    j << "],\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i) { if (i) j << ','; j << JStr(warnings[i]); }
    j << "],\"elapsed_ms\":" << ms << "}";
    EmitJson(j.str());
    return 0;
}

static int EmitError(const std::string& code, const std::string& message) {
    EmitJson("{\"ok\":false,\"contract_version\":" + std::to_string(kContractVersion) +
             ",\"error\":{\"code\":" + JStr(code) + ",\"message\":" + JStr(message) + "}}");
    if (g_verbose) std::fprintf(stderr, "[gateway] error: %s: %s\n", code.c_str(), message.c_str());
    return ExitCodeFor(code);
}

int main(int argc, char** argv) {
    try {
        Options o = ParseArgs(argc, argv);
        if (o.backendsExplicit) {
            for (const std::string& b : o.backends)
                if (b != "escl")
                    throw ScanError("invalid_argument", "Backend '" + b + "' is not available in this build; this platform supports escl only.");
        }
        if (o.command == "help") {
            const std::string h = HelpText();
            std::fwrite(h.data(), 1, h.size(), stdout);
            std::fflush(stdout);
            return o.explicitHelp ? 0 : 2;
        }
        if (o.command == "version") return CmdVersion();

        BackendList bes;
        if (o.backends.count("escl")) bes.push_back(std::make_unique<escl::EsclBackend>());

        if (o.command == "list") return CmdList(bes);
        return CmdScan(bes, o);
    } catch (const ScanError& e) {
        return EmitError(e.code, e.what());
    } catch (const std::exception& e) {
        return EmitError("internal_error", e.what());
    } catch (...) {
        return EmitError("internal_error", "Unknown fatal error.");
    }
}
