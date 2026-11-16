// A native mod with a settings panel: kmod::ShowPanel builds Android controls from a JSON description; the mod keeps its values in
// a small file in its data folder (api->data_dir, shared storage, survives updates).
#include "kmod.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static bool g_enabled = true;
static double g_volume = 3.5;
static int g_panel = -1;

static std::string SettingsPath() { return std::string(kmod::g_api->data_dir) + "/settings.txt"; }

static void Load()
{
    if (FILE* f = fopen(SettingsPath().c_str(), "r"))
    {
        int en = 1; double vol = 3.5;
        if (fscanf(f, "%d %lf", &en, &vol) == 2) { g_enabled = en != 0; g_volume = vol; }
        fclose(f);
    }
}

static void Save()
{
    if (FILE* f = fopen(SettingsPath().c_str(), "w")) { fprintf(f, "%d %f\n", g_enabled ? 1 : 0, g_volume); fclose(f); }
}

// called on the game thread for every user action in the panel (id = the widget's "id" in the spec)
static void OnPanelEvent(int panel, const char* id, const char* value)
{
    std::string w = id;
    if (w == "enabled") { g_enabled = std::string(value) == "true"; Save(); }
    else if (w == "volume") { g_volume = atof(value); Save(); kmod::g_api->ui_set(panel, "info", ("Volume is now " + std::string(value)).c_str()); }
    else if (w == "reset") { g_enabled = true; g_volume = 3.5; Save(); kmod::g_api->ui_set(panel, "enabled", "true"); kmod::g_api->ui_set(panel, "volume", "3.5"); }
    else if (w == "$close") g_panel = -1;
}

static int LuaOpenPanel(KModLua L)
{
    (void)L;
    if (g_panel > 0) return 0;                            // already open
    char spec[1024];
    snprintf(spec, sizeof(spec), R"({"title":"My panel mod","items":[
        {"type":"text","id":"info","text":"Edits are saved when you change them."},
        {"type":"switch","id":"enabled","text":"Enabled","value":%s},
        {"type":"slider","id":"volume","text":"Volume","min":0,"max":10,"value":%g},
        {"type":"button","id":"reset","text":"Reset to defaults"}]})", g_enabled ? "true" : "false", g_volume);
    g_panel = kmod::ShowPanel(spec, OnPanelEvent);
    return 0;
}

static int LuaVolume(KModLua L) { kmod::g_api->lua.pushnumber(L, g_volume); return 1; }

static int OnStart()
{
    Load();
    kmod::g_api->register_lua_function("open_panel", LuaOpenPanel);
    kmod::g_api->register_lua_function("volume", LuaVolume);
    return 0;
}
KMOD_MOD(OnStart, nullptr)
