// Lua host for the kmod mod core: UE4SS-style scripting API on top of UE reflection.
#pragma once
#include <string>

namespace SDK { class UObject; class UFunction; }

void LuaHost_Init();                                   // game thread; create state, register API, load mods
void LuaHost_Tick(double now);                         // game thread; timers, queued callbacks, dev console
bool LuaHost_HasHook(SDK::UFunction* fn);              // cheap check used by the ProcessEvent detour
void LuaHost_PreHook(SDK::UObject* self, SDK::UFunction* fn, void* parms);
void LuaHost_PostHook(SDK::UObject* self, SDK::UFunction* fn, void* parms);
void LuaHost_RunString(const std::string& code, const char* chunkName);
void LuaHost_ReloadMods();
