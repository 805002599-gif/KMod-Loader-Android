// Lua host: reflection-driven UObject proxies, UE4SS-style globals, hooks, timers, mod loader, dev console.
// API names follow UE4SS (docs/lua-api) so existing knowledge/scripts carry over.
#include "lua_host.h"

#include <dirent.h>
#include <dlfcn.h>
#include <zlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "Engine_classes.hpp"
#include "modloader_lua.h"    // generated from lua/modloader.lua  (kModLoaderLua)
#include "modmanager_lua.h"   // generated from lua/modmanager.lua (kModManagerLua)
#include "commands_lua.h"       // generated from lua/commands.lua (kCommandsLua)

using namespace SDK;

// UE4SS' Lua build calls these lock hooks (its luauser.c is Windows-only). We only ever run Lua on the game thread.
extern "C" {
void LuaLockInitial(lua_State*) {}
void LuaLock(lua_State*) {}
void LuaUnlock(lua_State*) {}
}

// ---------------------------------------------------------------------------------------------
// utf helpers
// ---------------------------------------------------------------------------------------------
std::u16string Utf8To16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) { cp -= 0x10000; out += char16_t(0xD800 + (cp >> 10)); out += char16_t(0xDC00 + (cp & 0x3FF)); }
        else out += char16_t(cp);
    }
    return out;
}

std::string Utf16To8(const char16_t* s, size_t len)
{
    std::string out;
    for (size_t i = 0; i < len; i++)
    {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00); }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reflection helpers
// ---------------------------------------------------------------------------------------------
namespace
{
enum PT
{
    T_Unknown, T_Bool, T_Int8, T_Byte, T_Int16, T_UInt16, T_Int, T_UInt32, T_Int64, T_UInt64, T_Float, T_Double,
    T_Name, T_Str, T_Text, T_Object, T_Class, T_Weak, T_Soft, T_Interface, T_Struct, T_Array, T_Enum,
    T_Delegate, T_Multicast, T_Map, T_Set
};

constexpr uint64_t CPF_ConstParm = 0x2, CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

PT TypeOf(FProperty* p)
{
    static std::unordered_map<std::string, PT> m = {
        {"BoolProperty", T_Bool}, {"Int8Property", T_Int8}, {"ByteProperty", T_Byte}, {"Int16Property", T_Int16},
        {"UInt16Property", T_UInt16}, {"IntProperty", T_Int}, {"UInt32Property", T_UInt32}, {"Int64Property", T_Int64},
        {"UInt64Property", T_UInt64}, {"FloatProperty", T_Float}, {"DoubleProperty", T_Double}, {"NameProperty", T_Name},
        {"StrProperty", T_Str}, {"TextProperty", T_Text}, {"ObjectProperty", T_Object}, {"ClassProperty", T_Class},
        {"WeakObjectProperty", T_Weak}, {"SoftObjectProperty", T_Soft}, {"SoftClassProperty", T_Soft},
        {"InterfaceProperty", T_Interface}, {"StructProperty", T_Struct}, {"ArrayProperty", T_Array},
        {"EnumProperty", T_Enum}, {"DelegateProperty", T_Delegate}, {"MulticastInlineDelegateProperty", T_Multicast},
        {"MulticastSparseDelegateProperty", T_Multicast}, {"MapProperty", T_Map}, {"SetProperty", T_Set},
        {"ObjectPtrProperty", T_Object}, {"ClassPtrProperty", T_Class}, {"LazyObjectProperty", T_Weak}};
    // cache by FFieldClass pointer
    static std::unordered_map<FFieldClass*, PT> cache;
    auto it = cache.find(p->ClassPrivate);
    if (it != cache.end()) return it->second;
    auto f = m.find(p->ClassPrivate->Name.ToString());
    PT t = f == m.end() ? T_Unknown : f->second;
    cache[p->ClassPrivate] = t;
    return t;
}

std::string FNameStr(const FName& n)
{
    std::string s = n.ToString();
    if (n.Number > 0) s += "_" + std::to_string(n.Number - 1);
    return s;
}

UClass* FindClassCached(const std::string& name)
{
    static std::unordered_map<std::string, UClass*> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    UClass* c = UObject::FindClassFast(name);
    if (c) cache[name] = c;
    return c;
}

FProperty* FindProp(UStruct* s, const std::string& name)
{
    for (UStruct* c = s; c; c = c->SuperStruct)
        for (FField* f = c->ChildProperties; f; f = f->Next)
            if (f->Name.ToString() == name) return static_cast<FProperty*>(f);
    return nullptr;
}

UFunction* FindFunc(UStruct* s, const std::string& name)
{
    UClass* fcls = UFunction::StaticClass();
    for (UStruct* c = s; c; c = c->SuperStruct)
        for (UField* f = c->Children; f; f = f->Next)
            if (f->Class == fcls && f->Name.ToString() == name) return static_cast<UFunction*>(f);
    return nullptr;
}

uint32_t FuncParmsSize(UFunction* fn)
{
    uint32_t sz = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(fn) + 0xB6);
    for (FField* f = fn->ChildProperties; f; f = f->Next)
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (p->PropertyFlags & CPF_Parm) sz = std::max<uint32_t>(sz, p->Offset + p->ElementSize * std::max(1, p->ArrayDim));
    }
    return sz;
}

struct RawArray { void* Data; int32_t Num; int32_t Max; };

// -- engine-side string helpers (call the BP library UFunctions so the engine allocates) -----------
FName MakeFName(const std::string& s)
{
    static UFunction* fn = nullptr;
    static UObject* cdo = nullptr;
    if (!fn)
    {
        UClass* c = FindClassCached("KismetStringLibrary");
        if (c) { fn = FindFunc(c, "Conv_StringToName"); cdo = UKismetStringLibrary::GetDefaultObj(); }
    }
    FName out;
    if (!fn || !cdo) return out;
    std::u16string u = Utf8To16(s);
    struct { FString In; FName Ret; } p;
    memset(&p, 0, sizeof(p));
    int32_t n = (int32_t)u.size() + 1;
    p.In = FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n);
    cdo->ProcessEvent(fn, &p);
    return p.Ret;
}

FText MakeFText(const std::string& s)
{
    static UFunction* fn = nullptr;
    static UObject* cdo = nullptr;
    if (!fn)
    {
        fn = UKismetTextLibrary::StaticClass()->GetFunction("KismetTextLibrary", "Conv_StringToText");
        cdo = UKismetTextLibrary::GetDefaultObj();
    }
    std::u16string u = Utf8To16(s);
    struct { FString In; FText Ret; } p;
    memset(&p, 0, sizeof(p));
    int32_t n = (int32_t)u.size() + 1;
    p.In = FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n);
    cdo->ProcessEvent(fn, &p);
    FText out; memcpy(&out, &p.Ret, sizeof(FText));
    return out;
}

// ---------------------------------------------------------------------------------------------
// Lua object proxies
// ---------------------------------------------------------------------------------------------
lua_State* L_ = nullptr;
constexpr const char* MT_OBJECT = "kmod.UObject";
constexpr const char* MT_PARAMS = "kmod.Params";

struct LuaObj { UObject* p; int32_t idx; };
struct LuaParams { UFunction* fn; uint8_t* parms; };

void PushObject(lua_State* L, UObject* o)
{
    LuaObj* u = static_cast<LuaObj*>(lua_newuserdatauv(L, sizeof(LuaObj), 0));
    u->p = o; u->idx = o ? o->Index : -1;
    luaL_setmetatable(L, MT_OBJECT);
}

bool ObjAlive(const LuaObj* o)
{
    if (!o || !o->p || o->idx < 0) return false;
    FUObjectItem* it = ObjRef::ItemAt(o->idx);
    return it && it->Object == o->p && ObjRef::ItemUsable(it);
}

LuaObj* ToLuaObj(lua_State* L, int i) { return static_cast<LuaObj*>(luaL_testudata(L, i, MT_OBJECT)); }

UObject* CheckObj(lua_State* L, int i)
{
    LuaObj* o = ToLuaObj(L, i);
    if (!o) luaL_error(L, "expected UObject");
    if (!ObjAlive(o)) luaL_error(L, "UObject is invalid (destroyed or null)");
    return o->p;
}

void PushFromAddr(lua_State* L, FProperty* p, const uint8_t* addr);

void PushStruct(lua_State* L, UStruct* s, const uint8_t* base)
{
    lua_newtable(L);
    for (UStruct* c = s; c; c = c->SuperStruct)
        for (FField* f = c->ChildProperties; f; f = f->Next)
        {
            FProperty* fp = static_cast<FProperty*>(f);
            PushFromAddr(L, fp, base + fp->Offset);
            lua_setfield(L, -2, f->Name.ToString().c_str());
        }
}

void PushFromAddr(lua_State* L, FProperty* p, const uint8_t* addr)
{
    switch (TypeOf(p))
    {
    case T_Bool: { auto* b = static_cast<FBoolProperty*>(p); lua_pushboolean(L, (addr[b->ByteOffset] & b->FieldMask) != 0); break; }
    case T_Int8: lua_pushinteger(L, *reinterpret_cast<const int8_t*>(addr)); break;
    case T_Byte: lua_pushinteger(L, *addr); break;
    case T_Int16: lua_pushinteger(L, *reinterpret_cast<const int16_t*>(addr)); break;
    case T_UInt16: lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(addr)); break;
    case T_Int: lua_pushinteger(L, *reinterpret_cast<const int32_t*>(addr)); break;
    case T_UInt32: lua_pushinteger(L, *reinterpret_cast<const uint32_t*>(addr)); break;
    case T_Int64: case T_UInt64: lua_pushinteger(L, *reinterpret_cast<const int64_t*>(addr)); break;
    case T_Float: lua_pushnumber(L, *reinterpret_cast<const float*>(addr)); break;
    case T_Double: lua_pushnumber(L, *reinterpret_cast<const double*>(addr)); break;
    case T_Enum:
        switch (p->ElementSize)
        {
        case 1: lua_pushinteger(L, *addr); break;
        case 2: lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(addr)); break;
        case 4: lua_pushinteger(L, *reinterpret_cast<const int32_t*>(addr)); break;
        default: lua_pushinteger(L, *reinterpret_cast<const int64_t*>(addr)); break;
        }
        break;
    case T_Name: lua_pushstring(L, FNameStr(*reinterpret_cast<const FName*>(addr)).c_str()); break;
    case T_Str:
    {
        const FString* s = reinterpret_cast<const FString*>(addr);
        std::string u = s->ToString();
        lua_pushlstring(L, u.data(), u.size());
        break;
    }
    case T_Text:
    {
        const FText* t = reinterpret_cast<const FText*>(addr);
        if (t->TextData) lua_pushstring(L, t->ToString().c_str()); else lua_pushstring(L, "");
        break;
    }
    case T_Object: case T_Class: case T_Interface: PushObject(L, *reinterpret_cast<UObject* const*>(addr)); break;
    case T_Weak:
    {
        int32_t index = *reinterpret_cast<const int32_t*>(addr);
        FUObjectItem* it = index >= 0 ? ObjRef::ItemAt(index) : nullptr;
        PushObject(L, (it && ObjRef::ItemUsable(it)) ? it->Object : nullptr);
        break;
    }
    case T_Struct: PushStruct(L, static_cast<FStructProperty*>(p)->Struct, addr); break;
    case T_Array:
    {
        FArrayProperty* ap = static_cast<FArrayProperty*>(p);
        const RawArray* arr = reinterpret_cast<const RawArray*>(addr);
        lua_newtable(L);
        if (arr->Data && arr->Num > 0 && arr->Num < 100000)
            for (int32_t i = 0; i < arr->Num; i++)
            {
                PushFromAddr(L, ap->InnerProperty, static_cast<const uint8_t*>(arr->Data) + (size_t)i * ap->InnerProperty->ElementSize);
                lua_rawseti(L, -2, i + 1);
            }
        break;
    }
    default: lua_pushnil(L); break;
    }
}

// isParam: the memory is a temporary call buffer (the engine copies strings out of it), so FString/FText may point at our temps.
bool SetFromLua(lua_State* L, int idx, FProperty* p, uint8_t* addr, bool isParam, std::deque<std::u16string>& keep, std::string& err)
{
    idx = lua_absindex(L, idx);
    switch (TypeOf(p))
    {
    case T_Bool:
    {
        auto* b = static_cast<FBoolProperty*>(p);
        if (lua_toboolean(L, idx)) addr[b->ByteOffset] |= b->FieldMask; else addr[b->ByteOffset] &= ~b->FieldMask;
        return true;
    }
    case T_Int8: case T_Byte: *addr = (uint8_t)luaL_checkinteger(L, idx); return true;
    case T_Int16: case T_UInt16: *reinterpret_cast<int16_t*>(addr) = (int16_t)luaL_checkinteger(L, idx); return true;
    case T_Int: case T_UInt32: *reinterpret_cast<int32_t*>(addr) = (int32_t)luaL_checkinteger(L, idx); return true;
    case T_Int64: case T_UInt64: *reinterpret_cast<int64_t*>(addr) = (int64_t)luaL_checkinteger(L, idx); return true;
    case T_Float: *reinterpret_cast<float*>(addr) = (float)luaL_checknumber(L, idx); return true;
    case T_Double: *reinterpret_cast<double*>(addr) = luaL_checknumber(L, idx); return true;
    case T_Enum:
    {
        int64_t v = (int64_t)luaL_checkinteger(L, idx);
        switch (p->ElementSize)
        {
        case 1: *addr = (uint8_t)v; break;
        case 2: *reinterpret_cast<int16_t*>(addr) = (int16_t)v; break;
        case 4: *reinterpret_cast<int32_t*>(addr) = (int32_t)v; break;
        default: *reinterpret_cast<int64_t*>(addr) = v; break;
        }
        return true;
    }
    case T_Name: *reinterpret_cast<FName*>(addr) = MakeFName(luaL_checkstring(L, idx)); return true;
    case T_Str:
        if (!isParam) { err = "writing FString properties on objects is not supported (use a setter function)"; return false; }
        keep.push_back(Utf8To16(luaL_checkstring(L, idx)));
        { int32_t n = (int32_t)keep.back().size() + 1; *reinterpret_cast<FString*>(addr) = FString(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(keep.back().c_str())), n, n); }
        return true;
    case T_Text:
        if (!isParam) { err = "writing FText properties on objects is not supported (use a setter function)"; return false; }
        *reinterpret_cast<FText*>(addr) = MakeFText(luaL_checkstring(L, idx));
        return true;
    case T_Object: case T_Class: case T_Interface:
    {
        if (lua_isnil(L, idx)) { *reinterpret_cast<UObject**>(addr) = nullptr; return true; }
        LuaObj* o = ToLuaObj(L, idx);
        if (!o) { err = "expected UObject or nil"; return false; }
        *reinterpret_cast<UObject**>(addr) = ObjAlive(o) ? o->p : nullptr;
        return true;
    }
    case T_Struct:
    {
        if (!lua_istable(L, idx)) { err = "expected table for struct"; return false; }
        UStruct* s = static_cast<FStructProperty*>(p)->Struct;
        for (UStruct* c = s; c; c = c->SuperStruct)
            for (FField* f = c->ChildProperties; f; f = f->Next)
            {
                lua_getfield(L, idx, f->Name.ToString().c_str());
                bool ok = true;
                if (!lua_isnil(L, -1)) { FProperty* fp = static_cast<FProperty*>(f); ok = SetFromLua(L, -1, fp, addr + fp->Offset, isParam, keep, err); }
                lua_pop(L, 1);
                if (!ok) return false;
            }
        return true;
    }
    default: err = std::string("unsupported property type ") + p->ClassPrivate->Name.ToString(); return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Calling UFunctions from Lua:  obj:Func(args...)  -> return value, then out params
// ---------------------------------------------------------------------------------------------
int CallUFunctionImpl(lua_State* L, UObject* self, UFunction* fn, int firstArg)
{
    uint32_t sz = FuncParmsSize(fn);
    std::vector<uint8_t> buf(sz + 64, 0);
    std::deque<std::u16string> keep;
    int arg = firstArg;
    FProperty* ret = nullptr;
    std::vector<FProperty*> outs;
    std::string err;

    for (FField* f = fn->ChildProperties; f; f = f->Next)
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (!(p->PropertyFlags & CPF_Parm)) continue;
        if (p->PropertyFlags & CPF_ReturnParm) { ret = p; continue; }
        bool isOut = (p->PropertyFlags & CPF_OutParm) && !(p->PropertyFlags & CPF_ConstParm);
        if (isOut) { outs.push_back(p); continue; }
        if (arg > lua_gettop(L)) luaL_error(L, "missing argument '%s' for %s", f->Name.ToString().c_str(), fn->GetName().c_str());
        if (!SetFromLua(L, arg, p, buf.data() + p->Offset, true, keep, err))
            luaL_error(L, "argument '%s' of %s: %s", f->Name.ToString().c_str(), fn->GetName().c_str(), err.c_str());
        arg++;
    }

    self->ProcessEvent(fn, buf.data());

    int n = 0;
    if (ret) { PushFromAddr(L, ret, buf.data() + ret->Offset); n++; }
    for (FProperty* o : outs) { PushFromAddr(L, o, buf.data() + o->Offset); n++; }
    return n;
}

int CallUFunction(lua_State* L)
{
    UFunction* fn = static_cast<UFunction*>(lua_touserdata(L, lua_upvalueindex(1)));
    UObject* self = CheckObj(L, 1);
    return CallUFunctionImpl(L, self, fn, 2);
}

// ---------------------------------------------------------------------------------------------
// UObject methods
// ---------------------------------------------------------------------------------------------
int M_IsValid(lua_State* L) { LuaObj* o = ToLuaObj(L, 1); lua_pushboolean(L, o && ObjAlive(o)); return 1; }
int M_GetFullName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, o->GetFullName().c_str()); return 1; }
int M_GetName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, o->GetName().c_str()); return 1; }
// Package-qualified path ("/Game/UI/Foo.Foo_C", "/Game/Maps/M.M:PersistentLevel"), unlike GetFullName which drops package paths.
int M_GetPathName(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    std::vector<UObject*> chain;
    for (UObject* c = o; c && chain.size() < 32; c = c->Outer) chain.push_back(c);
    std::string path;
    for (size_t i = chain.size(); i-- > 0;)
    {
        if (i + 1 == chain.size()) path = InSDKUtils::GetNameByIndex(chain[i]->Name.GetDisplayIndex());
        else path += (i + 2 == chain.size() ? "." : ":") + InSDKUtils::GetNameByIndex(chain[i]->Name.GetDisplayIndex());
    }
    lua_pushstring(L, path.c_str());
    return 1;
}
int M_GetClass(lua_State* L) { UObject* o = CheckObj(L, 1); PushObject(L, o->Class); return 1; }
int M_GetOuter(lua_State* L) { UObject* o = CheckObj(L, 1); PushObject(L, o->Outer); return 1; }
int M_GetAddress(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushinteger(L, (lua_Integer)(uintptr_t)o); return 1; }
int M_IsA(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UClass* c = nullptr;
    if (lua_type(L, 2) == LUA_TSTRING) c = FindClassCached(lua_tostring(L, 2));
    else if (LuaObj* co = ToLuaObj(L, 2)) c = ObjAlive(co) ? static_cast<UClass*>(co->p) : nullptr;
    lua_pushboolean(L, c && o->IsA(c));
    return 1;
}

// debug helpers
int M_GetProperties(lua_State* L)   // {name = {type=,offset=,size=}, ...} for the class chain; also functions in .functions
{
    UObject* o = CheckObj(L, 1);
    UStruct* s = o->IsA(UStruct::StaticClass()) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(o->Class);
    lua_newtable(L);
    int i = 1;
    for (UStruct* c = s; c; c = c->SuperStruct)
        for (FField* f = c->ChildProperties; f; f = f->Next)
        {
            FProperty* p = static_cast<FProperty*>(f);
            lua_newtable(L);
            lua_pushstring(L, f->Name.ToString().c_str()); lua_setfield(L, -2, "name");
            lua_pushstring(L, p->ClassPrivate->Name.ToString().c_str()); lua_setfield(L, -2, "type");
            lua_pushinteger(L, p->Offset); lua_setfield(L, -2, "offset");
            lua_pushstring(L, c->GetName().c_str()); lua_setfield(L, -2, "owner");
            lua_rawseti(L, -2, i++);
        }
    return 1;
}

int M_GetFunctions(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UStruct* s = o->IsA(UStruct::StaticClass()) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(o->Class);
    UClass* fcls = UFunction::StaticClass();
    lua_newtable(L);
    int i = 1;
    for (UStruct* c = s; c; c = c->SuperStruct)
        for (UField* f = c->Children; f; f = f->Next)
            if (f->Class == fcls)
            {
                lua_pushstring(L, (c->GetName() + "." + f->GetName()).c_str());
                lua_rawseti(L, -2, i++);
            }
    return 1;
}

int M_GetPropertyValue(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    FProperty* p = FindProp(o->Class, luaL_checkstring(L, 2));
    if (!p) { lua_pushnil(L); return 1; }
    PushFromAddr(L, p, reinterpret_cast<uint8_t*>(o) + p->Offset);
    return 1;
}

int M_SetPropertyValue(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    FProperty* p = FindProp(o->Class, luaL_checkstring(L, 2));
    if (!p) return luaL_error(L, "no property '%s'", lua_tostring(L, 2));
    std::deque<std::u16string> keep; std::string err;
    if (!SetFromLua(L, 3, p, reinterpret_cast<uint8_t*>(o) + p->Offset, false, keep, err)) return luaL_error(L, "%s", err.c_str());
    return 0;
}

int M_CallFunction(lua_State* L)   // obj:CallFunction("Name", args...)
{
    UObject* o = CheckObj(L, 1);
    const char* name = luaL_checkstring(L, 2);
    UFunction* fn = FindFunc(o->Class, name);
    if (!fn) return luaL_error(L, "no function '%s' on %s", name, o->Class->GetName().c_str());
    return CallUFunctionImpl(L, o, fn, 3);
}

const luaL_Reg kObjectMethods[] = {
    {"IsValid", M_IsValid}, {"GetFullName", M_GetFullName}, {"GetName", M_GetName}, {"GetFName", M_GetName},
    {"GetClass", M_GetClass}, {"GetPathName", M_GetPathName}, {"GetOuter", M_GetOuter}, {"GetAddress", M_GetAddress}, {"IsA", M_IsA},
    {"GetPropertyValue", M_GetPropertyValue}, {"SetPropertyValue", M_SetPropertyValue}, {"CallFunction", M_CallFunction},
    {"GetProperties", M_GetProperties}, {"GetFunctions", M_GetFunctions}, {nullptr, nullptr}};

int Obj_Index(lua_State* L)
{
    LuaObj* lo = ToLuaObj(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) return 0;
    const char* key = lua_tostring(L, 2);

    // built-in methods first (also work on invalid objects, e.g. IsValid)
    luaL_getmetatable(L, MT_OBJECT);
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 3);

    if (!ObjAlive(lo)) return luaL_error(L, "UObject is invalid (reading '%s')", key);
    UObject* o = lo->p;
    UStruct* cls = o->IsA(UStruct::StaticClass()) ? static_cast<UStruct*>(o) : static_cast<UStruct*>(o->Class);
    if (FProperty* p = FindProp(o->Class, key)) { PushFromAddr(L, p, reinterpret_cast<uint8_t*>(o) + p->Offset); return 1; }
    if (UFunction* fn = FindFunc(o->Class, key)) { lua_pushlightuserdata(L, fn); lua_pushcclosure(L, CallUFunction, 1); return 1; }
    (void)cls;
    lua_pushnil(L);
    return 1;
}

int Obj_NewIndex(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    const char* key = luaL_checkstring(L, 2);
    FProperty* p = FindProp(o->Class, key);
    if (!p) return luaL_error(L, "no property '%s' on %s", key, o->Class->GetName().c_str());
    std::deque<std::u16string> keep; std::string err;
    if (!SetFromLua(L, 3, p, reinterpret_cast<uint8_t*>(o) + p->Offset, false, keep, err)) return luaL_error(L, "%s", err.c_str());
    return 0;
}

int Obj_ToString(lua_State* L)
{
    LuaObj* lo = ToLuaObj(L, 1);
    if (!ObjAlive(lo)) lua_pushstring(L, "UObject(invalid)");
    else lua_pushfstring(L, "UObject: %s", lo->p->GetFullName().c_str());
    return 1;
}

int Obj_Eq(lua_State* L)
{
    LuaObj* a = ToLuaObj(L, 1); LuaObj* b = ToLuaObj(L, 2);
    lua_pushboolean(L, a && b && a->p == b->p);
    return 1;
}

// hook params proxy
int Params_Index(lua_State* L)
{
    LuaParams* lp = static_cast<LuaParams*>(luaL_checkudata(L, 1, MT_PARAMS));
    const char* key = luaL_checkstring(L, 2);
    for (FField* f = lp->fn->ChildProperties; f; f = f->Next)
        if (f->Name.ToString() == key) { FProperty* p = static_cast<FProperty*>(f); PushFromAddr(L, p, lp->parms + p->Offset); return 1; }
    lua_pushnil(L);
    return 1;
}

int Params_NewIndex(lua_State* L)
{
    LuaParams* lp = static_cast<LuaParams*>(luaL_checkudata(L, 1, MT_PARAMS));
    const char* key = luaL_checkstring(L, 2);
    for (FField* f = lp->fn->ChildProperties; f; f = f->Next)
        if (f->Name.ToString() == key)
        {
            std::deque<std::u16string> keep; std::string err;
            if (!SetFromLua(L, 3, static_cast<FProperty*>(f), lp->parms + static_cast<FProperty*>(f)->Offset, false, keep, err)) return luaL_error(L, "%s", err.c_str());
            return 0;
        }
    return luaL_error(L, "no parameter '%s'", key);
}

// ---------------------------------------------------------------------------------------------
// Object search
// ---------------------------------------------------------------------------------------------
bool IsCDO(UObject* o) { return (static_cast<uint32_t>(o->Flags) & 0x10) != 0; }

template <class F>
void ForEachObject(F&& f)
{
    TUObjectArray* a = InSDKUtils::GetGObjects();
    if (!a) return;
    for (int32_t i = 0; i < a->Num(); i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || !o->Class) continue;
        if (f(o)) break;
    }
}

int G_FindFirstOf(lua_State* L)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    UObject* found = nullptr;
    if (c) ForEachObject([&](UObject* o) { if (!IsCDO(o) && o->IsA(c)) { found = o; return true; } return false; });
    PushObject(L, found);
    return 1;
}

int G_FindAllOf(lua_State* L)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    lua_newtable(L);
    int n = 0;
    if (c) ForEachObject([&](UObject* o) { if (!IsCDO(o) && o->IsA(c)) { PushObject(L, o); lua_rawseti(L, -2, ++n); } return false; });
    if (n == 0) { lua_pop(L, 1); lua_pushnil(L); }   // UE4SS returns nil when nothing found
    return 1;
}

// "/Script/Engine.Actor"  |  "/Game/UI/WBP_X.WBP_X_C"  |  "Class Package.Name" -> match against GetFullName() (path components trimmed by the dump format)
std::string NormalizePath(std::string p)
{
    size_t sp = p.find(' ');
    if (sp != std::string::npos) p = p.substr(sp + 1);          // drop leading class name
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) p = p.substr(slash + 1);    // keep last path component onwards
    for (char& ch : p) if (ch == ':') ch = '.';
    return p;
}

UObject* FindByPath(const std::string& path, bool functionOnly)
{
    std::string want = NormalizePath(path);
    UObject* found = nullptr;
    UClass* fcls = UFunction::StaticClass();
    ForEachObject([&](UObject* o) {
        if (functionOnly && o->Class != fcls) return false;
        std::string full = o->GetFullName();
        size_t sp = full.find(' ');
        if (sp == std::string::npos) return false;
        std::string rest = full.substr(sp + 1);
        // exact, or a suffix that starts at a component boundary ("Class_C.Func" matches "Pkg.Class_C.Func")
        if (rest == want || (rest.size() > want.size() && rest.compare(rest.size() - want.size(), want.size(), want) == 0 &&
                             rest[rest.size() - want.size() - 1] == '.')) { found = o; return true; }
        return false;
    });
    return found;
}

int G_StaticFindObject(lua_State* L)
{
    const char* path = lua_isstring(L, lua_gettop(L)) ? lua_tostring(L, lua_gettop(L)) : "";
    PushObject(L, FindByPath(path, false));
    return 1;
}

int G_FindObject(lua_State* L)   // FindObject(className, objectShortName)
{
    UClass* c = FindClassCached(luaL_checkstring(L, 1));
    const char* name = luaL_optstring(L, 2, "");
    UObject* found = nullptr;
    if (c) ForEachObject([&](UObject* o) { if (o->IsA(c) && o->GetName() == name) { found = o; return true; } return false; });
    PushObject(L, found);
    return 1;
}

int G_CreateInvalidObject(lua_State* L) { PushObject(L, nullptr); return 1; }

// LoadAsset(path) -> UObject | nil, err.   Synchronously loads any asset (pak content included) by object path,
// e.g. "/Game/UI/Foo/WBP_Foo.WBP_Foo_C" (a Blueprint class) or "/Game/Art/T_Icon.T_Icon". A path without ".Name" gets
// the package's short name appended. Already-loaded assets are returned as is. Runs on the game thread.
int G_LoadAsset(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    if (path.empty() || path[0] != '/') { lua_pushnil(L); lua_pushstring(L, "asset path must start with '/'"); return 2; }
    if (path.find('.') == std::string::npos) { size_t s = path.rfind('/'); path += "." + path.substr(s + 1); }
    UObject* obj = nullptr;                                       // LoadAsset_Blocking returns at once for loaded assets
    {
        std::u16string u = Utf8To16(path);
        int32_t n = (int32_t)u.size() + 1;
        FString fs(reinterpret_cast<TCHAR*>(const_cast<char16_t*>(u.c_str())), n, n);
        FSoftObjectPath sp = UKismetSystemLibrary::MakeSoftObjectPath(fs);
        TSoftObjectPtr<UObject> ref = UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(sp);
        obj = UKismetSystemLibrary::LoadAsset_Blocking(ref);
    }
    if (!obj) { lua_pushnil(L); lua_pushstring(L, ("could not load " + path).c_str()); return 2; }
    PushObject(L, obj);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Output (logcat + dev console capture)
// ---------------------------------------------------------------------------------------------
std::string g_consoleOut;
bool g_capture = false;

// DEV overlay bridge (exported by libkmod.so)
typedef int (*PopEvalFn)(char*, int);
typedef void (*AppendFn)(const char*);
PopEvalFn g_popEval = nullptr;
AppendFn g_overlayAppend = nullptr;

void ResolveOverlay()
{
    void* h = dlopen("libkmod.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libkmod.so", RTLD_NOW);
    if (!h) return;
    g_popEval = reinterpret_cast<PopEvalFn>(dlsym(h, "KMod_PopEval"));
    g_overlayAppend = reinterpret_cast<AppendFn>(dlsym(h, "KMod_OverlayAppend"));
}

// Output funnel: shared log file + logcat, the dev overlay console and the dev console capture.
void EmitTagged(KLogLevel level, const char* tag, const std::string& s)
{
    KLogMsg(level, tag, s);
    const std::string shown = (strcmp(tag, "lua") == 0) ? s : std::string("[") + tag + "] " + s;
    if (kDevBuild && g_overlayAppend) g_overlayAppend(shown.c_str());
    if (g_capture) { g_consoleOut += shown; g_consoleOut += "\n"; }
}

void EmitLine(const std::string& s)
{
    EmitTagged(s.compare(0, 7, "[error]") == 0 ? KLOG_ERROR : KLOG_INFO, "lua", s);
}

int G_Print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++)
    {
        size_t len; const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += "\t";
        line.append(s, len);
        lua_pop(L, 1);
    }
    EmitLine(line);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Hooks, timers, queued work
// ---------------------------------------------------------------------------------------------
struct HookEntry { int id; int preRef; int postRef; std::string owner; };
std::unordered_map<UFunction*, std::vector<HookEntry>> g_hooks;
int g_nextHookId = 1;

struct Timer { double due; int fnRef; double repeatSec; bool loop; std::string owner; };
std::vector<Timer> g_timers;
double g_now = 0;

struct NewObjWatch { std::string cls; UClass* c; int fnRef; std::string owner; };
std::vector<NewObjWatch> g_newObjWatches;
int32_t g_lastObjCount = 0;

// Per-mod wrappers carry the mod name as upvalue 1 so a mod's hooks/timers can be dropped when it is disabled.
std::string Owner(lua_State* L)
{
    return lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING ? lua_tostring(L, lua_upvalueindex(1)) : "";
}

int G_RegisterHook(lua_State* L)
{
    std::string path = luaL_checkstring(L, 1);
    UFunction* fn = static_cast<UFunction*>(FindByPath(path, true));
    if (!fn) return luaL_error(L, "RegisterHook: function not found: %s", path.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    HookEntry e{g_nextHookId++, LUA_NOREF, LUA_NOREF, Owner(L)};
    lua_pushvalue(L, 2); e.preRef = luaL_ref(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, 3)) { lua_pushvalue(L, 3); e.postRef = luaL_ref(L, LUA_REGISTRYINDEX); }
    g_hooks[fn].push_back(e);
    lua_pushinteger(L, e.id);
    lua_pushinteger(L, e.id);
    return 2;
}

int G_UnregisterHook(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 2 <= lua_gettop(L) && lua_isinteger(L, 2) ? 2 : 1);
    for (auto& kv : g_hooks)
        for (size_t i = 0; i < kv.second.size(); i++)
            if (kv.second[i].id == id)
            {
                luaL_unref(L, LUA_REGISTRYINDEX, kv.second[i].preRef);
                luaL_unref(L, LUA_REGISTRYINDEX, kv.second[i].postRef);
                kv.second.erase(kv.second.begin() + i);
                return 0;
            }
    return 0;
}

int G_ExecuteWithDelay(lua_State* L)
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_timers.push_back({g_now + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), 0, false, Owner(L)});
    return 0;
}

int G_ExecuteInGameThread(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    g_timers.push_back({g_now, luaL_ref(L, LUA_REGISTRYINDEX), 0, false, Owner(L)});
    return 0;
}

int G_LoopAsync(lua_State* L)   // LoopAsync(ms, fn): repeats until fn returns true
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_timers.push_back({g_now + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), ms / 1000.0, true, Owner(L)});
    return 0;
}

int G_NotifyOnNewObject(lua_State* L)
{
    std::string cn = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_newObjWatches.push_back({cn, nullptr, luaL_ref(L, LUA_REGISTRYINDEX), Owner(L)});
    return 0;
}

int TraceHandler(lua_State* L)
{
    const char* m = lua_tostring(L, 1);
    luaL_traceback(L, L, m ? m : "(error object is not a string)", 1);
    return 1;
}

// Runtime errors raised by a mod's hooks/timers/watchers are reported to Lua (kmod._mod_error) so it can disable a mod that keeps failing.
void ReportModError(lua_State* L, const std::string& owner, const std::string& msg)
{
    if (owner.empty()) return;
    int top = lua_gettop(L);
    lua_getglobal(L, "kmod");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "_mod_error");
        if (lua_isfunction(L, -1))
        {
            lua_pushstring(L, owner.c_str()); lua_pushstring(L, msg.c_str());
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
}

bool PCall(lua_State* L, int nargs, int nres, const char* what, const std::string& owner = std::string())
{
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, TraceHandler);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    if (rc != LUA_OK)
    {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        EmitLine(std::string("[error] ") + what + ": " + msg);
        ReportModError(L, owner, msg);
        return false;
    }
    return true;
}

void RunHooks(UObject* self, UFunction* fn, void* parms, bool post)
{
    auto it = g_hooks.find(fn);
    if (it == g_hooks.end() || !L_) return;
    std::vector<HookEntry> copy = it->second;   // callbacks may unregister
    for (const HookEntry& e : copy)
    {
        int ref = post ? e.postRef : e.preRef;
        if (ref == LUA_NOREF || ref == LUA_REFNIL) continue;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
        PushObject(L_, self);
        LuaParams* lp = static_cast<LuaParams*>(lua_newuserdatauv(L_, sizeof(LuaParams), 0));
        lp->fn = fn; lp->parms = static_cast<uint8_t*>(parms);
        luaL_setmetatable(L_, MT_PARAMS);
        PCall(L_, 2, 0, post ? "post-hook" : "pre-hook", e.owner);
    }
}

// ---------------------------------------------------------------------------------------------
// Mod loader: <files>/kmod/mods/<ModName>/Scripts/main.lua ; enabled when <ModName>/enabled.txt exists (UE4SS layout)
// ---------------------------------------------------------------------------------------------
std::string ModsDir() { return ExternalFilesDir() + "/kmod/mods"; }
std::string DevDir() { return ExternalFilesDir() + "/kmod/dev"; }

bool FileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::string ReadFileStr(const std::string& path)
{
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);   // UTF-8 BOM
    return out;
}

// Calls the Lua loader: kmod.load_mods() (migration, inbox install, scan, dependency order, run enabled mods)
void CallLoadMods()
{
    lua_State* L = L_;
    lua_getglobal(L, "kmod");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "load_mods");
        if (lua_isfunction(L, -1)) PCall(L, 0, 0, "load_mods");
        else { lua_pop(L, 1); EmitLine("[error] kmod.load_mods is not defined (modloader.lua failed to load?)"); }
    }
    lua_pop(L, 1);
}

// ---------------------------------------------------------------------------------------------
// Dev console: runs <files>/kmod/dev/console.lua whenever it changes, writes console.out
// ---------------------------------------------------------------------------------------------
time_t g_consoleMtime = 0;
double g_nextConsoleCheck = 0;

void CheckConsole()
{
    std::string path = DevDir() + "/console.lua";
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_mtime == g_consoleMtime) return;
    g_consoleMtime = st.st_mtime;
    std::string src = ReadFileStr(path);
    g_consoleOut.clear();
    g_capture = true;
    LuaHost_RunString(src, "console.lua");
    g_capture = false;
    if (FILE* f = fopen((DevDir() + "/console.out").c_str(), "wb")) { fwrite(g_consoleOut.data(), 1, g_consoleOut.size(), f); fclose(f); }
}


// ---------------------------------------------------------------------------------------------
// kmod.* native helpers (Lua has no directory API)
// ---------------------------------------------------------------------------------------------
int Ccb_ListDir(lua_State* L)   // kmod.listdir(path) -> { {name=, dir=}, ... }
{
    lua_newtable(L);
    int n = 0;
    if (DIR* d = opendir(luaL_checkstring(L, 1)))
    {
        std::string base = lua_tostring(L, 1);
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.') continue;
            struct stat st;
            bool isDir = stat((base + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            lua_newtable(L);
            lua_pushstring(L, e->d_name); lua_setfield(L, -2, "name");
            lua_pushboolean(L, isDir); lua_setfield(L, -2, "dir");
            lua_rawseti(L, -2, ++n);
        }
        closedir(d);
    }
    return 1;
}
int Ccb_Exists(lua_State* L) { struct stat st; lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0); return 1; }
int Ccb_Mkdir(lua_State* L) { lua_pushboolean(L, mkdir(luaL_checkstring(L, 1), 0777) == 0); return 1; }
int Kmod_Log(lua_State* L)   // kmod.log(level, tag, ...): level = "debug" | "info" | "warn" | "error"
{
    const char* lv = luaL_checkstring(L, 1);
    const char* tag = luaL_checkstring(L, 2);
    std::string msg;
    for (int i = 3; i <= lua_gettop(L); i++)
    {
        size_t len; const char* str = luaL_tolstring(L, i, &len);
        if (i > 3) msg += "\t";
        msg.append(str, len);
        lua_pop(L, 1);
    }
    KLogLevel level = strcmp(lv, "debug") == 0 ? KLOG_DEBUG : strcmp(lv, "warn") == 0 ? KLOG_WARN : strcmp(lv, "error") == 0 ? KLOG_ERROR : KLOG_INFO;
    EmitTagged(level, tag, msg);
    return 0;
}
#ifdef KMOD_DEV
// DEV only: crash the process on purpose to test the crash logging (kmod.debug_crash("segv" | "abort"))
int Kmod_DebugCrash(lua_State* L)
{
    const char* kind = luaL_optstring(L, 1, "segv");
    KLog(KLOG_WARN, "debug", "kmod.debug_crash('%s') requested - crashing on purpose", kind);
    if (strcmp(kind, "abort") == 0) abort();
    volatile int* p = nullptr;
    *p = 42;                                   // SIGSEGV
    return 0;
}
#endif
int Ccb_ReloadMods(lua_State*) { LuaHost_ReloadMods(); return 0; }
// ---------------------------------------------------------------------------------------------
// Filesystem helpers for the Lua loader: unzip (.kmod), recursive delete, owner-tagged mod API
// ---------------------------------------------------------------------------------------------
bool MkdirAll(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t Rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool SafeZipName(const std::string& n)
{
    if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= n.size())
    {
        size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        if (n.compare(i, j - i, "..") == 0) return false;     // no path traversal
        i = j + 1;
    }
    return true;
}

// minimal zip reader: stored + deflate, size limits, path-traversal safe
bool UnzipTo(const std::string& zipPath, const std::string& dest, std::string& err)
{
    std::string blob = ReadFileStr(zipPath);
    if (blob.size() < 22 || blob.size() > (128u << 20)) { err = "bad zip size"; return false; }
    const unsigned char* d = reinterpret_cast<const unsigned char*>(blob.data());
    const size_t n = blob.size();
    size_t eocd = std::string::npos;
    for (size_t back = 22; back <= std::min<size_t>(n, 65557); back++)
        if (Rd32(d + (n - back)) == 0x06054b50) { eocd = n - back; break; }
    if (eocd == std::string::npos) { err = "not a zip file"; return false; }
    const uint16_t count = Rd16(d + eocd + 10);
    size_t p = Rd32(d + eocd + 16);
    size_t total = 0;
    for (uint16_t k = 0; k < count; k++)
    {
        if (p + 46 > n || Rd32(d + p) != 0x02014b50) { err = "corrupt central directory"; return false; }
        const uint16_t method = Rd16(d + p + 10);
        const uint32_t csize = Rd32(d + p + 20), usize = Rd32(d + p + 24);
        const uint16_t nlen = Rd16(d + p + 28), elen = Rd16(d + p + 30), clen = Rd16(d + p + 32);
        const uint32_t lho = Rd32(d + p + 42);
        if (p + 46 + nlen > n) { err = "corrupt entry name"; return false; }
        std::string name(reinterpret_cast<const char*>(d + p + 46), nlen);
        p += 46 + nlen + elen + clen;
        for (char& c : name) if (c == '\\') c = '/';
        if (!SafeZipName(name)) { err = "unsafe path in zip: " + name; return false; }
        const std::string full = dest + "/" + name;
        if (name.back() == '/') { MkdirAll(full); continue; }
        if (usize > (32u << 20)) { err = "entry too large: " + name; return false; }
        total += usize;
        if (total > (128u << 20)) { err = "archive too large"; return false; }
        if ((size_t)lho + 30 > n || Rd32(d + lho) != 0x04034b50) { err = "corrupt local header"; return false; }
        const size_t dataOff = (size_t)lho + 30 + Rd16(d + lho + 26) + Rd16(d + lho + 28);
        if (dataOff + csize > n) { err = "truncated entry: " + name; return false; }
        std::string out;
        if (method == 0) out.assign(reinterpret_cast<const char*>(d + dataOff), csize);
        else if (method == 8)
        {
            out.resize(usize);
            z_stream zs{};
            zs.next_in = const_cast<Bytef*>(d + dataOff); zs.avail_in = csize;
            zs.next_out = reinterpret_cast<Bytef*>(&out[0]); zs.avail_out = usize;
            if (inflateInit2(&zs, -15) != Z_OK) { err = "inflateInit failed"; return false; }
            int r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (r != Z_STREAM_END || zs.total_out != usize) { err = "inflate failed: " + name; return false; }
        }
        else { err = "unsupported compression method in " + name; return false; }
        MkdirAll(full.substr(0, full.rfind('/')));
        FILE* f = fopen(full.c_str(), "wb");
        if (!f) { err = "cannot write " + full; return false; }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    return true;
}

void RmTree(const std::string& path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode))
    {
        if (DIR* d = opendir(path.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                std::string nm = e->d_name;
                if (nm == "." || nm == "..") continue;
                RmTree(path + "/" + nm);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    }
    else unlink(path.c_str());
}

int Ccb_Unzip(lua_State* L)    // ok, err = kmod.unzip(zipPath, destDir)
{
    std::string err;
    std::string dest = luaL_checkstring(L, 2);
    MkdirAll(dest);
    if (UnzipTo(luaL_checkstring(L, 1), dest, err)) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0); lua_pushstring(L, err.c_str());
    return 2;
}

int Ccb_Rmtree(lua_State* L)   // only inside <files>/kmod/
{
    std::string path = luaL_checkstring(L, 1);
    std::string root = ExternalFilesDir() + "/kmod/";
    if (path.compare(0, root.size(), root) != 0 || path.find("..") != std::string::npos) return luaL_error(L, "rmtree: refusing path %s", path.c_str());
    RmTree(path);
    return 0;
}

int Ccb_ModApi(lua_State* L)   // kmod.mod_api(modId) -> owner-tagged RegisterHook / ExecuteWithDelay / ... for that mod's environment
{
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    static const struct { const char* n; lua_CFunction f; } reg[] = {
        {"RegisterHook", G_RegisterHook}, {"ExecuteWithDelay", G_ExecuteWithDelay}, {"ExecuteInGameThread", G_ExecuteInGameThread},
        {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject}};
    for (auto& r : reg) { lua_pushstring(L, name); lua_pushcclosure(L, r.f, 1); lua_setfield(L, -2, r.n); }
    return 1;
}



// REPL used by the overlay console: try as an expression first (prints its value), else run as statements.
void RunRepl(const std::string& code)
{
    lua_State* L = L_;
    {   // console commands (kmod.console_exec in console.lua) take precedence over plain Lua
        lua_getglobal(L, "kmod");
        bool handled = false;
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "console_exec");
            if (lua_isfunction(L, -1))
            {
                lua_pushlstring(L, code.data(), code.size());
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) { handled = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
                else { EmitLine(std::string("[error] ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?")); lua_pop(L, 1); handled = true; }
            }
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (handled) return;
    }
    std::string expr = "return " + code;
    if (luaL_loadbuffer(L, expr.data(), expr.size(), "=console") != LUA_OK)
    {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=console") != LUA_OK)
        {
            EmitLine(std::string("[error] ") + lua_tostring(L, -1));
            lua_pop(L, 1);
            return;
        }
    }
    int base = lua_gettop(L) - 1;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK)
    {
        EmitLine(std::string("[error] ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_settop(L, base);
        return;
    }
    int n = lua_gettop(L) - base;
    if (n > 0)
    {
        std::string line = "=";
        for (int i = 1; i <= n; i++)
        {
            size_t len; const char* str = luaL_tolstring(L, base + i, &len);
            line += (i > 1 ? "	" : " ");
            line.append(str, len);
            lua_pop(L, 1);
        }
        EmitLine(line);
    }
    lua_settop(L, base);
}

const char* kPrelude = R"LUA(
kmod = { dev = ... }
-- Generic widget tree dumper (debug): kmod.tree(widget, maxDepth)
function kmod.tree(w, maxDepth, depth, out)
  depth = depth or 0; maxDepth = maxDepth or 6; out = out or {}
  if not w or not w:IsValid() then return out end
  local geo = nil
  local ok, g = pcall(function() return w:GetCachedGeometry() end)
  if ok and g then geo = g end
  local info = string.format("%s%s [%s]", string.rep("  ", depth), w:GetName(), w:GetClass():GetName())
  if geo and geo.Size then
    local ok2, s = pcall(function() return string.format(" size=(%.0f,%.0f) abs=(%.0f,%.0f)", geo.Size.X, geo.Size.Y, geo.AbsolutePosition.X, geo.AbsolutePosition.Y) end)
    if ok2 then info = info .. s end
  end
  out[#out+1] = info
  if depth < maxDepth then
    local okc, n = pcall(function() return w:GetChildrenCount() end)
    if okc and n and n > 0 then
      for i = 0, n - 1 do kmod.tree(w:GetChildAt(i), maxDepth, depth + 1, out) end
    else
      local okt, tree = pcall(function() return w.WidgetTree end)
      if okt and tree and tree:IsValid() then
        local okr, root = pcall(function() return tree.RootWidget end)
        if okr and root and root:IsValid() then kmod.tree(root, maxDepth, depth + 1, out) end
      end
      local okcnt, content = pcall(function() return w:GetContent() end)
      if okcnt and content and content:IsValid() then kmod.tree(content, maxDepth, depth + 1, out) end
    end
  end
  return out
end
function kmod.print_tree(w, maxDepth) for _, l in ipairs(kmod.tree(w, maxDepth)) do print(l) end end
)LUA";

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
void LuaHost_RunString(const std::string& code, const char* chunkName)
{
    if (!L_) return;
    if (luaL_loadbuffer(L_, code.data(), code.size(), chunkName) != LUA_OK)
    {
        EmitLine(std::string("[error] ") + chunkName + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return;
    }
    PCall(L_, 0, 0, chunkName);
}

void LuaHost_Init()
{
    if (L_) return;
    if (kDevBuild) ResolveOverlay();
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    lua_State* L = L_;

    // UObject metatable
    luaL_newmetatable(L, MT_OBJECT);
    lua_pushcfunction(L, Obj_Index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Obj_NewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Obj_ToString); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Obj_Eq); lua_setfield(L, -2, "__eq");
    lua_newtable(L); luaL_setfuncs(L, kObjectMethods, 0); lua_setfield(L, -2, "__methods");
    lua_pop(L, 1);

    luaL_newmetatable(L, MT_PARAMS);
    lua_pushcfunction(L, Params_Index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Params_NewIndex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    static const luaL_Reg globals[] = {
        {"print", G_Print}, {"FindFirstOf", G_FindFirstOf}, {"FindAllOf", G_FindAllOf},
        {"StaticFindObject", G_StaticFindObject}, {"FindObject", G_FindObject}, {"CreateInvalidObject", G_CreateInvalidObject},
        {"LoadAsset", G_LoadAsset},
        {"RegisterHook", G_RegisterHook}, {"UnregisterHook", G_UnregisterHook}, {"ExecuteWithDelay", G_ExecuteWithDelay},
        {"ExecuteInGameThread", G_ExecuteInGameThread}, {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject},
        {nullptr, nullptr}};
    lua_pushglobaltable(L);
    luaL_setfuncs(L, globals, 0);
    lua_pop(L, 1);

    // prelude gets `dev` as its vararg
    std::string prelude = kPrelude;
    if (luaL_loadbuffer(L, prelude.data(), prelude.size(), "=prelude") == LUA_OK)
    {
        lua_pushboolean(L, kDevBuild);
        PCall(L, 1, 0, "prelude");
    }
    else { EmitLine(std::string("[error] prelude: ") + lua_tostring(L, -1)); lua_pop(L, 1); }

    lua_getglobal(L, "kmod");
    lua_pushcfunction(L, Ccb_ListDir); lua_setfield(L, -2, "listdir");
    lua_pushcfunction(L, Ccb_Exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, Ccb_Mkdir); lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, Ccb_ReloadMods); lua_setfield(L, -2, "reload_mods");
    lua_pushcfunction(L, Ccb_Unzip); lua_setfield(L, -2, "unzip");
    lua_pushcfunction(L, Ccb_Rmtree); lua_setfield(L, -2, "rmtree");
    lua_pushcfunction(L, Ccb_ModApi); lua_setfield(L, -2, "mod_api");
    lua_pushcfunction(L, Kmod_Log); lua_setfield(L, -2, "log");
#ifdef KMOD_DEV
    lua_pushcfunction(L, Kmod_DebugCrash); lua_setfield(L, -2, "debug_crash");
#endif
    lua_pushstring(L, KMOD_VERSION); lua_setfield(L, -2, "version");
    lua_pushstring(L, KMOD_FLAVOR); lua_setfield(L, -2, "flavor");
    lua_pushinteger(L, KMOD_API_LEVEL); lua_setfield(L, -2, "api");
    lua_newtable(L);
    lua_pushstring(L, ExternalFilesDir().c_str()); lua_setfield(L, -2, "files");
    lua_pushstring(L, ModsDir().c_str()); lua_setfield(L, -2, "mods");
    lua_pushstring(L, DevDir().c_str()); lua_setfield(L, -2, "dev");
    lua_pushstring(L, KLogDir().c_str()); lua_setfield(L, -2, "logs");
    lua_setfield(L, -2, "paths");
    lua_pop(L, 1);
    mkdir((ExternalFilesDir() + "/kmod").c_str(), 0777);
    mkdir(ModsDir().c_str(), 0777);
    if (kDevBuild) mkdir(DevDir().c_str(), 0777);

    // built-in scripts (owner ""): the mod loader, then the mod manager UI. A dev build lets <dev>/modloader.lua and
    // <dev>/modmanager.lua override the embedded copies.
    {
        struct { const char* file; const char* embedded; const char* chunk; } builtins[] = {
            {"modloader.lua", kModLoaderLua, "=modloader"}, {"commands.lua", kCommandsLua, "=commands"}, {"modmanager.lua", kModManagerLua, "=modmanager"}};
        for (auto& b : builtins)
        {
            std::string code = b.embedded;
            if (kDevBuild) { std::string o = ReadFileStr(DevDir() + "/" + b.file); if (!o.empty()) { code = o; EmitLine(std::string("[builtin] using dev override ") + b.file); } }
            LuaHost_RunString(code, b.chunk);
        }
    }

    g_lastObjCount = InSDKUtils::GetGObjects()->Num();
    EmitLine(std::string("kmod ") + KMOD_VERSION + " (" + KMOD_FLAVOR + "): Lua host ready (" + LUA_VERSION + ")");
    KLogInstallCrashHandlers();
    CallLoadMods();
}

void LuaHost_ReloadMods()
{
    // drop everything registered by user mods (owner != ""); built-in scripts (owner "") stay
    if (!L_) return;
    for (auto& kv : g_hooks)
    {
        auto& v = kv.second;
        for (size_t i = 0; i < v.size();)
            if (!v[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, v[i].preRef); luaL_unref(L_, LUA_REGISTRYINDEX, v[i].postRef); v.erase(v.begin() + i); }
            else i++;
    }
    for (size_t i = 0; i < g_timers.size();)
        if (!g_timers[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_timers[i].fnRef); g_timers.erase(g_timers.begin() + i); } else i++;
    for (size_t i = 0; i < g_newObjWatches.size();)
        if (!g_newObjWatches[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_newObjWatches[i].fnRef); g_newObjWatches.erase(g_newObjWatches.begin() + i); } else i++;
    EmitLine("[mod] reloading mods");
    CallLoadMods();
}

bool LuaHost_HasHook(UFunction* fn) { return !g_hooks.empty() && g_hooks.count(fn) != 0; }
void LuaHost_PreHook(UObject* self, UFunction* fn, void* parms) { RunHooks(self, fn, parms, false); }
void LuaHost_PostHook(UObject* self, UFunction* fn, void* parms) { RunHooks(self, fn, parms, true); }

void LuaHost_Tick(double now)
{
    if (!L_) return;
    g_now = now;

    // due timers (copy: callbacks may add timers)
    std::vector<Timer> due, keep;
    for (auto& t : g_timers) (t.due <= now ? due : keep).push_back(t);
    g_timers = keep;
    for (auto& t : due)
    {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, t.fnRef);
        bool done = true;
        if (PCall(L_, 0, 1, "timer", t.owner)) { done = !t.loop || lua_toboolean(L_, -1); lua_pop(L_, 1); }
        if (t.loop && !done) { Timer n = t; n.due = now + t.repeatSec; g_timers.push_back(n); }
        else luaL_unref(L_, LUA_REGISTRYINDEX, t.fnRef);
    }

    // new-object watches (poll newly allocated GObjects slots)
    if (!g_newObjWatches.empty())
    {
        TUObjectArray* a = InSDKUtils::GetGObjects();
        int32_t cnt = a->Num();
        for (int32_t i = g_lastObjCount; i < cnt; i++)
        {
            FUObjectItem* it = ObjRef::ItemAt(i);
            UObject* o = it ? it->Object : nullptr;
            if (!o || !ObjRef::ItemUsable(it) || !o->Class) continue;
            for (auto& w : g_newObjWatches)
            {
                if (!w.c) w.c = FindClassCached(w.cls);
                if (w.c && !IsCDO(o) && o->IsA(w.c))
                {
                    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.fnRef);
                    PushObject(L_, o);
                    PCall(L_, 1, 0, "NotifyOnNewObject", w.owner);
                }
            }
        }
        g_lastObjCount = cnt;
    }

    if (kDevBuild)
    {
        if (g_popEval)
        {
            static char evalBuf[65536];
            int n;
            while ((n = g_popEval(evalBuf, (int)sizeof(evalBuf))) >= 0) RunRepl(std::string(evalBuf, n));
        }
        if (now >= g_nextConsoleCheck) { g_nextConsoleCheck = now + 0.5; CheckConsole(); }
    }
}
