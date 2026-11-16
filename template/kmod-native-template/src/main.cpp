// A minimal kmod native mod: logs, adds a Lua function, hooks a game function and counts calls.
#include "kmod.h"

static int g_calls = 0;

// runs around every call of the hooked function; `params` is the parameter block of that function
static void OnConcat(void* self, void* params, void* result, void* user)
{
    g_calls++;
}

static int LuaAdd(KModLua L)                       // Native.add(a, b)
{
    const KModApi* api = kmod::g_api;
    api->lua.pushinteger(L, api->lua.tointeger(L, 1) + api->lua.tointeger(L, 2));
    return 1;                                      // one result
}

static int LuaCalls(KModLua L)                     // Native.calls()
{
    kmod::g_api->lua.pushinteger(L, g_calls);
    return 1;
}

static int OnStart()
{
    KLOGI("hello from C++ (api level %u)", kmod::g_api->api_level);
    kmod::g_api->register_lua_function("add", LuaAdd);          // only during kmod_start
    kmod::g_api->register_lua_function("calls", LuaCalls);
    kmod::HookFunction("KismetStringLibrary:Concat_StrStr", OnConcat);
    return 0;                                      // non-zero = the mod failed to start
}

static void OnStop() { KLOGI("bye"); }

KMOD_MOD(OnStart, OnStop)
