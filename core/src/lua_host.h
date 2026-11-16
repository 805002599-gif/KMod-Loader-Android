// Lua host for the kmod mod core: UE4SS-style scripting API on top of UE reflection.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace SDK { class UObject; class UFunction; }

void LuaHost_Init();                                   // game thread; create state, register API, load mods
void LuaHost_Tick(double now);                         // game thread; timers, queued callbacks, dev console
void LuaHost_FastTick(double now);                     // game thread; FastLoop timers and the fast new-object scan (every few ms)
bool LuaHost_HasFast();
bool LuaHost_HasHook(SDK::UFunction* fn);              // cheap check used by the ProcessEvent detour
void LuaHost_PreHook(SDK::UObject* self, SDK::UFunction* fn, void* parms, void* result = nullptr);    // result: script-function hooks only
void LuaHost_PostHook(SDK::UObject* self, SDK::UFunction* fn, void* parms, void* result = nullptr);
void LuaHost_RunString(const std::string& code, const char* chunkName);
void LuaHost_ReloadMods();
bool KMod_DetectLayout(bool force);
bool KMod_LayoutMismatch();
std::vector<std::string> KMod_ScriptDiag(bool start);
bool KMod_EnsureNativeHook(SDK::UFunction* fn, std::string& note);     // core.cpp: thunk for the calls the Blueprint VM makes to a native function
void KMod_NativeStats(unsigned long long& vmCalls, int& slots, bool& haveTable, int& chain);
void KMod_ScriptStats(unsigned long long& pi, unsigned long long& pe, bool& installed);                               // core.cpp: start layout detection in the background (dev: needs kmod/dev/libkmodmd7.so)
std::string KMod_LayoutReport();
bool KMod_MountPak(const std::string& path);           // core.cpp; game thread: mount one .pak through the engine
void KMod_MountPendingPaks();                           // core.cpp; game thread: mount the paks in kmod/paks that have not been handled yet
std::vector<std::pair<std::string, std::string>> KMod_PakStatus();   // (file name, status) for every .pak in kmod/paks
