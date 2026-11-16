#include "resolver.h"

#include <link.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "kmod_log.h"
#include "kmod_common.h"
#include "Basic.hpp"
#include "CoreUObject_classes.hpp"

using namespace SDK;

namespace
{
struct Seg { uintptr_t lo, hi; uint32_t flags; };
std::vector<Seg> g_segs;
uintptr_t g_modLo = 0, g_modHi = 0;

void LoadSegs()
{
    if (!g_segs.empty()) return;
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void*) -> int
        {
            if (!info->dlpi_name || !InSDKUtils::PathEndsWithModuleName(info->dlpi_name)) return 0;
            for (int i = 0; i < info->dlpi_phnum; i++)
            {
                const ElfW(Phdr)& p = info->dlpi_phdr[i];
                if (p.p_type != PT_LOAD) continue;
                Seg s{info->dlpi_addr + p.p_vaddr, info->dlpi_addr + p.p_vaddr + p.p_memsz, p.p_flags};
                g_segs.push_back(s);
                if (!g_modLo || s.lo < g_modLo) g_modLo = s.lo;
                if (s.hi > g_modHi) g_modHi = s.hi;
            }
            return 1;
        },
        nullptr);
}

bool SafeRead(uintptr_t addr, void* out, size_t n)       // fails instead of crashing on unmapped memory
{
    if (addr < 0x1000) return false;
    struct iovec lv = {out, n}, rv = {reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &lv, 1, &rv, 1, 0) == (ssize_t)n;
}

bool InModule(uint64_t v) { return v >= g_modLo && v < g_modHi; }
bool InText(uint64_t v) { for (auto& s : g_segs) if ((s.flags & PF_X) && v >= s.lo && v < s.hi) return true; return false; }

// --- GObjects -----------------------------------------------------------------------------------
bool ObjArrayOk(uintptr_t a)
{
    uint8_t h[0x30];
    if (!SafeRead(a, h, sizeof(h))) return false;
    int32_t max = *reinterpret_cast<int32_t*>(h + 0x20), num = *reinterpret_cast<int32_t*>(h + 0x24);
    int32_t maxc = *reinterpret_cast<int32_t*>(h + 0x28), numc = *reinterpret_cast<int32_t*>(h + 0x2c);
    uint64_t objs = *reinterpret_cast<uint64_t*>(h + 0x10);
    if (num < 1000 || num > 8000000 || max < num || max > 16000000) return false;
    if (numc <= 0 || maxc < numc || (int64_t)numc * 65536 < num || (int64_t)(numc - 1) * 65536 > num) return false;
    if (!objs || (objs & 7)) return false;
    uint64_t chunk0 = 0, obj0 = 0;
    if (!SafeRead(objs, &chunk0, 8) || !chunk0) return false;
    if (!SafeRead(chunk0, &obj0, 8)) return false;
    uint64_t vt = 0;
    return obj0 && SafeRead(obj0, &vt, 8) && InModule(vt);        // the first object is a UObject with a vtable inside the engine
}

// --- GNames -------------------------------------------------------------------------------------
bool NamesOk(uintptr_t base, uintptr_t pool)
{
    uint64_t p = 0;
    if (!SafeRead(pool + 0x40, &p, 8) || InModule(p)) return false;
    uint8_t e[6];
    if (!SafeRead(p, e, 6)) return false;
    if ((e[0] & 1) || e[1] != 0x01 || memcmp(e + 2, "None", 4) != 0) return false;      // FNameEntryHeader{Len=4}, "None"
    uintptr_t old = Offsets::GNames;
    Offsets::GNames = pool - base;
    bool ok = InSDKUtils::GetNameByIndex(0) == "None";
    if (!ok) Offsets::GNames = old;
    return ok;
}

uintptr_t g_foundObjects = 0, g_foundNames = 0;
}

bool Resolver_ResolveCore(uintptr_t base, std::string& report)
{
    LoadSegs();
    if (g_segs.empty()) { report = "module segments not found"; return false; }
    report.clear();
    // test hook (dev builds): <root>/dev/force_scan makes the dumped offsets be ignored, to exercise the scanning path
    static const bool forceScan = access((KModRoot() + "/dev/force_scan").c_str(), F_OK) == 0;
    if (forceScan && !g_foundObjects) { Offsets::GObjects = 0x10; Offsets::GNames = 0x10; Offsets::ProcessEvent = 0x10; report += "(force_scan) "; }

    // GObjects
    if (!g_foundObjects)
    {
        if (ObjArrayOk(base + Offsets::GObjects)) { g_foundObjects = base + Offsets::GObjects; KLog(KLOG_INFO, "resolver", "GObjects: dumped offset 0x%lx is valid", (unsigned long)Offsets::GObjects); }
        else
        {
            for (auto& s : g_segs)
            {
                if (!(s.flags & PF_W) || g_foundObjects) continue;
                for (uintptr_t a = (s.lo + 7) & ~7ul; a + 0x30 <= s.hi; a += 8)
                {
                    int32_t num = *reinterpret_cast<int32_t*>(a + 0x24), max = *reinterpret_cast<int32_t*>(a + 0x20);
                    if (num < 1000 || num > 8000000 || max < num) continue;
                    if (ObjArrayOk(a)) { g_foundObjects = a; break; }
                }
            }
            if (g_foundObjects) { Offsets::GObjects = g_foundObjects - base; KLog(KLOG_INFO, "resolver", "GObjects: found by scanning at 0x%lx", (unsigned long)Offsets::GObjects); }
        }
    }
    if (!g_foundObjects) { report += "GObjects not found"; return false; }

    // GNames
    if (!g_foundNames)
    {
        if (NamesOk(base, base + Offsets::GNames)) { g_foundNames = base + Offsets::GNames; KLog(KLOG_INFO, "resolver", "GNames: dumped offset 0x%lx is valid", (unsigned long)Offsets::GNames); }
        else
        {
            for (auto& s : g_segs)
            {
                if (!(s.flags & PF_W) || g_foundNames) continue;
                for (uintptr_t a = (s.lo + 7) & ~7ul; a + 8 <= s.hi; a += 8)
                {
                    uint64_t p = *reinterpret_cast<uint64_t*>(a);
                    if (p < 0x10000 || InModule(p) || a < s.lo + 0x40) continue;
                    if (NamesOk(base, a - 0x40)) { g_foundNames = a - 0x40; break; }
                }
            }
            if (g_foundNames) KLog(KLOG_INFO, "resolver", "GNames: found by scanning at 0x%lx", (unsigned long)Offsets::GNames);
        }
    }
    if (!g_foundNames) { report += "GNames not found"; return false; }

    // ProcessEvent: slot ProcessEventIdx of any UObject's vtable (stable for an engine version, unlike the code address)
    for (int i = 1; i < 2000; i++)
    {
        UObject* o = InSDKUtils::GetObjectByIndex(i);
        if (!o) continue;
        uint64_t vt = 0, fn = 0;
        if (!SafeRead(reinterpret_cast<uintptr_t>(o), &vt, 8) || !InModule(vt)) continue;
        if (!SafeRead(vt + 8ul * Offsets::ProcessEventIdx, &fn, 8) || !InText(fn)) continue;
        uintptr_t off = fn - base;
        char b[100];
        snprintf(b, sizeof(b), "ProcessEvent: vtable slot -> 0x%lx (dumped 0x%lx)", (unsigned long)off, (unsigned long)Offsets::ProcessEvent);
        report += b;
        Offsets::ProcessEvent = off;
        Offsets::GWorld = 0;
        return true;
    }
    report += "ProcessEvent not found";
    return false;
}

// -------------------------------------------------------------------------------------------------
// Pak layer
// -------------------------------------------------------------------------------------------------
namespace
{
// Does the function at `fn` load the address `target` (ADRP + ADD) in its first 0x80 bytes?
bool LoadsAddress(uintptr_t fn, uintptr_t target)
{
    uint32_t code[32];
    if (!SafeRead(fn, code, sizeof(code))) return false;
    for (int i = 0; i + 1 < 32; i++)
    {
        uint32_t ins = code[i];
        if ((ins & 0x9f000000) != 0x90000000) continue;                  // ADRP
        int rd = ins & 31;
        int64_t imm = (((ins >> 5) & 0x7ffff) << 2) | ((ins >> 29) & 3);
        if (imm & (1 << 20)) imm -= 1 << 21;
        uintptr_t page = ((fn + i * 4) & ~0xfffUL) + (imm << 12);
        for (int j = i + 1; j < i + 4 && j < 32; j++)
        {
            uint32_t a = code[j];
            if ((a & 0xff800000) == 0x91000000 && ((a >> 5) & 31) == (uint32_t)rd && page + ((a >> 10) & 0xfff) == target) return true;
        }
    }
    return false;
}

// The worker reads its argument struct from x1: ldr xN,[x1] (path) and ldr wM,[x1,#8] (order) near its start.
bool ReadsMountArgs(uintptr_t fn)
{
    uint32_t code[40];
    if (!SafeRead(fn, code, sizeof(code))) return false;
    if (code[0] != 0xA9BA7BFD) return false;                              // stp x29, x30, [sp, #-0x60]!
    bool path = false, order = false;
    for (int i = 0; i < 40; i++)
    {
        if ((code[i] & 0xFFFFFFE0) == 0xF9400020) path = true;           // ldr x?, [x1]
        if ((code[i] & 0xFFFFFFE0) == 0xB9400820) order = true;          // ldr w?, [x1, #8]
    }
    return path && order;
}
}

bool Resolver_ResolvePak(uintptr_t base, PakAddrs& out, std::string& report)
{
    LoadSegs();
    // 1) the log string used by the mount function
    static const char16_t kStr[] = u"Mounting pak file: %s";
    uintptr_t strAddr = 0;
    for (auto& s : g_segs)
    {
        if ((s.flags & PF_W) || (s.flags & PF_X)) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(s.lo);
        const uint8_t* hit = static_cast<const uint8_t*>(memmem(p, s.hi - s.lo, kStr, sizeof(kStr) - 2));
        if (hit) { strAddr = reinterpret_cast<uintptr_t>(hit); break; }
    }
    if (!strAddr) { report = "log string not found"; return false; }

    // 2) the delegate object the engine bound to FCoreDelegates::MountPak: { vtable, ., handle, this, function }
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) { report = "cannot read maps"; return false; }
    std::vector<uint8_t> buf(1 << 20);
    std::unordered_set<uint64_t> seen;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), maps))
    {
        unsigned long lo, hi; char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        if (strstr(line, ".so") || strstr(line, "/system/") || strstr(line, "/apex/") || strstr(line, "/dev/")) continue;
        for (unsigned long a = lo; a < hi && !found; a += buf.size() - 0x40)
        {
            size_t want = std::min<unsigned long>(buf.size(), hi - a);
            struct iovec lv = {buf.data(), want}, rv = {reinterpret_cast<void*>(a), want};
            ssize_t got = process_vm_readv(getpid(), &lv, 1, &rv, 1, 0);
            if (got < 0x30) continue;
            for (size_t i = 0; i + 0x30 <= (size_t)got; i += 8)
            {
                uint64_t vt = *reinterpret_cast<uint64_t*>(&buf[i]);
                if (!InModule(vt)) continue;
                uint64_t fn = *reinterpret_cast<uint64_t*>(&buf[i + 0x20]);
                if (!InText(fn) || !seen.insert(fn).second) continue;
                if (!LoadsAddress(fn, strAddr)) continue;
                out.mountFn = fn;
                out.pakFile = reinterpret_cast<void*>(*reinterpret_cast<uint64_t*>(&buf[i + 0x18]));
                found = true;
                break;
            }
        }
    }
    fclose(maps);
    if (!found) { report = "delegate object not found"; return false; }

    // 3) the worker: a BL target inside the function that reads the argument struct
    uint32_t code[64];
    if (!SafeRead(out.mountFn, code, sizeof(code))) { report = "cannot read the mount function"; return false; }
    for (int i = 0; i < 64; i++)
    {
        if ((code[i] & 0xfc000000) != 0x94000000) continue;               // BL
        int64_t imm = code[i] & 0x3ffffff;
        if (imm & 0x2000000) imm -= 0x4000000;
        uintptr_t target = out.mountFn + i * 4 + imm * 4;
        if (InText(target) && ReadsMountArgs(target)) out.worker = target;     // keep the last match
    }
    if (!out.worker) { report = "mount worker not identified"; return false; }
    char b[160];
    snprintf(b, sizeof(b), "mount fn 0x%lx worker 0x%lx pak file %p", (unsigned long)(out.mountFn - base), (unsigned long)(out.worker - base), out.pakFile);
    report = b;
    return true;
}
