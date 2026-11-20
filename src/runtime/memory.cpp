// Guest address space: one 4 GiB host reservation, guest address a at g_base + a
// (MEM1 and its heap extension twice, the locked cache; the VM region is translated, see vm.cpp).
//
// Save states keep the host stacks of the guest's threads (threads.cpp), whose recompiled
// frames hold host pointers: return addresses into the exe, the guest contexts, the stacks
// themselves. So the exe is linked at a fixed base (no ASLR), and guest memory and the fiber
// stacks go to fixed addresses too: a state then loads in any later session of the same build.
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
// macOS: no memfd (a POSIX shared memory object, unlinked at once), no MAP_FIXED_NOREPLACE (a
// plain hint, checked below), Mach-O instead of ELF, and the PIE the arm64 loader insists on:
// as on Android, a state loads only into the session that made it.
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#define MAP_FIXED_NOREPLACE 0
#else
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/sharedmem.h>
#include <link.h>
#endif
#endif

#include <atomic>
#include <string>

#include <xxhash.h>

#include "runtime.h"
#include "savestate.h"

namespace rt {

u8* g_base = nullptr;
#ifdef _WIN32
static HANDLE s_mem1 = nullptr;
#else
static int s_mem1 = -1;  // memfd: MEM1, mapped twice (0x80000000 and 0xC0000000)
#endif
static u8* s_stacks = nullptr;

constexpr u64 kFixedGuestBase = 0x0000020000000000ull;  // 2 TiB: free in practice
constexpr u64 kFixedStacks = 0x0000021000000000ull;

// R2_FAULT_DUMP="<spec>[;<spec>...]": on a guest access fault, log guest memory reached from the
// registers. A spec is r<n> followed by steps, +<hex> (add) or * (load the word there), and
// :<hex length>, e.g. "r22:200;r22+130*0*:100" (the object in r22, and what the first entry of
// its pointer array at +0x130 points to). Registers are as the context last stored them.
static void faultDump(const Ctx& c) {
    const char* spec = getenv("R2_FAULT_DUMP");
    if (!spec) return;
    auto readable = [](u32 a) { return a >= 0x80000000u && a < 0x80000000u + kMem1Mapped; };
    for (const char* p = spec; *p;) {
        if (*p != 'r') break;
        char* e = nullptr;
        const int reg = int(strtol(p + 1, &e, 10));
        p = e;
        u32 a = reg >= 0 && reg < 32 ? c.r[reg] : 0;
        bool ok = true;
        std::string path = "r" + std::to_string(reg);
        while (*p == '+' || *p == '*') {
            if (*p == '+') {
                a += u32(strtoul(p + 1, &e, 16));
                p = e;
            } else {
                p++;
                if (!readable(a)) {
                    ok = false;
                    break;
                }
                a = __builtin_bswap32(*reinterpret_cast<const u32*>(g_base + a));
            }
        }
        u32 len = 0x40;
        if (*p == ':') {
            len = u32(strtoul(p + 1, &e, 16));
            p = e;
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        if (!ok || !readable(a)) {
            logf("fault", "dump: %08X not readable", a);
            continue;
        }
        for (u32 off = 0; off < len && readable(a + off + 15); off += 16) {
            const u8* m = g_base + a + off;
            logf("fault", "%08X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X", a + off, m[0], m[1],
                 m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
}

#ifdef _WIN32
static DWORD s_guestThread = 0;  // the host thread the guest's fibers run on
static bool onGuestThread() { return GetCurrentThreadId() == s_guestThread; }
static unsigned long hostThreadId() { return GetCurrentThreadId(); }
#else
static pthread_t s_guestThread{};
static bool s_haveGuestThread = false;
static bool onGuestThread() { return s_haveGuestThread && pthread_equal(pthread_self(), s_guestThread); }
#ifdef __APPLE__
static unsigned long hostThreadId() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return (unsigned long)id;
}
#else
static unsigned long hostThreadId() { return (unsigned long)gettid(); }
#endif
#endif

// The SDK's BATs map 0x80000000-0x8FFFFFFF (cached) and 0xC0000000-0xCFFFFFFF (uncached) to
// physical 0-0x0FFFFFFF, but MEM1 ends at 24 MiB: on hardware an access past it does not fault
// (reads give nothing useful, Dolphin returns 0). Battle of Hoth reads there: a per-part loop
// (0x7FCF5418, block 0x7FCF58C8) indexes its 20-byte part records with r31, which only its two
// inner loops set, so the paths that jump straight to the block use the caller's r31 (a stack
// address): base + 0x800ABFB4 * 20, 14 MiB past the records (0x8230xxxx), about 30 s into the
// mission. Such pages get committed (zero-filled) when first touched and the access goes on.
// The heap extension (24-32 MiB, runtime.h) is mapped with MEM1: reads there give zeros too
// until the game's heap overflows into it (on hardware it would have stopped then).
static bool pastMem1(u32 guest) {
    return (guest >= 0x80000000u + kMem1Mapped && guest < 0x90000000u) || (guest >= 0xC0000000u + kMem1Mapped && guest < 0xC8000000u);
}

static bool commitPastMem1(u32 guest, bool write) {
    u8* page = g_base + (guest & ~0xFFFFu);
#ifdef _WIN32
    if (!VirtualAlloc(page, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        MEMORY_BASIC_INFORMATION mi{};  // another thread may have committed it meanwhile
        if (!VirtualQuery(g_base + guest, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    }
#else
    // (the reservation's own pages: zero-filled once readable; again is harmless)
    if (mprotect(page, 0x10000, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    static std::atomic<int> n{0};
    if (n++ < 8) {
        const bool guestThread = onGuestThread();
        logf("mem", "%s %08X, past the end of MEM1 (as on hardware: zeros, no fault)%s%s", write ? "write to" : "read of", guest,
             guestThread ? " in " : " by a host thread", guestThread ? describeAddr(curCtx().curFn).c_str() : "");
    }
    return true;
}

// A fault in guest memory: pages past MEM1 are committed, anything else is fatal (with the
// guest's registers when it was the game's thread). False: not guest memory.
static bool onGuestFault(u64 host, bool write, const void* at) {
    const u64 b = u64(g_base);
    if (!g_base || host < b || host >= b + 0x100000000ull) return false;
    const u32 guest = u32(host - b);
    if (pastMem1(guest) && commitPastMem1(guest, write)) return true;
    // Another host thread (Aurora's GX worker reading vertex arrays and textures, audio):
    // the guest's registers say nothing about it, its own call stack does.
    if (!onGuestThread()) {
        logf("fault", "host thread %lu %s guest address %08X at %p", hostThreadId(), write ? "wrote" : "read", guest, at);
        printHostStack(stderr, 0);
        fatal("host %s of unmapped guest address %08X (not the guest's thread)", write ? "write" : "read", guest);
    }
    const Ctx& c = curCtx();
    faultDump(c);
    if (getenv("R2_FAULT_DUMP")) printHostStack(stderr, 0);
    char regs[600];
    int n = 0;
    for (int i = 0; i < 32; i++) n += snprintf(regs + n, sizeof regs - n, "%sr%d=%08X", i % 8 ? " " : "\n  ", i, c.r[i]);
    snprintf(regs + n, sizeof regs - n, "\n  lr=%08X ctr=%08X", c.lr, c.ctr);
    fatal("guest %s of unmapped address %08X (current fn %s)%s", write ? "write" : "read", guest,
          describeAddr(curCtx().curFn).c_str(), regs);
}

#ifdef _WIN32
static LONG CALLBACK onAccessViolation(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_base) return EXCEPTION_CONTINUE_SEARCH;
    const u64 host = ep->ExceptionRecord->ExceptionInformation[1];
    const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    if (onGuestFault(host, write, ep->ExceptionRecord->ExceptionAddress)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
// SIGSEGV: guest memory as above; anything else goes to main.cpp's crash report
#if defined(__APPLE__) && defined(__aarch64__)
// macOS: the syndrome is in the exception state; WnR (bit 6) as below
static bool faultWasWrite(const ucontext_t* uc) { return (uc->uc_mcontext->__es.__esr >> 6) & 1; }
#elif defined(__aarch64__)
// AArch64: the kernel puts the fault's syndrome (ESR) in a record of the signal frame's
// reserved area; WnR (bit 6) says a data abort was a write.
static bool faultWasWrite(const ucontext_t* uc) {
    const u8* p = reinterpret_cast<const u8*>(uc->uc_mcontext.__reserved);
    const u8* end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        u32 magic, size;
        memcpy(&magic, p, 4);
        memcpy(&size, p + 4, 4);
        if (!magic || size < 8) break;
        if (magic == 0x45535201u && size >= 16) {  // ESR_MAGIC: {head, u64 esr}
            u64 esr;
            memcpy(&esr, p + 8, 8);
            return (esr >> 6) & 1;
        }
        p += size;
    }
    return false;
}
#endif
static void onSegv(int sig, siginfo_t* si, void* ucv) {
    const auto* uc = static_cast<const ucontext_t*>(ucv);
#if defined(__APPLE__) && defined(__aarch64__)
    const bool write = faultWasWrite(uc);
    const void* at = reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__pc);
#elif defined(__aarch64__)
    const bool write = faultWasWrite(uc);
    const void* at = reinterpret_cast<const void*>(uc->uc_mcontext.pc);
#else
    const bool write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;  // page fault error code: W
    const void* at = reinterpret_cast<const void*>(uc->uc_mcontext.gregs[REG_RIP]);
#endif
    if (onGuestFault(u64(si->si_addr), write, at)) return;
    hostCrash(sig, si, ucv);
}
#endif

#ifdef _WIN32
static bool mapAt(u8* base) {
    void* a = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Mapped, base + 0x80000000u);
    void* b = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Mapped, base + 0xC0000000u);
    // (0x7E000000-0x7FFFFFFF is translated by vm.cpp and never touched at g_base + a.)
    void* l = VirtualAlloc(base + kLockedCache, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* p = VirtualAlloc(base + kPortArena, kPortArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (a && b && l && p) {
        g_base = base;
        return true;
    }
    if (a) UnmapViewOfFile(a);
    if (b) UnmapViewOfFile(b);
    if (l) VirtualFree(l, 0, MEM_RELEASE);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return false;
}

bool memInit() {
    s_mem1 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMem1Mapped, nullptr);
    if (!s_mem1) return false;
    bool ok = mapAt(reinterpret_cast<u8*>(kFixedGuestBase));
    for (int attempt = 0; !ok && attempt < 32; attempt++) {
        u8* base = static_cast<u8*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
        if (!base) return false;
        VirtualFree(base, 0, MEM_RELEASE);
        ok = mapAt(base);
    }
    if (!ok) return false;
    // the guest threads' stacks (committed per fiber, threads.cpp)
    const u64 stacks = u64(kMaxFibers) * kFiberStackSize;
    s_stacks = static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(kFixedStacks), stacks, MEM_RESERVE, PAGE_NOACCESS));
    if (!s_stacks) s_stacks = static_cast<u8*>(VirtualAlloc(nullptr, stacks, MEM_RESERVE, PAGE_NOACCESS));
    if (!s_stacks) return false;
    if (u64(g_base) != kFixedGuestBase || u64(s_stacks) != kFixedStacks)
        logf("mem", "guest memory at %p, stacks at %p (not the fixed addresses: save states of other sessions will not load)",
             g_base, s_stacks);
    s_guestThread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, onAccessViolation);
    return true;
}
#else
// Linux: a 4 GiB reservation (no access), MEM1 (and the heap extension after it) from a memfd
// mapped into it twice, the locked cache and the port arena made accessible; pages past MEM1
// become accessible when touched.
static u8* reserve(u64 fixed, u64 size) {
    void* p = mmap(reinterpret_cast<void*>(fixed), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED || u64(p) != fixed) {
        if (p != MAP_FAILED) munmap(p, size);
        p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
}

bool memInit() {
#ifdef __ANDROID__
    // (bionic has memfd_create from API 30; shared memory of the platform's own from 26)
    s_mem1 = ASharedMemory_create("red_two_mem1", kMem1Mapped);
    if (s_mem1 < 0) return false;
#elif defined(__APPLE__)
    char name[64];
    snprintf(name, sizeof name, "/red_two_mem1.%d", int(getpid()));
    s_mem1 = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (s_mem1 < 0) return false;
    shm_unlink(name);  // (the descriptor keeps it)
    if (ftruncate(s_mem1, kMem1Mapped) != 0) return false;
#else
    s_mem1 = memfd_create("red_two_mem1", 0);
    if (s_mem1 < 0 || ftruncate(s_mem1, kMem1Mapped) != 0) return false;
#endif
    u8* base = reserve(kFixedGuestBase, 0x100000000ull);
    if (!base) return false;
    for (u32 at : {0x80000000u, 0xC0000000u})
        if (mmap(base + at, kMem1Mapped, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s_mem1, 0) == MAP_FAILED) return false;
    // (0x7E000000-0x7FFFFFFF is translated by vm.cpp and never touched at g_base + a.)
    if (mprotect(base + kLockedCache, 0x10000, PROT_READ | PROT_WRITE) != 0 ||
        mprotect(base + kPortArena, kPortArenaSize, PROT_READ | PROT_WRITE) != 0)
        return false;
    g_base = base;
    // the guest threads' stacks (made accessible per fiber, threads.cpp)
    s_stacks = reserve(kFixedStacks, u64(kMaxFibers) * kFiberStackSize);
    if (!s_stacks) return false;
    if (u64(g_base) != kFixedGuestBase || u64(s_stacks) != kFixedStacks)
        logf("mem", "guest memory at %p, stacks at %p (not the fixed addresses: save states of other sessions will not load)",
             g_base, s_stacks);
    s_guestThread = pthread_self();
    s_haveGuestThread = true;
    struct sigaction sa{};
    sa.sa_sigaction = onSegv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    return true;
}
#endif

u8* fiberStackRegion() { return s_stacks; }

u8* physPtr(u32 phys, u32 len) {
    phys &= 0x3FFFFFFF;
    if (phys >= kMem1Mapped || len > kMem1Mapped - phys) return nullptr;
    return g_base + 0x80000000u + phys;
}

// The exe's identity: its code. A state holds return addresses into it, so it loads only into
// the build that saved it.
#if defined(__ANDROID__)
// Android: the program is a shared library (libmain.so) the system loads at a random address,
// so a state loads only into the session that made it (the base is part of the identity). Its
// code: the executable segments of the object holding this function.
HostLayout hostLayout() {
    static u64 base = 0, hash = 0;
    if (!base) {
        dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void*) -> int {
                const u64 self = u64(&hostLayout);
                for (int i = 0; i < info->dlpi_phnum; i++) {
                    const auto& ph = info->dlpi_phdr[i];
                    if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
                    const u64 lo = info->dlpi_addr + ph.p_vaddr;
                    if (self < lo || self >= lo + ph.p_memsz) continue;
                    base = info->dlpi_addr;
                    for (int j = 0; j < info->dlpi_phnum; j++) {
                        const auto& p = info->dlpi_phdr[j];
                        if (p.p_type == PT_LOAD && (p.p_flags & PF_X))
                            hash ^= XXH3_64bits_withSeed(reinterpret_cast<const void*>(info->dlpi_addr + p.p_vaddr), p.p_filesz, u64(j));
                    }
                    return 1;
                }
                return 0;
            },
            nullptr);
    }
    return {base, hash, u64(g_base), u64(s_stacks)};
}
#elif defined(__APPLE__)
// macOS: the main image (index 0) at its ASLR slide; its code is the __TEXT,__text section.
HostLayout hostLayout() {
    static const u64 base = u64(_dyld_get_image_header(0));
    static const u64 hash = [] {
        unsigned long size = 0;
        const u8* text = getsectiondata(reinterpret_cast<const mach_header_64*>(base), "__TEXT", "__text", &size);
        return text ? XXH3_64bits(text, size) : 0;
    }();
    return {base, hash, u64(g_base), u64(s_stacks)};
}
#elif !defined(_WIN32)
extern "C" char __executable_start;  // the linker's: the start of the image
// The executable segments as the loader mapped them. Not __executable_start..etext: that range
// also covers the read-only data, and an exe rewritten by patchelf (the AppImage's rpath) has
// its headers moved to a new segment, leaving an unmapped page in that range (reading it
// crashed every save and load in the AppImage).
static int hashCodeSegments(dl_phdr_info* info, size_t, void* out) {
    u64 h = 0, n = 0;  // n, not the header's index: the same code hashes the same after patchelf
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
            h ^= XXH3_64bits_withSeed(reinterpret_cast<const void*>(info->dlpi_addr + ph.p_vaddr), ph.p_filesz, n++);
    }
    *static_cast<u64*>(out) = h;
    return 1;  // the first object is the exe itself
}
HostLayout hostLayout() {
    static u64 hash = [] {
        u64 h = 0;
        dl_iterate_phdr(hashCodeSegments, &h);
        return h;
    }();
    return {u64(&__executable_start), hash, u64(g_base), u64(s_stacks)};
}
#else
HostLayout hostLayout() {
    static u64 hash = [] {
        const u8* img = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        u64 h = 0;
        for (u32 i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE)
                h ^= XXH3_64bits_withSeed(img + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, i);
        return h ^ u64(nt->OptionalHeader.SizeOfImage);
    }();
    return {u64(GetModuleHandleW(nullptr)), hash, u64(g_base), u64(s_stacks)};
}
#endif

void memSaveState(state::Writer& w) {
    w.section("MEM1");
    w.bytes(g_base + 0x80000000u, kMem1Mapped);  // with the heap extension
    w.bytes(g_base + kLockedCache, kLockedCacheSize);
    w.bytes(g_base + kPortArena, kPortArenaSize);
}

void memLoadState(state::Reader& r) {
    r.section("MEM1");
    r.bytes(g_base + 0x80000000u, kMem1Mapped);
    r.bytes(g_base + kLockedCache, kLockedCacheSize);
    r.bytes(g_base + kPortArena, kPortArenaSize);
}

}  // namespace rt
