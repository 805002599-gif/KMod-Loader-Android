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
void LuaHost_PreHook(SDK::UObject* self, SDK::UFunction* fn, void* parms);
void LuaHost_PostHook(SDK::UObject* self, SDK::UFunction* fn, void* parms);
void LuaHost_RunString(const std::string& code, const char* chunkName);
void LuaHost_ReloadMods();
bool KMod_MountPak(const std::string& path);           // core.cpp; game thread: mount one .pak through the engine
void KMod_MountPendingPaks();                           // core.cpp; game thread: mount the paks in kmod/paks that have not been handled yet
std::vector<std::pair<std::string, std::string>> KMod_PakStatus();   // (file name, status) for every .pak in kmod/paks
