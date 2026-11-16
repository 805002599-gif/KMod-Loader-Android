// kmod mod core (Android prototype): hooks UObject::ProcessEvent, gets onto the game thread,
// and builds a minimal "Mods" button + panel with UMG widgets created at runtime.
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <algorithm>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "lua_host.h"
#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"

using namespace SDK;

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    KLogMsg(KLOG_INFO, "core", buf);
}

// ---------------------------------------------------------------------------------------------
// Minimal arm64 inline hook (patches 16 bytes; refuses if the first 4 instructions are PC-relative)
// ---------------------------------------------------------------------------------------------
static bool IsPcRelative(uint32_t i)
{
    if ((i & 0x1F000000) == 0x10000000) return true; // ADR/ADRP
    if ((i & 0x3B000000) == 0x18000000) return true; // LDR literal
    if ((i & 0x7C000000) == 0x14000000) return true; // B/BL
    if ((i & 0xFF000010) == 0x54000000) return true; // B.cond
    if ((i & 0x7E000000) == 0x34000000) return true; // CBZ/CBNZ
    if ((i & 0x7E000000) == 0x36000000) return true; // TBZ/TBNZ
    return false;
}

static bool InstallInlineHook(void* target, void* detour, void** outTrampoline)
{
    uint32_t* code = reinterpret_cast<uint32_t*>(target);
    for (int k = 0; k < 4; k++)
        if (IsPcRelative(code[k])) { Log("hook: instruction %d (0x%08x) is PC-relative, refusing", k, code[k]); return false; }

    // trampoline: 4 original instructions + ldr x16,#8 ; br x16 ; .quad target+16
    void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { Log("hook: trampoline mmap RWX failed errno=%d", errno); return false; }
    uint32_t* tr = reinterpret_cast<uint32_t*>(mem);
    memcpy(tr, code, 16);
    tr[4] = 0x58000050; // ldr x16, #8
    tr[5] = 0xD61F0200; // br x16
    *reinterpret_cast<uint64_t*>(&tr[6]) = reinterpret_cast<uint64_t>(target) + 16;
    __builtin___clear_cache(reinterpret_cast<char*>(tr), reinterpret_cast<char*>(tr + 8));

    uintptr_t page = reinterpret_cast<uintptr_t>(target) & ~uintptr_t(4095);
    if (mprotect(reinterpret_cast<void*>(page), 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    { Log("hook: mprotect RWX on target failed errno=%d", errno); return false; }

    uint32_t patch[4] = { 0x58000050, 0xD61F0200, 0, 0 };
    *reinterpret_cast<uint64_t*>(&patch[2]) = reinterpret_cast<uint64_t>(detour);
    memcpy(target, patch, 16);
    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target) + 16);
    mprotect(reinterpret_cast<void*>(page), 8192, PROT_READ | PROT_EXEC);

    *outTrampoline = mem;
    return true;
}

// ---------------------------------------------------------------------------------------------
// ProcessEvent detour -> game-thread tick
// ---------------------------------------------------------------------------------------------
using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
static ProcessEventFn g_origPE = nullptr;
static std::atomic<uint64_t> g_peCalls{0};
static thread_local int t_gameThread = -1; // -1 unknown, 0 no, 1 yes
static thread_local bool t_busy = false;

static void OnGameTick();

static double NowSec()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void HookedProcessEvent(UObject* self, UFunction* fn, void* parms)
{
    if (t_gameThread < 0)
    {
        char name[32] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        t_gameThread = (strcmp(name, "GameThread") == 0) ? 1 : 0;
        Log("ProcessEvent first seen on thread '%s' tid=%d -> gameThread=%d", name, (int)gettid(), t_gameThread);
    }

    const bool hooked = t_gameThread == 1 && !t_busy && LuaHost_HasHook(fn);
    if (hooked) { t_busy = true; LuaHost_PreHook(self, fn, parms); t_busy = false; }

    g_origPE(self, fn, parms);

    if (t_busy) return;
    if (t_gameThread != 1) return;
    if (hooked) { t_busy = true; LuaHost_PostHook(self, fn, parms); t_busy = false; }

    static double lastTick = 0;
    if ((++g_peCalls & 0x0F) != 0) return;
    double now = NowSec();
    static double lastFast = 0;
    if (LuaHost_HasFast() && now - lastFast >= 0.004)          // fast lane (FastLoop): every few ms, only when a mod asked for it
    {
        lastFast = now;
        t_busy = true;
        LuaHost_FastTick(now);
        t_busy = false;
    }
    if (now - lastTick < 0.05) return; // ~20 Hz
    lastTick = now;

    t_busy = true;
    OnGameTick();
    t_busy = false;
}

// ---------------------------------------------------------------------------------------------
// Pak mounting: kmod/paks/*.pak are mounted through the game's own "mount" console command (see paks in modloader.lua).
// The engine picks the pak order from the path, which would put our paks BELOW the game's own, so the real mount
// function (FPakPlatformFile mount worker, called with {path, order, ...} in x1) is hooked and the order is raised for
// paks that live in kmod/paks. Offset is for libUnreal.so of KARDS 1.61.27596 (checked by its first instruction).
// ---------------------------------------------------------------------------------------------
static const uintptr_t kPakMountVA = 0x4dc30f0;
using PakMountFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static PakMountFn g_origPakMount = nullptr;
static std::atomic<int> g_pakMounts{0};

static bool WidePrefix(const char16_t* w, const std::string& prefix)
{
    for (size_t i = 0; i < prefix.size(); i++) if (w[i] != (char16_t)(unsigned char)prefix[i]) return false;
    return true;
}

static uint64_t HookedPakMount(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    const char16_t* path = a1 ? *reinterpret_cast<const char16_t* const*>(a1) : nullptr;
    if (path)
    {
        static const std::string prefix = KModRoot() + "/paks/";
        static const std::string alt = "/sdcard/Android/media/com.android1939.kardsapk/kmod/paks/";
        int32_t* order = reinterpret_cast<int32_t*>(a1 + 8);
        std::string shown;
        for (int i = 0; i < 200 && path[i]; i++) shown += (path[i] < 128 ? (char)path[i] : '?');
        if (WidePrefix(path, prefix) || WidePrefix(path, alt))
        {
            int n = g_pakMounts.fetch_add(1);
            Log("pak mount: %s (order %d -> %d)", shown.c_str(), (int)*order, 100 + n);
            *order = 100 + n;      // later mounts win over earlier ones; all of them win over the game's paks
        }
        else Log("pak mount (game): %s order=%d", shown.c_str(), (int)*order);
    }
    return g_origPakMount(a0, a1, a2, a3, a4, a5, a6, a7);
}

// The pak layer's "mount" entry (HandleMountPakDelegate(this, const FString& path, int32 order)) is not reachable from
// the console in a shipping build, and we do not have its `this`. The engine binds it to FCoreDelegates::MountPak with
// a small delegate object { vtable, ?, handle, this, function }; find that object in memory once and take `this` from it.
static const uintptr_t kPakMountDelegateVT = 0x92982e0;     // vtable (value stored in the delegate object)
static const uintptr_t kPakMountDelegateFn = 0x4dc0218;     // the bound function
static void* g_pakFile = nullptr;

static void* FindPakFile(uintptr_t base)
{
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) { Log("pak mount: cannot read /proc/self/maps errno=%d", errno); return nullptr; }
    const uint64_t vt = base + kPakMountDelegateVT, fn = base + kPakMountDelegateFn;
    std::vector<uint8_t> buf(1 << 20);
    void* found = nullptr;
    char line[512];
    size_t scanned = 0, regions = 0, vtOnly = 0;
    while (!found && fgets(line, sizeof(line), maps))
    {
        unsigned long lo, hi; char perms[8] = {}; int nameAt = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &nameAt) < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        if (strstr(line, ".so") || strstr(line, "/system/") || strstr(line, "/apex/") || strstr(line, "/dev/")) continue;
        regions++;
        for (unsigned long a = lo; a < hi && !found; a += buf.size())
        {
            size_t want = std::min<unsigned long>(buf.size(), hi - a);
            struct iovec lv = {buf.data(), want}, rv = {reinterpret_cast<void*>(a), want};
            ssize_t got = process_vm_readv(getpid(), &lv, 1, &rv, 1, 0);      // fails (instead of crashing) on unreadable memory
            if (got < 0x28) continue;
            scanned += got;
            for (size_t i = 0; i + 0x28 <= (size_t)got; i += 8)
            {
                if (*reinterpret_cast<uint64_t*>(&buf[i]) != vt) continue;
                vtOnly++;
                if (*reinterpret_cast<uint64_t*>(&buf[i + 0x20]) != fn) continue;
                found = reinterpret_cast<void*>(*reinterpret_cast<uint64_t*>(&buf[i + 0x18]));
                break;
            }
        }
    }
    fclose(maps);
    Log("pak mount: scanned %zu regions, %zu MB, %zu vtable hits", regions, scanned >> 20, vtOnly);
    return found;
}

// Game thread. Mounts a pak file through the engine (the order is raised by HookedPakMount for kmod/paks).
bool KMod_MountPak(const std::string& path)
{
    uintptr_t base = InSDKUtils::GetImageBase();
    if (!base || !g_origPakMount) return false;
    if (!g_pakFile) { g_pakFile = FindPakFile(base); Log("pak mount: PakPlatformFile object %p", g_pakFile); }
    if (!g_pakFile) return false;
    std::u16string w;
    for (unsigned char c : path) w.push_back(c);          // paths are ASCII in practice; UTF-8 bytes are widened one by one
    struct { const char16_t* data; int32_t num; int32_t max; } fs = {w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1};
    using Fn = uint64_t (*)(void*, void*, int32_t);
    uint64_t r = reinterpret_cast<Fn>(base + kPakMountDelegateFn)(g_pakFile, &fs, -1);
    Log("pak mount: %s -> %d", path.c_str(), (int)(r & 0xff));
    return (r & 0xff) != 0;
}

static double g_readySince = 0;

// Runs on the game thread (throttled ~20 Hz) from the ProcessEvent hook.
static void OnGameTick()
{
    UEngine* engine = UEngine::GetEngine();
    if (!engine || !engine->GameViewport) return;
    if (!UWorld::GetWorld()) return;

    double now = NowSec();
    if (g_readySince == 0) { g_readySince = now; Log("engine ready"); }
    if (now - g_readySince < 5.0) return; // let the game settle

    LuaHost_Init();          // no-op after the first call
    LuaHost_Tick(now);
}

// ---------------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------------
static void InitThread()
{
    Log("core init thread started");
    for (int i = 0; i < 600; i++) // up to 5 minutes
    {
        uintptr_t base = InSDKUtils::GetImageBase();
        if (base)
        {
            TUObjectArray* objs = InSDKUtils::GetGObjects();
            if (objs && objs->Num() > 50000) { break; }
        }
        usleep(500 * 1000);
    }
    uintptr_t base = InSDKUtils::GetImageBase();
    if (!base) { KLog(KLOG_ERROR, "core", "libUnreal.so not found"); return; }
    sleep(3);

    void* target = reinterpret_cast<void*>(base + Offsets::ProcessEvent);
    Log("image base=0x%lx ProcessEvent=%p objects=%d", (unsigned long)base, target, InSDKUtils::GetGObjects()->Num());
    void* tramp = nullptr;
    if (!InstallInlineHook(target, reinterpret_cast<void*>(&HookedProcessEvent), &tramp)) { KLog(KLOG_ERROR, "core", "hook install FAILED"); return; }
    g_origPE = reinterpret_cast<ProcessEventFn>(tramp);
    Log("ProcessEvent hook installed, trampoline=%p", tramp);
    {
        uint32_t* mt = reinterpret_cast<uint32_t*>(base + kPakMountVA);
        if (mt[0] != 0xA9BA7BFD)      // stp x29, x30, [sp, #-0x60]!
            Log("pak mount: unexpected code at the mount function (other game build?): kmod/paks will not get priority");
        else
        {
            void* ptramp = nullptr;
            if (InstallInlineHook(mt, reinterpret_cast<void*>(&HookedPakMount), &ptramp)) { g_origPakMount = reinterpret_cast<PakMountFn>(ptramp); Log("pak mount hook installed"); }
        }
    }
    KLogInstallCrashHandlers();      // the engine installs its own handlers during start-up; (re)install ours on top of them
}

__attribute__((constructor)) static void CoreMain()
{
    KLogInit();
    Log("libkmodcore loaded");
    std::thread(InitThread).detach();
}
