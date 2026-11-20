// Red Two: boots the recompiled game.
//
//   red_two.exe [--disc <GSWE64.iso>] [--trace] [--import-dolphin[=saves,textures,codes]]
//
// Environment: R2_IO_LOG=1 (every hardware register access), R2_DVD_LOG=1, R2_DSP_LOG=1,
// R2_ARAM_LOG=1, R2_TRACE=1 (record guest function entries; ring dumped on fatal errors).
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <cxxabi.h>
#include <dirent.h>
#ifdef __ANDROID__
// Android: SDL's Java activity loads libmain.so and calls SDL_main (main below, renamed)
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <unwind.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#else
#include <execinfo.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <pthread.h>
#include <cstdint>
static long gettid() {  // (macOS has no gettid: the thread's system-wide id)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return long(id);
}
#endif
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#include <ctime>
#endif

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "gfx/gx_bridge.h"
#include "hw/gx_fifo.h"
#include "mods.h"
#include "achievements.h"
#include "runtime.h"
#include "update.h"
#include "settings.h"
#include "../common/disc_image.h"
#include "hw/musyx_dsp.h"
#include "savestate.h"
#include "table.h"

#if !defined(_WIN32) && !defined(__ANDROID__)
// This program's file (Linux: /proc/self/exe; macOS: the loader's, inside Red Two.app)
static std::string selfExe() {
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    char real[PATH_MAX];
    return realpath(buf, real) ? real : buf;
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
#endif
}
#endif

namespace rt {

// --- logging ----------------------------------------------------------------------------
static std::mutex s_logMu;
static u64 s_t0Tb = 0;

void logf(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(s_logMu);
    const double t = double(tbNow() - s_t0Tb) / double(kTbClock);
    fprintf(stderr, "[%9.3f %-6s] %s\n", t, tag, buf);
    fflush(stderr);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "RedTwo", "[%9.3f %-6s] %s", t, tag, buf);  // (adb logcat)
#endif
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n[%9.3f] FATAL: %s\n", double(tbNow() - s_t0Tb) / double(kTbClock), buf);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "RedTwo", "FATAL: %s", buf);
#endif
    fprintf(stderr, "%s", guestBacktrace().c_str());
    if (g_trace) {
        fprintf(stderr, "recent guest functions (oldest first):\n");
        printTraceRing(stderr, 40);
    }
    fflush(stderr);
    quit(1);
}

// Ends the process at once. ExitProcess runs DLL teardown (Dawn/D3D12, SDL) while other host
// threads may hold locks, which can leave a dead, unclosable window behind.
void quit(int code) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), UINT(code));
#else
    _exit(code);  // (the same reason: no atexit teardown of Dawn / SDL)
#endif
}

#ifndef _WIN32
// Linux: main() keeps its arguments for a restart (the same program image, the same process)
static char** s_argv = nullptr;
#endif

static std::string s_restartProgram;  // an update put a new program in place: restart into it
void setRestartProgram(const std::string& path) { s_restartProgram = path; }

#ifdef __ANDROID__
// Android: static methods of the activity (android/java/.../RedTwoActivity.java), called on
// this thread (SDL attaches it to the Java VM).
namespace android {
struct Call {
    JNIEnv* env = nullptr;
    jclass cls = nullptr;
    jmethodID m = nullptr;
    Call(const char* name, const char* sig) {
        env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
        if (!env || !act) return;
        cls = env->GetObjectClass(act);
        env->DeleteLocalRef(act);
        if (cls) m = env->GetStaticMethodID(cls, name, sig);
        if (!m) {
            env->ExceptionClear();
            logf("main", "Android: the activity has no method %s %s", name, sig);
        }
    }
    ~Call() {
        if (env && env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        if (env && cls) env->DeleteLocalRef(cls);
    }
    std::string str(jstring s) {
        if (!s) return {};
        const char* c = env->GetStringUTFChars(s, nullptr);
        std::string r = c ? c : "";
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        return r;
    }
};
std::string callString(const char* name) {
    Call c(name, "()Ljava/lang/String;");
    return c.m ? c.str(static_cast<jstring>(c.env->CallStaticObjectMethod(c.cls, c.m))) : std::string();
}
int callIntWithString(const char* name, const std::string& arg) {
    Call c(name, "(Ljava/lang/String;)I");
    if (!c.m) return -1;
    jstring a = c.env->NewStringUTF(arg.c_str());
    const int r = c.env->CallStaticIntMethod(c.cls, c.m, a);
    c.env->DeleteLocalRef(a);
    return c.env->ExceptionCheck() ? -1 : r;
}
void callVoidWithString(const char* name, const std::string& arg) {
    Call c(name, "(Ljava/lang/String;)V");
    if (!c.m) return;
    jstring a = c.env->NewStringUTF(arg.c_str());
    c.env->CallStaticVoidMethod(c.cls, c.m, a);
    c.env->DeleteLocalRef(a);
}
bool callVoid(const char* name) {
    Call c(name, "()V");
    if (!c.m) return false;
    c.env->CallStaticVoidMethod(c.cls, c.m);
    return !c.env->ExceptionCheck();
}
}  // namespace android
#endif

void quitToDesktop() {
    state::finishWrites();  // a save state still being written
    cardFlushNow();         // (written once a retrace: the last one's writes may be pending)
    quit(0);
}

// After a save import: the game read its save at boot, so a new instance (same program, same
// arguments, same working folder) takes over and this one quits.
void restartGame(bool toLauncher) {
    // the new instance: straight into the game (a reset, a save import) or to the launcher
#ifdef _WIN32
    _putenv(toLauncher ? "R2_LAUNCHER=1" : "R2_LAUNCHER=0");
#else
    setenv("R2_LAUNCHER", toLauncher ? "1" : "0", 1);
#endif
#if defined(__ANDROID__)
    // Android: a small activity in a process of its own ends this one and starts the game
    // again (RestartActivity.java); this thread waits for the end.
    logf("main", "restarting");
    fflush(stderr);
    if (!android::callVoid("restartApp")) {
        logf("main", "restart failed");
        return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    quit(0);
#elif !defined(_WIN32)
    // Linux: the process replaces itself (same PID: Steam, gamescope and an AppImage's mount
    // stay with it). Its other threads and windows go with the old image.
    logf("main", "restarting");
    fflush(stderr);
    // The new image would inherit every descriptor not marked close-on-exec: the window
    // system's socket (the old window would stay until the new process ends), the GPU's and
    // input devices. Those close; pipes stay (an AppImage's runtime keeps its mount alive
    // through one), and so do files (the log).
    if (DIR* d = opendir("/proc/self/fd")) {
        std::vector<int> close;
        while (dirent* e = readdir(d)) {
            const int fd = atoi(e->d_name);
            if (fd <= 2 || fd == dirfd(d)) continue;
            char target[256];
            const ssize_t n = readlinkat(dirfd(d), e->d_name, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (!strncmp(target, "socket:", 7) || !strncmp(target, "/dev/", 5)) close.push_back(fd);
        }
        closedir(d);
        for (int fd : close) ::close(fd);
    }
    if (s_argv && !s_restartProgram.empty()) {  // (an updated AppImage: the new file, not this mount)
        std::vector<char*> args;
        args.push_back(const_cast<char*>(s_restartProgram.c_str()));
        for (char** a = s_argv + 1; *a; a++) args.push_back(*a);
        args.push_back(nullptr);
        execv(s_restartProgram.c_str(), args.data());
    }
    if (s_argv) execv(selfExe().c_str(), s_argv);
    logf("main", "restart failed (errno %d)", errno);
    return;
#else
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    if (!s_restartProgram.empty())  // (an update: the new red_two.exe; this one is now red_two.exe.old)
        MultiByteToWideChar(CP_UTF8, 0, s_restartProgram.c_str(), -1, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        logf("main", "restart failed (error %lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf("main", "restarting");
    quit(0);
#endif
}

// The menu's reset, like the console's reset button: the game starts over from the boot screen
// in a new instance (as after a save import); the memory card's last writes and a save state
// still being written reach their files first.
void resetGame(bool toLauncher) {
    state::finishWrites();
    cardFlushNow();
    restartGame(toLauncher);
}

#ifndef _WIN32
#ifdef __ANDROID__
// Android: the host call stack through the unwinder, names from the libraries' dynamic symbol
// tables (dladdr), demangled; offsets into each library for addr2line / ndk-stack.
void printHostStack(FILE* f, int skip) {
    struct Walk {
        void** cur;
        void** end;
    };
    void* frames[48];
    Walk w{frames, frames + 48};
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
            auto* s = static_cast<Walk*>(arg);
            const uintptr_t pc = _Unwind_GetIP(ctx);
            if (pc) {
                if (s->cur == s->end) return _URC_END_OF_STACK;
                *s->cur++ = reinterpret_cast<void*>(pc);
            }
            return _URC_NO_REASON;
        },
        &w);
    const int n = int(w.cur - frames);
    fprintf(f, "host call stack (thread %ld):\n", long(gettid()));
    for (int i = skip + 1; i < n; i++) {
        Dl_info di{};
        const uintptr_t pc = uintptr_t(frames[i]);
        if (!dladdr(frames[i], &di) || !di.dli_fname) {
            fprintf(f, "  %p\n", frames[i]);
            continue;
        }
        std::string name = di.dli_sname ? di.dli_sname : "?";
        int status = 0;
        if (char* dm = di.dli_sname ? abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &status) : nullptr) {
            if (status == 0) name = dm;
            free(dm);
        }
        const char* lib = strrchr(di.dli_fname, '/');
        fprintf(f, "  %s+0x%lx (%s+0x%lx)\n", name.c_str(), (unsigned long)(pc - uintptr_t(di.dli_saddr)),
                lib ? lib + 1 : di.dli_fname, (unsigned long)(pc - uintptr_t(di.dli_fbase)));
        __android_log_print(ANDROID_LOG_ERROR, "RedTwo", "  %s+0x%lx (%s+0x%lx)", name.c_str(),
                            (unsigned long)(pc - uintptr_t(di.dli_saddr)), lib ? lib + 1 : di.dli_fname,
                            (unsigned long)(pc - uintptr_t(di.dli_fbase)));
    }
}
#else
// Linux: the host call stack through the C library's backtrace (names from the exe's dynamic
// symbol table: linked with -rdynamic), demangled.
void printHostStack(FILE* f, int skip) {
    void* frames[48];
    const int n = backtrace(frames, 48);
    char** names = backtrace_symbols(frames, n);
    fprintf(f, "host call stack (thread %ld):\n", long(gettid()));
    for (int i = skip + 1; i < n; i++) {
        std::string line = names ? names[i] : "?";
        // "exe(_ZN2rt3fooEv+0x12) [0x...]": demangle the name between '(' and '+'
        const size_t open = line.find('('), plus = line.find('+', open);
        if (open != std::string::npos && plus != std::string::npos && plus > open + 1) {
            int status = 0;
            char* dm = abi::__cxa_demangle(line.substr(open + 1, plus - open - 1).c_str(), nullptr, nullptr, &status);
            if (dm && status == 0) line = line.substr(0, open + 1) + dm + line.substr(plus);
            free(dm);
        }
        fprintf(f, "  %s\n", line.c_str());
    }
    free(names);
}
#endif

// A crash outside guest memory (memory.cpp's SIGSEGV handler passes those on), SIGILL, SIGFPE.
void hostCrash(int sig, void* info, void* uc) {
    (void)uc;
    const siginfo_t* si = static_cast<const siginfo_t*>(info);
    fprintf(stderr, "\nFATAL: host signal %d (%s) at address %p\n", sig, strsignal(sig), si ? si->si_addr : nullptr);
    printHostStack(stderr, 0);
    if (g_trace) printTraceRing(stderr, 40);
    quit(3);
}
#else
// The host call stack, symbolized from the PDB (fatal errors only).
void printHostStack(FILE* f, int skip) {
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(DWORD(skip + 1), 48, frames, nullptr);
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);  // fails harmlessly if the profiler already did it
    fprintf(f, "host call stack (thread %lu):\n", GetCurrentThreadId());
    for (USHORT i = 0; i < n; i++) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const DWORD64 a = DWORD64(frames[i]);
        if (SymFromAddr(proc, a, &disp, si)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof line;
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, a, &ldisp, &line))
                fprintf(f, "  %s+0x%llx (%s:%lu)\n", si->Name, (unsigned long long)disp, line.FileName, line.LineNumber);
            else
                fprintf(f, "  %s+0x%llx\n", si->Name, (unsigned long long)disp);
        } else {
            fprintf(f, "  %p\n", frames[i]);
        }
    }
}

static LONG WINAPI onHostCrash(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "\nFATAL: host exception %08lX at %p\n", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (g_trace) printTraceRing(stderr, 40);
    quit(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// abort() (Aurora's and the C++ library's last resort, std::terminate) ends the process through
// __fastfail, past the unhandled-exception filter: say where it came from first.
static void onAbort(int) {
    fprintf(stderr, "\n[%9.3f] FATAL: abort() called\n", double(tbNow() - s_t0Tb) / double(kTbClock));
    printHostStack(stderr, 1);
    fprintf(stderr, "%s", guestBacktrace().c_str());
    if (g_trace) {
        fprintf(stderr, "recent guest functions (oldest first):\n");
        printTraceRing(stderr, 40);
    }
    quit(4);
}

bool envFlag(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

const char* symName(u32 addr) {
    for (const SymName* s = g_symNames; s->name; s++)
        if (s->addr == addr) return s->name;
    return nullptr;
}

std::string describeAddr(u32 addr) {
    char buf[96];
    if (const char* n = symName(addr)) snprintf(buf, sizeof buf, "%08X (%s)", addr, n);
    else snprintf(buf, sizeof buf, "%08X", addr);
    return buf;
}

// --- disc -----------------------------------------------------------------------------

// The disc: an ISO/GCM or RVZ/WIA image (src/common/disc_image.cpp).
static std::unique_ptr<disc::Image> s_image;

bool discOpen(const std::string& path) {
    std::string err;
    s_image = disc::open(path, err);
    if (!s_image) {
        logf("disc", "%s", err.c_str());
        return false;
    }
    logf("disc", "%s image %s (%llu MiB)", s_image->kind(), path.c_str(), (unsigned long long)(s_image->size() >> 20));
    return s_image->size() > 0x440;
}

bool discRead(u64 off, void* dst, u32 len) { return s_image && s_image->read(off, dst, len); }

std::string discProblem(const std::string& path, std::string* what) {
    if (path.empty()) return "No disc image chosen.";
    std::string err;
    const auto img = disc::open(path, err);
    if (!img) return "Cannot read it as a disc image (.iso, .gcm, .rvz or .wia): " + err;
    u8 hdr[0x60] = {};
    if (!img->read(0, hdr, sizeof hdr)) return "Cannot read it: the file is too short or damaged.";
    // (the GameCube disc magic word at 0x1C)
    if (hdr[0x1C] != 0xC2 || hdr[0x1D] != 0x33 || hdr[0x1E] != 0x9F || hdr[0x1F] != 0x3D)
        return "This is not a GameCube disc image.";
    const std::string id(reinterpret_cast<const char*>(hdr), 6);
    std::string name(reinterpret_cast<const char*>(hdr + 0x20), strnlen(reinterpret_cast<const char*>(hdr + 0x20), 0x40));
    while (!name.empty() && (name.back() == ' ' || u8(name.back()) < 0x20)) name.pop_back();
    for (char& ch : name)
        if (u8(ch) < 0x20 || u8(ch) > 0x7E) ch = '?';
    if (what) {
        char size[32];
        snprintf(size, sizeof size, "%.2f GiB", double(img->size()) / double(1ull << 30));
        *what = (name.empty() ? std::string("a disc") : name) + " (" + id + (hdr[7] ? ", revision " + std::to_string(hdr[7]) : "") +
                "), " + img->kind() + " image, " + size;
    }
    if (id == "GSWE64") return hdr[7] == 0 ? std::string() : "This is revision " + std::to_string(hdr[7]) + " of the disc; Red Two was made from revision 0.";
    if (id.compare(0, 3, "GSW") == 0)
        return "This is Rogue Leader for another region (" + id + "); Red Two needs the USA release (GSWE64).";
    return "This is " + (name.empty() ? std::string("another game") : name) + " (" + id + "), not Rogue Leader (USA, GSWE64).";
}

static u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

struct DiscFile {
    std::string path;
    u32 off, size;
};
static std::vector<DiscFile> s_files;

static void loadFst() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) return;
    const u32 fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428);
    std::vector<u8> fst(fstSize);
    if (!discRead(fstOff, fst.data(), fstSize)) return;
    const u32 n = be32(&fst[8]);
    const char* names = reinterpret_cast<const char*>(&fst[n * 12]);
    std::vector<std::pair<std::string, u32>> dirs{{"", n}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        const u8* e = &fst[i * 12];
        const std::string name = names + (be32(e) & 0xFFFFFF);
        const std::string path = dirs.back().first + name;
        if (e[0]) dirs.push_back({path + "/", be32(e + 8)});
        else s_files.push_back({path, be32(e + 4), be32(e + 8)});
    }
}

std::string discFileAt(u64 off) {
    for (auto& f : s_files)
        if (off >= f.off && off < u64(f.off) + f.size) {
            char b[32];
            snprintf(b, sizeof b, "+0x%llX", (unsigned long long)(off - f.off));
            return f.path + b;
        }
    return "";
}

// --- guest thread dump (debugging; races with the guest, reads memory without faulting) ---
u32 peekGuest32(u32 a) {
    u8* p;
    if (isVm(a)) {
        p = g_vmRead[(a - kVmStart) >> 12];
        if (!p) return 0;
        p += a & 0xFFF;
    } else if (isMem1(a)) {
        p = hp(a);
    } else {
        return 0;
    }
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}

void dumpThreads() {
    // OSThread: context 0x000, state u16 0x2C8, priority 0x2D0, queue 0x2DC, linkActive 0x2FC
    static const char* states[] = {"?", "ready", "running", "?", "waiting", "?", "?", "?", "moribund"};
    u32 t = peekGuest32(0x800000DC);
    for (int n = 0; t && n < 32; n++) {
        const u32 state = peekGuest32(t + 0x2C8) >> 16;
        std::string chain;
        u32 sp = peekGuest32(t + 4);
        for (int d = 0; d < 10 && sp; d++) {
            sp = peekGuest32(sp);
            const u32 lr = sp ? peekGuest32(sp + 4) : 0;
            if (!lr) break;
            char b[16];
            snprintf(b, sizeof b, " %08X", lr);
            chain += b;
        }
        logf("thread", "%08X %-8s prio %2u waits on %08X ctx lr %08X srr0 %08X | stack:%s", t,
             state < 9 ? states[state] : "?", peekGuest32(t + 0x2D0), peekGuest32(t + 0x2DC), peekGuest32(t + 0x84), peekGuest32(t + 0x198),
             chain.c_str());
        t = peekGuest32(t + 0x2FC);
    }
}

// --- frame hook -------------------------------------------------------------------------
static u32 s_frames = 0;
void viFrameHook() {
    static const bool noGfx = envFlag("R2_NO_GFX"), gxLog = envFlag("R2_GX_LOG");
    s_frames++;
    if (noGfx) input::poll();  // otherwise the renderer polls (Aurora pads)
    else gfx::vblank();
    state::scriptTick();  // R2_STATE_SCRIPT (tests)
    if (s_frames % 120 == 0) vmAramReport();
    // R2_WATCH_PTR=<addr>[,<offset>,<len>[,<from seconds>]] (hex): R2_WATCH on what the pointer at addr points to,
    // offset -0x20 len 0x20 by default (the game's heap block header: who corrupts it?)
    if (static const char* wp = getenv("R2_WATCH_PTR"); wp) {
        static u32 at = 0, last = 0;
        static s32 off = -0x20;
        static u32 len = 0x20;
        static double from = 0;  // ,<seconds>: start following then
        if (!at) {
            char* end;
            at = u32(strtoul(wp, &end, 16));
            if (*end == ',') off = s32(strtol(end + 1, &end, 16));
            if (*end == ',') len = u32(strtoul(end + 1, &end, 16));
            if (*end == ',') from = strtod(end + 1, &end);
        }
        const u32 p = double(tbNow() - s_t0Tb) / double(kTbClock) >= from ? peekGuest32(at) : 0;
        if (p != last) {
            last = p;
            extern void watchSet(u32 lo, u32 len);
            watchSet(p ? p + u32(off) : 0, p ? len : 0);
            logf("watch", "%08X now points to %08X: watching %08X+%X (header %08X %08X %08X %08X %08X %08X %08X %08X)", at, p,
                 p + u32(off), len, peekGuest32(p - 32), peekGuest32(p - 28), peekGuest32(p - 24), peekGuest32(p - 20),
                 peekGuest32(p - 16), peekGuest32(p - 12), peekGuest32(p - 8), peekGuest32(p - 4));
        }
    }
    // R2_MEM_DUMP=<vblank>,<phys addr>,<len>[;...]: guest memory -> build/mem_<vblank>_<addr>.bin.
    static const char* memDump = getenv("R2_MEM_DUMP");
    for (const char* s = memDump; s && *s;) {
        char* end;
        const u32 at = u32(strtoul(s, &end, 10)), raw = u32(strtoul(end + 1, &end, 16)),
                  addr = isVm(raw) ? raw : raw & 0x1FFFFFFF, len = u32(strtoul(end + 1, &end, 16));
        if (at == s_frames && isVm(addr)) {
            // VM addresses: the pages mapped right now (others read as zero)
            std::vector<u8> buf(len & ~3u);
            for (u32 o = 0; o < buf.size(); o += 4) {
                const u32 w = peekGuest32(addr + o);
                buf[o] = u8(w >> 24), buf[o + 1] = u8(w >> 16), buf[o + 2] = u8(w >> 8), buf[o + 3] = u8(w);
            }
            char name[64];
            snprintf(name, sizeof name, "build/mem_%u_%08X.bin", at, addr);
            if (FILE* f = fopen(name, "wb")) {
                fwrite(buf.data(), 1, buf.size(), f);
                fclose(f);
                logf("dump", "wrote %s", name);
            }
        } else if (at == s_frames)
            if (const u8* p = physPtr(addr, len)) {
                char name[64];
                snprintf(name, sizeof name, "build/mem_%u_%08X.bin", at, addr);
                if (FILE* f = fopen(name, "wb")) {
                    fwrite(p, 1, len, f);
                    fclose(f);
                    logf("dump", "wrote %s", name);
                }
            }
        s = *end ? end + 1 : end;
    }
    // R2_PEEK=<addr>:<len>[,...] (hex; virtual VM addresses work while their page is mapped):
    // log those guest words every R2_PEEK_EVERY vblanks (default 30).
    static const char* peekSpec = getenv("R2_PEEK");
    static const u32 peekEvery = getenv("R2_PEEK_EVERY") ? u32(atoi(getenv("R2_PEEK_EVERY"))) : 30;
    if (peekSpec && peekEvery && s_frames % peekEvery == 0) {
        for (const char* s = peekSpec; *s;) {
            char* end;
            const u32 addr = u32(strtoul(s, &end, 16)), len = *end == ':' ? u32(strtoul(end + 1, &end, 16)) : 4;
            std::string line;
            for (u32 o = 0; o < len; o += 4) {
                char b[12];
                snprintf(b, sizeof b, "%s%08X", o % 32 ? " " : (o ? " | " : ""), peekGuest32(addr + o));
                line += b;
            }
            logf("peek", "v%u %08X: %s", s_frames, addr, line.c_str());
            s = *end ? end + 1 : end;
        }
    }
    if (gxLog && s_frames % 60 == 0) {
        const GxStats& g = gxStats();
        logf("gx", "vblank %u, presented %llu: fifo %llu bytes, %llu draws, %llu verts, %llu BP, %llu XF, %llu CP, %llu DL", s_frames,
             (unsigned long long)gfx::frames(),
             (unsigned long long)g.bytes, (unsigned long long)g.draws, (unsigned long long)g.vertices,
             (unsigned long long)g.bp, (unsigned long long)g.xf, (unsigned long long)g.cp, (unsigned long long)g.dl);
        logf("gx", "%s", gfx::stats().c_str());
        extern u64 g_viLateMaxTb, g_viLateSumTb;
        logf("gx", "retrace lateness: avg %.2f ms, max %.2f ms", double(g_viLateSumTb) * 1000.0 / 60.0 / double(kTbClock),
             double(g_viLateMaxTb) * 1000.0 / double(kTbClock));
        g_viLateMaxTb = g_viLateSumTb = 0;
        extern u64 g_safepoints, g_timerFlags, g_idleWaits;
        logf("gx", "safepoints %llu, timer flags %llu, idle waits %llu", (unsigned long long)g_safepoints,
             (unsigned long long)g_timerFlags, (unsigned long long)g_idleWaits);
        g_safepoints = g_timerFlags = g_idleWaits = 0;
        logf("gx", "dsp: %s", musyxDspStats().c_str());
#ifdef _WIN32
        static const bool memLog = envFlag("R2_MEM_LOG");
        if (memLog) {  // large committed regions of this process
            MEMORY_BASIC_INFORMATION mbi;
            u64 privTotal = 0;
            for (u8* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi; p = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize) {
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Type == MEM_PRIVATE) privTotal += mbi.RegionSize;
                if (mbi.RegionSize >= (64u << 20))
                    logf("mem", "  %p %6llu MiB %s %s prot %lX", mbi.BaseAddress, (unsigned long long)(mbi.RegionSize >> 20),
                         mbi.Type == MEM_PRIVATE ? "private" : mbi.Type == MEM_MAPPED ? "mapped" : "image",
                         mbi.AllocationBase == mbi.BaseAddress ? "(alloc base)" : "", mbi.Protect);
            }
            logf("mem", "committed private total %llu MiB", (unsigned long long)(privTotal >> 20));
        }
#endif
    }
}

// --- boot -------------------------------------------------------------------------------
// Load main.dol and set up low memory the way the IPL and apploader leave it.
static u32 bootDisc() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) fatal("cannot read the disc header");
    if (memcmp(hdr, "GSWE64", 6) != 0) fatal("this is not Star Wars: Rogue Leader (USA): game ID %.6s", (const char*)hdr);
    if (hdr[7] != 0) fatal("unsupported disc revision %u (rev 0 is supported)", hdr[7]);
    const u32 dolOff = be32(hdr + 0x420), fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428),
              fstMax = be32(hdr + 0x42C);

    u8 dh[0x100];
    discRead(dolOff, dh, sizeof dh);
    for (int i = 0; i < 18; i++) {
        const u32 off = be32(dh + 4 * i), addr = be32(dh + 0x48 + 4 * i), size = be32(dh + 0x90 + 4 * i);
        if (!size) continue;
        if (!discRead(dolOff + off, hp(addr), size)) fatal("cannot read DOL section %d", i);
    }
    const u32 bss = be32(dh + 0xD8), bssSize = be32(dh + 0xDC), entry = be32(dh + 0xE0);
    // bss is cleared by __init_data, but the sdata sections inside it were just loaded
    (void)bss;
    (void)bssSize;

    // FST at the top of MEM1, as the apploader puts it
    const u32 fstAddr = (0x81800000u - fstMax) & ~0x1Fu;
    if (!discRead(fstOff, hp(fstAddr), fstSize)) fatal("cannot read the FST");
    const u32 bi2Addr = (fstAddr - 0x2000) & ~0x1Fu;
    discRead(0x440, hp(bi2Addr), 0x2000);

    memcpy(hp(0x80000000), hdr, 0x20);  // disc ID
    wr32(0x80000020, 0x0D15EA5E);       // boot magic
    wr32(0x80000024, 1);
    wr32(0x80000028, kMem1Size);        // physical memory size
    wr32(0x8000002C, 3);                // console type: retail
    wr32(0x80000030, 0);                // arena low: from the linker symbol
    wr32(0x80000034, bi2Addr);          // arena high: below bi2/FST
    wr32(0x80000038, fstAddr);
    wr32(0x8000003C, fstMax);
    wr32(0x800000CC, 0);                // video mode: NTSC
    wr32(0x800000D0, 0x01000000);       // ARAM size
    wr32(0x800000EC, 0x81800000);       // debug monitor top (none)
    wr32(0x800000F0, kMem1Size);        // simulated memory size
    wr32(0x800000F4, bi2Addr);
    wr32(0x800000F8, u32(kBusClock));
    wr32(0x800000FC, u32(kCpuClock));
    logf("boot", "DOL entry %08X, FST at %08X, arena high %08X", entry, fstAddr, bi2Addr);
    return entry;
}

}  // namespace rt

// The program is a Windows GUI app (no console window on a double-click). Its log goes to
// stderr when the parent captured it (test runs), to the parent's console with --console, and
// otherwise to a log of its own for every session: %APPDATA%\Red Two\logs\red_two_<date>_<time>.log
// (the newest 30 are kept; red_two.log in the working directory if that folder cannot be made).
static std::string s_sessionLog;

#ifndef _WIN32
// Linux: the log goes to stderr when it is a terminal or a test run captured it, and otherwise
// (a desktop launcher, Steam) to a log of its own for every session in the user data folder
// (~/.local/share/Red Two/logs/), the newest 30 kept.
static void setupLogOutput(int, char**) {
    if (isatty(2) || getenv("R2_BACKGROUND") || getenv("R2_HEADLESS") || getenv("R2_INPUT") || getenv("R2_LOG_STDERR")) return;
    std::string dir;
    if (const std::string u = rt::mods::userDir(); !u.empty()) {
        dir = u + "logs";
        mkdir(dir.c_str(), 0755);
        struct stat st{};
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) dir.clear();
    }
    if (dir.empty()) {
        s_sessionLog = "red_two.log";
    } else {
        std::vector<std::string> old;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d))
                if (!strncmp(e->d_name, "red_two_", 8) && strstr(e->d_name, ".log")) old.push_back(e->d_name);
            closedir(d);
        }
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 29 < old.size(); i++) unlink((dir + "/" + old[i]).c_str());
        const time_t t = time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);
        char name[64];
        snprintf(name, sizeof name, "/red_two_%04d-%02d-%02d_%02d-%02d-%02d.log", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
        s_sessionLog = dir + name;
    }
    if (!freopen(s_sessionLog.c_str(), "w", stderr)) freopen("/dev/null", "w", stderr);
}

static void logSessionHeader(int argc, char** argv) {
    using rt::logf;
    const std::string exePath = selfExe();
    const char* exe = exePath.c_str();
    std::string built = "?";
    struct stat st{};
    if (stat(exe, &st) == 0) {
        tm lt{};
        localtime_r(&st.st_mtime, &lt);
        char b[64];
        strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &lt);
        built = b;
    }
    const time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    char now[64];
    strftime(now, sizeof now, "%Y-%m-%d %H:%M:%S", &lt);
    std::string args;
    for (int i = 1; i < argc; i++) args += std::string(" ") + argv[i];
    char cwd[PATH_MAX] = "";
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    logf("session", "Red Two %s started %s; exe %s (built %s)%s", rt::update::version(), now, exe, built.c_str(), args.c_str());
    logf("session", "working directory %s%s%s%s%s", cwd, s_sessionLog.empty() ? "" : "; log ", s_sessionLog.c_str(),
         getenv("APPIMAGE") ? "; AppImage " : "", getenv("APPIMAGE") ? getenv("APPIMAGE") : "");
}
#else
static void setupLogOutput(int argc, char** argv) {
    bool console = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--console")) console = true;
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        freopen("CONOUT$", "w", stderr);
        return;
    }
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    std::string dir;
    if (const std::string u = rt::mods::userDir(); !u.empty()) {  // (the portable folder or %APPDATA%\Red Two)
        dir = u + "logs";
        CreateDirectoryA(dir.c_str(), nullptr);
        if (GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES) dir.clear();
    }
    if (dir.empty()) {
        s_sessionLog = "red_two.log";
    } else {
        // keep the newest 29 old ones (names sort by time)
        std::vector<std::string> old;
        WIN32_FIND_DATAA fd;
        if (HANDLE f = FindFirstFileA((dir + "\\red_two_*.log").c_str(), &fd); f != INVALID_HANDLE_VALUE) {
            do old.push_back(fd.cFileName);
            while (FindNextFileA(f, &fd));
            FindClose(f);
        }
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 29 < old.size(); i++) DeleteFileA((dir + "\\" + old[i]).c_str());
        SYSTEMTIME st;
        GetLocalTime(&st);
        char name[64];
        snprintf(name, sizeof name, "\\red_two_%04u-%02u-%02u_%02u-%02u-%02u.log", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        s_sessionLog = dir + name;
    }
    if (!freopen(s_sessionLog.c_str(), "w", stderr)) freopen("red_two.log", "w", stderr);
}

// The session log's first lines: which build this is and how it was started (the settings
// follow from settings::load).
static void logSessionHeader(int argc, char** argv) {
    using rt::logf;
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string built = "?";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fa)) {
        FILETIME local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
        char b[64];
        snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        built = b;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::string args;
    for (int i = 1; i < argc; i++) args += std::string(" ") + argv[i];
    char cwd[MAX_PATH] = "";
    GetCurrentDirectoryA(sizeof cwd, cwd);
    logf("session", "Red Two %s started %04u-%02u-%02u %02u:%02u:%02u; exe %s (built %s)%s", rt::update::version(), now.wYear,
         now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, exe, built.c_str(), args.c_str());
    logf("session", "working directory %s%s%s", cwd, s_sessionLog.empty() ? "" : "; log ", s_sessionLog.c_str());
}
#endif

// The disc image when none was given (--disc): the usual names in the working folder and
// orig/, then every .iso / .gcm / .rvz / .wia in the folders a player keeps it in - this
// program's, the AppImage's, the user data folder (%APPDATA%\Red Two, ~/.local/share/Red Two),
// and on Linux EmuDeck's (~/Emulation/roms/gc) - whose disc ID is GSWE64.
static std::string findDisc() {
    namespace fs = std::filesystem;
    for (const char* p : {"orig/GSWE64.iso", "orig/GSWE64.rvz", "GSWE64.rvz", "GSWE64.iso"})
        if (FILE* f = fopen(p, "rb")) {
            fclose(f);
            return p;
        }
    std::vector<fs::path> dirs = {fs::current_path()};
    if (const char* base = SDL_GetBasePath()) dirs.push_back(fs::path(std::u8string(base, base + strlen(base))));
    if (const char* ai = getenv("APPIMAGE")) dirs.push_back(fs::path(ai).parent_path());
#ifdef __APPLE__
    // macOS: the folder holding Red Two.app (the base path is its Contents/Resources/)
    if (const char* base = SDL_GetBasePath()) dirs.push_back(fs::path(base).parent_path().parent_path().parent_path().parent_path());
#endif
    // the user folder (the portable one; Android/data/<package>/files, reachable over USB)
    if (const std::string u = rt::mods::userDir(); !u.empty()) dirs.push_back(fs::path(std::u8string(u.begin(), u.end())));
#if !defined(_WIN32) && !defined(__ANDROID__)
    if (const char* home = getenv("HOME")) {
        dirs.push_back(fs::path(home) / "Emulation/roms/gc");
        dirs.push_back(fs::path(home) / "Emulation/roms/gamecube");
        dirs.push_back(fs::path(home) / "Games");
    }
#endif
    for (const fs::path& d : dirs) {
        std::error_code ec;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            std::string ext = it->path().extension().string();
            for (auto& c : ext) c = char(tolower(static_cast<unsigned char>(c)));
            if (ext != ".iso" && ext != ".gcm" && ext != ".rvz" && ext != ".wia") continue;
            const std::u8string u8 = it->path().u8string();
            const std::string path(u8.begin(), u8.end());
            std::string err;
            auto img = disc::open(path, err);
            char id[6] = {};
            if (img && img->read(0, id, 6) && !memcmp(id, "GSWE64", 6)) return path;
        }
    }
    return {};
}

#ifdef __ANDROID__
// Android: the disc image picked in the system's document picker (any folder, a USB stick, a
// cloud drive) - the one picked before (its permission kept), else the picker. The image is
// read through the descriptor the picker's document gave ("fd:<n>", kept open).
static std::string androidDisc() {
    using rt::logf;
    std::string uri = rt::android::callString("savedDiscUri");
    bool asked = false;
    for (;;) {
        if (!uri.empty()) {
            const int fd = rt::android::callIntWithString("openUriFd", uri);
            if (fd >= 0) {
                const std::string path = "fd:" + std::to_string(fd);
                std::string err;
                auto img = disc::open(path, err);
                char id[8] = {};
                if (img && img->read(0, id, 8) && !memcmp(id, "GSWE64", 6) && id[7] == 0) {
                    rt::android::callVoidWithString("saveDiscUri", uri);
                    logf("disc", "the picked disc %s", uri.c_str());
                    return path;
                }
                close(fd);
                logf("disc", "%s is not Rogue Leader (USA, GSWE64 revision 0)%s%s", uri.c_str(), err.empty() ? "" : ": ", err.c_str());
                if (asked)
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Red Two",
                                             "That file is not Star Wars: Rogue Squadron II - Rogue Leader (USA, GSWE64, revision 0). "
                                             "Choose your disc image (.iso, .gcm, .rvz or .wia).",
                                             nullptr);
            } else {
                logf("disc", "cannot open %s (moved or deleted?)", uri.c_str());
            }
        }
        if (!asked)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Red Two",
                                     "Red Two needs your own disc image of Star Wars: Rogue Squadron II - Rogue Leader (USA, GSWE64) "
                                     "as .iso, .gcm, .rvz or .wia.\n\nChoose it on the next screen; Red Two remembers it.",
                                     nullptr);
        asked = true;
        uri = rt::android::callString("pickDisc");
        if (uri.empty()) return {};  // canceled
    }
}
#endif

int main(int argc, char** argv) {
    using namespace rt;
#ifndef _WIN32
    s_argv = argv;
#endif
#ifdef __ANDROID__
    // relative paths (saves/, R2_* dumps) land in the app's shared storage folder
    if (const std::string u = mods::userDir(); !u.empty()) chdir(u.c_str());
    // the back button / gesture opens the menu (as F1) instead of closing the app
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    setupLogOutput(argc, argv);
    std::string disc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_trace = true;
    }
    if (envFlag("R2_TRACE")) g_trace = true;
#ifndef _WIN32
    signal(SIGABRT, onAbort);
    std::set_terminate([] { onAbort(SIGABRT); });
    signal(SIGINT, [](int) { quit(0); });
    signal(SIGTERM, [](int) { quit(0); });
    signal(SIGPIPE, SIG_IGN);  // (a closed log pipe must not end the game)
    {
        struct sigaction sa{};
        sa.sa_sigaction = [](int sig, siginfo_t* si, void* uc) { hostCrash(sig, si, uc); };
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGILL, &sa, nullptr);
        sigaction(SIGFPE, &sa, nullptr);
        sigaction(SIGSEGV, &sa, nullptr);  // (memory.cpp takes SIGSEGV over for guest memory)
    }
#else
    // A crash must end the process, not wait behind an error-reporting dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // A game: no EcoQoS. Windows 11 otherwise throttles the process (efficiency cores, coarse
    // timers) whenever its window is in the background or covered, e.g. on a second screen.
    {
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    }
    SetUnhandledExceptionFilter(onHostCrash);
    signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::set_terminate([] { onAbort(SIGABRT); });
    SetConsoleCtrlHandler([](DWORD) -> BOOL { quit(0); return TRUE; }, TRUE);  // Ctrl+C / console closed
#endif
    debugInit();
    if (const char* w = getenv("R2_WATCHDOG")) {
        const int secs = atoi(w) > 0 ? atoi(w) : 2;
        g_trace = true;
        std::thread([secs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                logf("watch", "%s", traceRing(12).c_str());
                dumpThreads();
            }
        }).detach();
    }
    eventsInit();
    logSessionHeader(argc, argv);
    settings::load();  // after eventsInit: logging needs the time base
    // --import-dolphin[=saves,textures,codes]: bring this game's things over from Dolphin, exit
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--import-dolphin", 16) && (argv[i][16] == 0 || argv[i][16] == '='))
            quit(mods::importCommandLine(argv[i][16] ? argv[i] + 17 : ""));
    }
    mods::codesInit();  // cheat codes (codes.ini), applied every retrace
    rt::update::init();  // a newer release on GitHub? (setting check_updates; a worker thread)
    viSetRate(settings::get().fps);
    profilerStart();
    diagStart();  // stall reports and perf lines in the log (profiler.cpp)
    s_t0Tb = tbNow();
    if (!memInit()) fatal("could not reserve the guest address space");
    tableInit();
    // The disc: --disc, else the one chosen in the launcher before (setting disc_path), else the
    // usual places (findDisc: next to the program, the user folder, ...). None, or one that is not
    // Rogue Leader (USA, revision 0): the launcher (gfx::init) asks for it before anything boots.
    const bool gfxOn = !envFlag("R2_NO_GFX");
    std::string discNote;  // why the launcher asks
    if (!disc.empty()) {
        if (const std::string problem = discProblem(disc); !problem.empty()) {
            logf("disc", "--disc %s: %s", disc.c_str(), problem.c_str());
            if (!gfxOn) fatal("--disc %s: %s", disc.c_str(), problem.c_str());
            discNote = "The disc image given with --disc (" + disc + ") cannot be used: " + problem;
            disc.clear();
        }
    } else if (const std::string saved = settings::get().discPath; !saved.empty()) {
        if (const std::string problem = discProblem(saved); problem.empty()) {
            disc = saved;
        } else {
            logf("disc", "the disc chosen before, %s: %s", saved.c_str(), problem.c_str());
            discNote = "The disc image chosen before (" + saved + ") cannot be used now: " + problem;
        }
    }
    if (disc.empty()) disc = findDisc();
#ifdef __ANDROID__
    if (disc.empty()) disc = androidDisc();
    if (disc.empty()) fatal("no disc image picked");
#endif
    if (disc.empty() && !gfxOn) fatal("no disc image: pass --disc <your Rogue Leader .iso or .rvz>");
    ra::init(disc);  // RetroAchievements: logs in meanwhile, hashes the disc once it is known (if enabled)
    // the window first: its launcher asks for the disc if there is none (and the game boots after it)
    if (gfxOn) {
        if (!gfx::init(argc, argv, disc, discNote) && disc.empty()) fatal("no window, and no disc image to start");
        ra::setDisc(disc);  // (the launcher's choice)
    } else {
        input::init(false);  // scripted input still works without a renderer
    }
    if (!discOpen(disc)) fatal("cannot open the disc image %s", disc.c_str());
    loadFst();
    Ctx* boot = nullptr;
    threadsInit(boot);
    Ctx& c = *boot;
    const u32 entry = bootDisc();
    if (envFlag("R2_RESTART_TEST")) {  // the save importer's restart, once (the new instance runs on)
#ifdef _WIN32
        _putenv("R2_RESTART_TEST=");
#else
        unsetenv("R2_RESTART_TEST");
#endif
        restartGame();
    }
    audio::init();
    hwInit();
    c.msr = 0x2032;  // FP, IR, DR, RI; interrupts off until the OS enables them
    logf("boot", "%u recompiled functions; jumping to %s", g_funcCount, describeAddr(entry).c_str());
    threadsRun(entry);  // the boot fiber runs the game; this thread only takes save states now
}
