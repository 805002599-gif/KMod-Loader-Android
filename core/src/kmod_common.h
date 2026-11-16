// Shared helpers for the kmod mod core (Android).
#pragma once
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

constexpr const char* kPackageName = "com.android1939.kardsapk";

inline std::string ExternalFilesDir()
{
    return std::string("/sdcard/Android/data/") + kPackageName + "/files";
}

// The kmod data directory. It lives in the app's *media* folder, which (unlike Android/data) file managers and
// editors can open, and which the game can read and write without any storage permission, including files that other
// apps or adb put there. Other shared folders (Documents, Download) do not work: files not created by the game are unreadable.
inline std::string KModRoot()
{
    return std::string("/sdcard/Android/media/") + kPackageName + "/kmod";
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
