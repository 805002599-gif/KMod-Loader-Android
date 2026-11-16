// kmod mod core (Android prototype): hooks UObject::ProcessEvent, gets onto the game thread,
// and builds a minimal "Mods" button + panel with UMG widgets created at runtime.
#include <android/log.h>
#include <dirent.h>
#include <strings.h>
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
#include "resolver.h"
#include "kmod_boot.h"
#include "net_redirect.h"
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
// Heartbeat: an engine timer (K2_SetTimer, every ~16 ms) calls Actor.ForceNetUpdate on the PlayerController. The call goes
// through ProcessEvent, so it drives the tick even on a static screen where the game itself calls almost nothing.
static UFunction* g_hbFn = nullptr;

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
        if (t_gameThread == 1) KBoot_SetGameThread((int)gettid());
        Log("ProcessEvent first seen on thread '%s' tid=%d -> gameThread=%d", name, (int)gettid(), t_gameThread);
    }

    const bool hooked = t_gameThread == 1 && !t_busy && LuaHost_HasHook(fn);
    if (hooked) { t_busy = true; LuaHost_PreHook(self, fn, parms); t_busy = false; }

    g_origPE(self, fn, parms);

    if (t_busy) return;
    if (t_gameThread != 1) return;
    if (hooked) { t_busy = true; LuaHost_PostHook(self, fn, parms); t_busy = false; }

    static double lastTick = 0;
    const bool beat = (fn == g_hbFn);
    if (!beat && (++g_peCalls & 0x0F) != 0) return;
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
// Pak mounting: kmod/paks/*.pak are mounted by calling the engine's own mount entry (HandleMountPakDelegate, bound to
// FCoreDelegates::MountPak). The engine picks the pak order from the path, which would put our paks BELOW the game's own,
// so the real mount worker (called with {path, order, ...} in x1) is hooked and the order is raised for paks that live in
// kmod/paks. All addresses are found at run time by resolver.cpp (no per-build offsets).
// ---------------------------------------------------------------------------------------------
using PakMountFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static PakMountFn g_origPakMount = nullptr;
static std::atomic<int> g_pakMounts{0};
static PakAddrs g_pak;
static bool g_pakTried = false, g_pakReady = false;

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

static bool EnsurePak(uintptr_t base)
{
    if (g_pakTried) return g_pakReady;
    g_pakTried = true;
    std::string rep;
    if (!Resolver_ResolvePak(base, g_pak, rep)) { KLog(KLOG_WARN, "core", "pak mounting unavailable (unsupported game build?): %s", rep.c_str()); return false; }
    Log("pak layer: %s", rep.c_str());
    void* ptramp = nullptr;
    if (InstallInlineHook(reinterpret_cast<void*>(g_pak.worker), reinterpret_cast<void*>(&HookedPakMount), &ptramp)) g_origPakMount = reinterpret_cast<PakMountFn>(ptramp);
    else KLog(KLOG_WARN, "core", "pak mount hook failed: paks will be mounted with the engine's default (lowest) priority");
    g_pakReady = true;
    return true;
}

// Game thread. Mounts a pak file through the engine (the order is raised by HookedPakMount for kmod/paks).
bool KMod_MountPak(const std::string& path)
{
    uintptr_t base = InSDKUtils::GetImageBase();
    if (!base || !EnsurePak(base) || !g_pak.pakFile) return false;
    std::u16string w;
    for (unsigned char c : path) w.push_back(c);          // paths are ASCII in practice; UTF-8 bytes are widened one by one
    struct { const char16_t* data; int32_t num; int32_t max; } fs = {w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1};
    using Fn = uint64_t (*)(void*, void*, int32_t);
    uint64_t r = reinterpret_cast<Fn>(g_pak.mountFn)(g_pak.pakFile, &fs, -1);
    Log("pak mount: %s -> %d", path.c_str(), (int)(r & 0xff));
    return (r & 0xff) != 0;
}

// kmod/paks/*.pak: mounted from here, at "engine ready", so the paks are in place before the game loads most of its content.
static std::vector<std::pair<std::string, std::string>> g_pakStatus;        // game thread only

static std::vector<std::string> ListPakFiles()
{
    std::vector<std::string> names;
    if (DIR* d = opendir((KModRoot() + "/paks").c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".pak") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

static std::string* PakStatusRef(const std::string& name)
{
    for (auto& p : g_pakStatus) if (p.first == name) return &p.second;
    g_pakStatus.emplace_back(name, "");
    return &g_pakStatus.back().second;
}

void KMod_MountPendingPaks()
{
    for (const std::string& name : ListPakFiles())
    {
        std::string* st = PakStatusRef(name);
        if (*st == "mounted" || *st == "failed") continue;                       // handled once per game run
        if (KBoot_SafeMode()) { *st = "skipped (safe mode)"; continue; }
        if (KBoot_IsQuarantined("pak", name)) { *st = "skipped (it crashed the game)"; KLog(KLOG_WARN, "core", "pak %s crashed the game last time: not mounted", name.c_str()); continue; }
        if (KBoot_IsQuarantined("off", name)) { *st = "switched off"; continue; }
        KBoot_SetActivity("pak", name.c_str(), "");                              // a crash while mounting is blamed on the pak
        bool ok = KMod_MountPak(KModRoot() + "/paks/" + name);
        KBoot_SetActivity(nullptr, nullptr, nullptr);
        *st = ok ? "mounted" : "failed";
        if (ok) Log("mounted pak %s", name.c_str()); else KLog(KLOG_WARN, "core", "could not mount pak %s", name.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> KMod_PakStatus()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : ListPakFiles())
    {
        std::string st = *PakStatusRef(name);
        if (st.empty()) st = KBoot_IsQuarantined("off", name) ? "switched off" : "not mounted yet";
        out.emplace_back(name, st);
    }
    return out;
}

static double g_readySince = 0;
static double g_lastHeartbeatArm = 0;

// Game thread. Arms (or re-arms: the timer is replaced, not duplicated) the heartbeat; also follows world changes.
static void ArmHeartbeat(double now)
{
    if (now - g_lastHeartbeatArm < 2.0) return;
    g_lastHeartbeatArm = now;
    UWorld* world = UWorld::GetWorld();
    if (!world) return;
    UObject* pc = UGameplayStatics::GetPlayerController(world, 0);
    if (!pc) return;
    if (!g_hbFn) g_hbFn = APlayerController::StaticClass()->GetFunction("Actor", "ForceNetUpdate");
    if (!g_hbFn) return;
    UKismetSystemLibrary::K2_SetTimer(pc, FString(u"ForceNetUpdate"), 0.016f, true, false, 0.f, 0.f);
}

// Runs on the game thread (throttled ~20 Hz) from the ProcessEvent hook.
static void OnGameTick()
{
    UEngine* engine = UEngine::GetEngine();
    if (!engine || !engine->GameViewport) return;
    if (!UWorld::GetWorld()) return;

    double now = NowSec();
    if (g_readySince == 0) { g_readySince = now; Log("engine ready"); KMod_MountPendingPaks(); }
    ArmHeartbeat(now);
    if (now - g_readySince < 5.0) return; // let the game settle
    static bool stableMarked = false;
    if (!stableMarked && now - g_readySince > 60.0) { stableMarked = true; KBoot_MarkStable(); }

    LuaHost_Init();          // no-op after the first call
    LuaHost_Tick(now);
}

// ---------------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------------
static void InitThread()
{
    Log("core init thread started");
    uintptr_t base = 0;
    std::string report;
    bool ok = false;
    for (int i = 0; i < 600 && !ok; i++)      // up to 5 minutes
    {
        base = InSDKUtils::GetImageBase();
        if (base && Resolver_ResolveCore(base, report))
        {
            TUObjectArray* objs = InSDKUtils::GetGObjects();
            ok = objs && objs->Num() > 50000;
        }
        if (!ok) usleep(500 * 1000);
    }
    if (!base) { KLog(KLOG_ERROR, "core", "libUnreal.so not found"); return; }
    if (!ok)
    {
        KLog(KLOG_ERROR, "core", "this game build is not supported (could not locate the engine data): %s. kmod stays inactive.", report.c_str());
        return;
    }
    Log("resolved: %s", report.c_str());
    sleep(3);

    void* target = reinterpret_cast<void*>(base + Offsets::ProcessEvent);
    Log("image base=0x%lx ProcessEvent=%p objects=%d", (unsigned long)base, target, InSDKUtils::GetGObjects()->Num());
    void* tramp = nullptr;
    if (!InstallInlineHook(target, reinterpret_cast<void*>(&HookedProcessEvent), &tramp)) { KLog(KLOG_ERROR, "core", "hook install FAILED"); return; }
    g_origPE = reinterpret_cast<ProcessEventFn>(tramp);
    Log("ProcessEvent hook installed, trampoline=%p", tramp);
    KLogInstallCrashHandlers();      // the engine installs its own handlers during start-up; (re)install ours on top of them
}

__attribute__((constructor)) static void CoreMain()
{
    KLogInit();
    KBoot_Init();
    NetRedirect_Start();          // before anything else: the game reads its server URLs early
    Log("libkmodcore loaded");
    std::thread(InitThread).detach();
}
