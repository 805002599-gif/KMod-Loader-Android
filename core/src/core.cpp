// kmod mod core (Android prototype): hooks UObject::ProcessEvent, gets onto the game thread,
// and builds a minimal "Mods" button + panel with UMG widgets created at runtime.
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
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
    if (now - lastTick < 0.05) return; // ~20 Hz
    lastTick = now;

    t_busy = true;
    OnGameTick();
    t_busy = false;
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
    KLogInstallCrashHandlers();      // the engine installs its own handlers during start-up; (re)install ours on top of them
}

__attribute__((constructor)) static void CoreMain()
{
    KLogInit();
    Log("libkmodcore loaded");
    std::thread(InitThread).detach();
}
