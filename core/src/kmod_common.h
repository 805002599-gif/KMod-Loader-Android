// Shared helpers for the kmod mod core (Android).
#pragma once
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>
#include <string>

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"

void Log(const char* fmt, ...);   // logcat (tag KModCore); defined in core.cpp

// Build flavour: define KMOD_DEV for the developer loader (console, verbose logging, hot-load).
#ifdef KMOD_DEV
constexpr bool kDevBuild = true;
#else
constexpr bool kDevBuild = false;
#endif

// The game's package name, read from the process (the process name is the package name; ":service" style suffixes are dropped).
// Nothing in kmod hardcodes it, so the same build works for any repackaged copy of the game.
inline const std::string& KPackageName()
{
    static const std::string name = []
    {
        std::string n;
        if (FILE* f = fopen("/proc/self/cmdline", "r"))
        {
            char buf[256] = {};
            size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            n.assign(buf, strnlen(buf, got));
        }
        size_t colon = n.find(':');
        if (colon != std::string::npos) n.resize(colon);
        bool ok = !n.empty();
        for (char c : n) if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) ok = false;
        return ok ? n : std::string("com.android1939.kardsapk");
    }();
    return name;
}

inline std::string ExternalFilesDir()
{
    return std::string("/sdcard/Android/data/") + KPackageName() + "/files";
}

// The kmod data directory. It lives in the app's *media* folder, which (unlike Android/data) file managers and
// editors can open, and which the game can read and write without any storage permission, including files that other
// apps or adb put there. Other shared folders (Documents, Download) do not work: files not created by the game are unreadable.
inline std::string KModRoot()
{
    return std::string("/sdcard/Android/media/") + KPackageName() + "/kmod";
}

// The app's private storage for kmod: mods are mirrored here from kmod/mods (which is shared storage, not executable and writable by
// other apps) and loaded from here.
inline std::string KPrivateRoot()
{
    return std::string("/data/data/") + KPackageName() + "/files/kmod";
}

// Checked reference to a UObject: survives GC / world teardown by re-validating against GObjects.
struct ObjRef
{
    SDK::UObject* p = nullptr;
    int32_t idx = -1;

    static SDK::FUObjectItem* ItemAt(int32_t i)
    {
        SDK::TUObjectArray* a = SDK::InSDKUtils::GetGObjects();
        if (!a || i < 0 || i >= a->Num()) return nullptr;
        SDK::FUObjectItem* chunk = a->Objects[i / SDK::TUObjectArray::ElementsPerChunk];
        if (!chunk) return nullptr;
        return reinterpret_cast<SDK::FUObjectItem*>(reinterpret_cast<uint8_t*>(chunk) +
               (i % SDK::TUObjectArray::ElementsPerChunk) * SDK::FUObjectItem::Stride);
    }
    static bool ItemUsable(const SDK::FUObjectItem* it)
    {
        int32_t flags = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(it) + 8);
        return !(flags & ((1 << 29) | (1 << 28))); // not Garbage / Unreachable
    }
    void Set(SDK::UObject* o) { p = o; idx = o ? o->Index : -1; }
    void Reset() { p = nullptr; idx = -1; }
    bool Valid() const
    {
        if (!p || idx < 0) return false;
        SDK::FUObjectItem* it = ItemAt(idx);
        return it && it->Object == p && ItemUsable(it);
    }
    template <class T> T* Get() const { return Valid() ? static_cast<T*>(p) : nullptr; }
};

std::u16string Utf8To16(const std::string& s);
std::string Utf16To8(const char16_t* s, size_t len);
