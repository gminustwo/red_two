// HTTP(S) for RetroAchievements and the updater: see http.h.
#include "http.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#else
#include <dlfcn.h>
#endif

namespace rt::http {
namespace {

// the body as it arrives: into the response or the file, with the progress callback
struct Sink {
    const Request& r;
    Response& out;
    u64 got = 0, total = 0;
    bool cancelled = false;
    bool write(const char* p, size_t n) {
        if (r.file) {
            if (fwrite(p, 1, n, r.file) != n) return false;
        } else {
            out.body.append(p, n);
        }
        got += n;
        if (r.progress && !r.progress(got, total)) {
            cancelled = true;
            return false;
        }
        return true;
    }
};

}  // namespace

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
}  // namespace

Response fetch(const Request& r) {
    Response out;
    // one session per user agent (RetroAchievements', the updater's)
    HINTERNET session = WinHttpOpen(widen(r.userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    const std::wstring wurl = widen(r.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        WinHttpCloseHandle(session);
        return out;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET con = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, r.post.empty() ? L"GET" : L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        // (per step: each read may wait this long; redirects - GitHub's downloads - are followed)
        WinHttpSetTimeouts(req, 10000, 10000, 20000, 30000);
        std::wstring headers;
        if (!r.post.empty())
            headers = L"Content-Type: " + widen(r.contentType.empty() ? "application/x-www-form-urlencoded" : r.contentType) + L"\r\n";
        for (const std::string& h : r.headers) headers += widen(h) + L"\r\n";
        const u64 start = GetTickCount64();
        if (WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1),
                               r.post.empty() ? WINHTTP_NO_REQUEST_DATA : LPVOID(r.post.data()), DWORD(r.post.size()),
                               DWORD(r.post.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                                &size, WINHTTP_NO_HEADER_INDEX);
            out.status = int(code);
            Sink sink{r, out};
            wchar_t len[32] = {};
            DWORD lenSize = sizeof len;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lenSize,
                                    WINHTTP_NO_HEADER_INDEX))
                sink.total = _wcstoui64(len, nullptr, 10);
            std::vector<char> buf(256 * 1024);
            bool complete = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) {
                    complete = false;
                    break;
                }
                if (!avail) break;
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, DWORD(buf.size())), &got)) {
                    complete = false;
                    break;
                }
                if (!got) break;
                if (!sink.write(buf.data(), got) ||
                    (r.timeoutSeconds > 0 && GetTickCount64() - start > u64(r.timeoutSeconds) * 1000)) {
                    complete = false;
                    break;
                }
            }
            out.ok = complete;
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(session);
    return out;
}

#elif defined(__ANDROID__)
// Android: Java's HttpURLConnection (RedTwoActivity.httpFetch): the system's TLS and
// certificates. Worker threads are attached to the Java VM by SDL_GetAndroidJNIEnv. The whole
// body comes back at once (no progress; extra headers are not passed).
Response fetch(const Request& r) {
    Response out;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
    if (!act) return out;
    jclass cls = env->GetObjectClass(act);
    env->DeleteLocalRef(act);
    jmethodID m = env->GetStaticMethodID(cls, "httpFetch", "(Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;[I)[B");
    if (!m) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return out;
    }
    jstring jurl = env->NewStringUTF(r.url.c_str());
    jbyteArray jpost = nullptr;
    if (!r.post.empty()) {
        jpost = env->NewByteArray(jsize(r.post.size()));
        env->SetByteArrayRegion(jpost, 0, jsize(r.post.size()), reinterpret_cast<const jbyte*>(r.post.data()));
    }
    jstring jtype = env->NewStringUTF(r.contentType.empty() ? "application/x-www-form-urlencoded" : r.contentType.c_str());
    jstring jua = env->NewStringUTF(r.userAgent.c_str());
    jintArray jstatus = env->NewIntArray(1);
    auto res = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, m, jurl, jpost, jtype, jua, jstatus));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else if (res) {
        jint st = 0;
        env->GetIntArrayRegion(jstatus, 0, 1, &st);
        out.status = int(st);
        const jsize n = env->GetArrayLength(res);
        std::string body(size_t(n), '\0');
        if (n) env->GetByteArrayRegion(res, 0, n, reinterpret_cast<jbyte*>(body.data()));
        Sink sink{r, out};
        sink.total = u64(n);
        out.ok = sink.write(body.data(), body.size());
    }
    for (jobject o : {jobject(res), jobject(jurl), jobject(jpost), jobject(jtype), jobject(jua), jobject(jstatus), jobject(cls)})
        if (o) env->DeleteLocalRef(o);
    return out;
}

#else
// libcurl from the system (every desktop Linux and SteamOS has libcurl.so.4), loaded when first
// needed: no build-time dependency, and the system's certificates.
namespace {
struct Curl {
    void* lib = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    int (*global_init)(long) = nullptr;
    bool ok = false;
};
Curl& curl() {
    static Curl c = [] {
        Curl c;
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "/usr/lib/libcurl.4.dylib"})
            if ((c.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!c.lib) {
            logf("http", "libcurl not found: no internet requests (RetroAchievements, updates)");
            return c;
        }
        auto sym = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(c.lib, name)); };
        sym(c.easy_init, "curl_easy_init");
        sym(c.easy_setopt, "curl_easy_setopt");
        sym(c.easy_perform, "curl_easy_perform");
        sym(c.easy_getinfo, "curl_easy_getinfo");
        sym(c.easy_cleanup, "curl_easy_cleanup");
        sym(c.slist_append, "curl_slist_append");
        sym(c.slist_free_all, "curl_slist_free_all");
        sym(c.global_init, "curl_global_init");
        c.ok = c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo && c.easy_cleanup && c.slist_append &&
               c.slist_free_all && c.global_init;
        if (c.ok) c.global_init(3);  // CURL_GLOBAL_DEFAULT
        return c;
    }();
    return c;
}

size_t curlWrite(char* p, size_t size, size_t n, void* user) {
    return static_cast<Sink*>(user)->write(p, size * n) ? size * n : 0;
}
int curlProgress(void* user, long long dltotal, long long, long long, long long) {
    auto* sink = static_cast<Sink*>(user);
    if (dltotal > 0) sink->total = u64(dltotal);
    return sink->cancelled ? 1 : 0;
}
}  // namespace

Response fetch(const Request& r) {
    Response out;
    Curl& c = curl();
    if (!c.ok) return out;
    void* h = c.easy_init();
    if (!h) return out;
    enum {
        OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_TIMEOUT = 13, OPT_LOW_SPEED_LIMIT = 19, OPT_LOW_SPEED_TIME = 20,
        OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018, OPT_HTTPHEADER = 10023, OPT_NOPROGRESS = 43, OPT_POST = 47,
        OPT_FOLLOWLOCATION = 52, OPT_POSTFIELDSIZE = 60, OPT_CONNECTTIMEOUT = 78, OPT_NOSIGNAL = 99,
        OPT_WRITEFUNCTION = 20011, OPT_XFERINFODATA = 10057, OPT_XFERINFOFUNCTION = 20219, INFO_RESPONSE_CODE = 0x200002,
    };
    Sink sink{r, out};
    void* headers = nullptr;
    c.easy_setopt(h, OPT_URL, r.url.c_str());
    c.easy_setopt(h, OPT_USERAGENT, r.userAgent.c_str());
    c.easy_setopt(h, OPT_WRITEFUNCTION, &curlWrite);
    c.easy_setopt(h, OPT_WRITEDATA, &sink);
    c.easy_setopt(h, OPT_NOPROGRESS, 0L);
    c.easy_setopt(h, OPT_XFERINFOFUNCTION, &curlProgress);
    c.easy_setopt(h, OPT_XFERINFODATA, &sink);
    c.easy_setopt(h, OPT_FOLLOWLOCATION, 1L);
    c.easy_setopt(h, OPT_NOSIGNAL, 1L);
    c.easy_setopt(h, OPT_CONNECTTIMEOUT, 10L);
    if (r.timeoutSeconds > 0) c.easy_setopt(h, OPT_TIMEOUT, long(r.timeoutSeconds));
    // (no total limit: a download stops only when nothing came for 30 s)
    c.easy_setopt(h, OPT_LOW_SPEED_LIMIT, 1L);
    c.easy_setopt(h, OPT_LOW_SPEED_TIME, 30L);
    if (!r.post.empty()) {
        const std::string ct = "Content-Type: " + (r.contentType.empty() ? std::string("application/x-www-form-urlencoded") : r.contentType);
        headers = c.slist_append(headers, ct.c_str());
        c.easy_setopt(h, OPT_POST, 1L);
        c.easy_setopt(h, OPT_POSTFIELDS, r.post.c_str());
        c.easy_setopt(h, OPT_POSTFIELDSIZE, long(r.post.size()));
    }
    for (const std::string& hd : r.headers) headers = c.slist_append(headers, hd.c_str());
    if (headers) c.easy_setopt(h, OPT_HTTPHEADER, headers);
    const int rc = c.easy_perform(h);
    long code = 0;
    if (rc == 0) c.easy_getinfo(h, INFO_RESPONSE_CODE, &code);
    out.status = int(code);
    out.ok = rc == 0;
    if (headers) c.slist_free_all(headers);
    c.easy_cleanup(h);
    return out;
}
#endif

}  // namespace rt::http
