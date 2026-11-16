// Finds the engine addresses kmod needs at run time instead of trusting offsets that were dumped for one game build.
// Every address is first checked at the dumped offset (fast path); when that does not validate, the module is scanned.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// GObjects, GNames, ProcessEvent (the SDK's writable Offsets::* are filled in) and GWorld (set to 0: the SDK asks the engine).
// Safe to call repeatedly until it returns true. `report` describes what was used.
bool Resolver_ResolveCore(uintptr_t base, std::string& report);

struct PakAddrs
{
    uintptr_t mountFn = 0;      // HandleMountPakDelegate(this, const FString& path, int32 order)
    uintptr_t worker = 0;       // the mount worker; x1 = { const TCHAR* path; int32 order; ... }
    void* pakFile = nullptr;    // the FPakPlatformFile object (`this`)
};
// Identifies this build of the engine library: "<mapped size>-<crc32 of the start of its code>".
std::string Resolver_BuildKey();

// All addresses in the module's memory where `pat` occurs (any segment).
void Resolver_FindBytes(const void* pat, size_t n, std::vector<uintptr_t>& hits);

// Heap scan (about a second). Returns false when the pak layer could not be identified (other game build).
bool Resolver_ResolvePak(uintptr_t base, PakAddrs& out, std::string& report);
