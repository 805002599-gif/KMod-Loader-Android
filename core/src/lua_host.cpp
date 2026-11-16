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
#include <set>
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
#include "json_lua.h"           // generated from lua/json.lua (kJsonLua)
#include "lua_worker.h"

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
std::string PathOf(UObject* o)
{
    std::vector<UObject*> chain;
    for (UObject* c = o; c && chain.size() < 32; c = c->Outer) chain.push_back(c);
    std::string path;
    for (size_t i = chain.size(); i-- > 0;)
    {
        std::string n = InSDKUtils::GetNameByIndex(chain[i]->Name.GetDisplayIndex());
        if (i + 1 == chain.size()) path = n;
        else path += (i + 2 == chain.size() ? "." : ":") + n;
    }
    return path;
}
int M_GetPathName(lua_State* L) { UObject* o = CheckObj(L, 1); lua_pushstring(L, PathOf(o).c_str()); return 1; }
// classObject:GetDefaultObject() -> the class default object (CDO), invalid for non-classes
int M_GetDefaultObject(lua_State* L)
{
    UObject* o = CheckObj(L, 1);
    UObject* cdo = o->IsA(UClass::StaticClass()) ? static_cast<UClass*>(o)->ClassDefaultObject : nullptr;
    PushObject(L, cdo);
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

// ---- type names and function signatures (console: funcs / hook / search) ----
std::string TypeName(FProperty* p, int depth = 0)
{
    if (!p) return "?";
    if (depth > 3) return "...";
    switch (TypeOf(p))
    {
    case T_Bool: return "bool"; case T_Int8: return "int8"; case T_Byte: return "byte"; case T_Int16: return "int16";
    case T_UInt16: return "uint16"; case T_Int: return "int"; case T_UInt32: return "uint32"; case T_Int64: return "int64";
    case T_UInt64: return "uint64"; case T_Float: return "float"; case T_Double: return "double"; case T_Name: return "Name";
    case T_Str: return "String"; case T_Text: return "Text";
    case T_Object: { auto* c = static_cast<FObjectPropertyBase*>(p)->PropertyClass; return "Object<" + (c ? c->GetName() : std::string("?")) + ">"; }
    case T_Class: { auto* c = static_cast<FClassProperty*>(p)->MetaClass; return "Class<" + (c ? c->GetName() : std::string("?")) + ">"; }
    case T_Struct: { auto* st = static_cast<FStructProperty*>(p)->Struct; return "Struct<" + (st ? st->GetName() : std::string("?")) + ">"; }
    case T_Enum: { auto* e = static_cast<FEnumProperty*>(p)->Enum; return "Enum<" + (e ? e->GetName() : std::string("?")) + ">"; }
    case T_Array: return "Array<" + TypeName(static_cast<FArrayProperty*>(p)->InnerProperty, depth + 1) + ">";
    case T_Map: { auto* m = static_cast<FMapProperty*>(p); return "Map<" + TypeName(m->KeyProperty, depth + 1) + "," + TypeName(m->ValueProperty, depth + 1) + ">"; }
    case T_Set: return "Set<" + TypeName(static_cast<FSetProperty*>(p)->ElementProperty, depth + 1) + ">";
    default: { std::string n = p->ClassPrivate->Name.ToString(); size_t k = n.rfind("Property"); return k != std::string::npos ? n.substr(0, k) : n; }
    }
}

struct ParamInfo { std::string name, type; bool out, ret, isConst; };
std::vector<ParamInfo> ParamsOf(UFunction* fn)
{
    std::vector<ParamInfo> v;
    for (FField* f = fn->ChildProperties; f; f = f->Next)
    {
        FProperty* p = static_cast<FProperty*>(f);
        if (!(p->PropertyFlags & CPF_Parm)) continue;
        bool ret = (p->PropertyFlags & CPF_ReturnParm) != 0;
        bool out = (p->PropertyFlags & CPF_OutParm) && !(p->PropertyFlags & CPF_ConstParm);
        v.push_back({f->Name.ToString(), TypeName(p), out && !ret, ret, (p->PropertyFlags & CPF_ConstParm) != 0});
    }
    return v;
}

std::string SigOf(UFunction* fn)
{
    std::string in, ret;
    for (auto& pi : ParamsOf(fn))
    {
        if (pi.ret) { ret = pi.type; continue; }
        if (!in.empty()) in += ", ";
        in += (pi.out ? "out " : "") + pi.name + ": " + pi.type;
    }
    return "(" + in + ")" + (ret.empty() ? "" : " -> " + ret);
}

// kmod.signatures(classOrObject) -> { {owner=, name=, sig=}, ... } for the class chain
int Kmod_Signatures(lua_State* L)
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
                lua_newtable(L);
                lua_pushstring(L, c->GetName().c_str()); lua_setfield(L, -2, "owner");
                lua_pushstring(L, f->GetName().c_str()); lua_setfield(L, -2, "name");
                lua_pushstring(L, SigOf(static_cast<UFunction*>(f)).c_str()); lua_setfield(L, -2, "sig");
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
    {"GetClass", M_GetClass}, {"GetDefaultObject", M_GetDefaultObject}, {"GetPathName", M_GetPathName}, {"GetOuter", M_GetOuter}, {"GetAddress", M_GetAddress}, {"IsA", M_IsA},
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

// kmod.func_params(path) -> { {name=, type=, out=, ret=}, ... } | nil
int Kmod_FuncParams(lua_State* L)
{
    UFunction* fn = static_cast<UFunction*>(FindByPath(luaL_checkstring(L, 1), true));
    if (!fn) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    int i = 1;
    for (auto& pi : ParamsOf(fn))
    {
        lua_newtable(L);
        lua_pushstring(L, pi.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, pi.type.c_str()); lua_setfield(L, -2, "type");
        lua_pushboolean(L, pi.out); lua_setfield(L, -2, "out");
        lua_pushboolean(L, pi.ret); lua_setfield(L, -2, "ret");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// kmod.search(pattern, kind, limit) -> results, total.   kind: "class" | "object" | "function" (default "class").
// Every space-separated word of `pattern` must occur in the object's full path (case-insensitive).
// results = { {path=, class=}, ... } (at most `limit`), total = number of matches.
int Kmod_Search(lua_State* L)
{
    std::string pat = Lower(luaL_checkstring(L, 1));
    std::string kind = luaL_optstring(L, 2, "class");
    int limit = (int)luaL_optinteger(L, 3, 30);
    std::vector<std::string> words;
    { size_t i = 0; while (i < pat.size()) { while (i < pat.size() && pat[i] == ' ') i++; size_t j = i; while (j < pat.size() && pat[j] != ' ') j++; if (j > i) words.push_back(pat.substr(i, j - i)); i = j; } }
    lua_newtable(L);
    int n = 0, total = 0;
    UClass* ucls = UClass::StaticClass();
    UClass* fcls = UFunction::StaticClass();
    UClass* pkg = UPackage::StaticClass();
    ForEachObject([&](UObject* o) {
        bool isClass = o->IsA(ucls), isFunc = o->Class == fcls;
        if (kind == "class" ? !isClass : kind == "function" ? !isFunc : (isClass || isFunc || o->Class == pkg || IsCDO(o))) return false;
        std::string path = PathOf(o);
        std::string lp = Lower(path);
        for (auto& w : words) if (lp.find(w) == std::string::npos) return false;
        total++;
        if (n < limit)
        {
            lua_newtable(L);
            lua_pushstring(L, path.c_str()); lua_setfield(L, -2, "path");
            lua_pushstring(L, o->Class ? o->Class->GetName().c_str() : ""); lua_setfield(L, -2, "class");
            lua_rawseti(L, -2, ++n);
        }
        return false;
    });
    lua_pushinteger(L, total);
    return 2;
}

// ---- type declarations for editors (LuaLS / EmmyLua annotations), generated from live reflection ----
std::string LuaIdent(std::string n)
{
    for (char& c : n) if (!(isalnum((unsigned char)c) || c == '_')) c = '_';
    if (n.empty() || isdigit((unsigned char)n[0])) n = "_" + n;
    static const char* kw[] = {"and","break","do","else","elseif","end","false","for","function","goto","if","in","local","nil","not","or","repeat","return","then","true","until","while"};
    for (const char* k : kw) if (n == k) return n + "_";
    return n;
}

std::string LuaType(FProperty* p, int depth = 0)
{
    if (!p) return "any";
    if (depth > 3) return "any";
    switch (TypeOf(p))
    {
    case T_Bool: return "boolean";
    case T_Int8: case T_Byte: case T_Int16: case T_UInt16: case T_Int: case T_UInt32: case T_Int64: case T_UInt64: case T_Float: case T_Double:
        if (TypeOf(p) == T_Byte) { auto* e = static_cast<FByteProperty*>(p)->Enum; if (e) return LuaIdent(e->GetName()); }
        return "number";
    case T_Name: case T_Str: case T_Text: return "string";
    case T_Object: { auto* c = static_cast<FObjectPropertyBase*>(p)->PropertyClass; return c ? LuaIdent(c->GetName()) : "KObject"; }
    case T_Class: case T_Weak: case T_Soft: case T_Interface: return "KObject";
    case T_Struct: { auto* st = static_cast<FStructProperty*>(p)->Struct; return st ? LuaIdent(st->GetName()) : "table"; }
    case T_Enum: { auto* e = static_cast<FEnumProperty*>(p)->Enum; return e ? LuaIdent(e->GetName()) : "number"; }
    case T_Array: return LuaType(static_cast<FArrayProperty*>(p)->InnerProperty, depth + 1) + "[]";
    case T_Set: return LuaType(static_cast<FSetProperty*>(p)->ElementProperty, depth + 1) + "[]";
    case T_Map: { auto* m = static_cast<FMapProperty*>(p); return "table<" + LuaType(m->KeyProperty, depth + 1) + ", " + LuaType(m->ValueProperty, depth + 1) + ">"; }
    default: return "any";
    }
}

// "/Script/Engine.Actor" -> "Script_Engine";  "/Game/UI/Foo" -> "Game";  "/Engine/..." -> "EngineContent"
std::string BucketOf(UObject* o)
{
    UObject* top = o;
    for (int i = 0; i < 32 && top->Outer; i++) top = top->Outer;
    std::string pkg = InSDKUtils::GetNameByIndex(top->Name.GetDisplayIndex());     // "/Script/Engine"
    std::vector<std::string> seg;
    { size_t i = 0; while (i < pkg.size()) { size_t j = pkg.find('/', i); if (j == std::string::npos) j = pkg.size(); if (j > i) seg.push_back(pkg.substr(i, j - i)); i = j + 1; } }
    if (seg.empty()) return "Other";
    if (seg[0] == "Script" && seg.size() > 1) return "Script_" + LuaIdent(seg[1]);
    if (seg[0] == "Engine") return "EngineContent";
    return LuaIdent(seg[0]);
}

// kmod.dump_types(outDir, {filter,...}) -> files, classes, structs, enums, functions
int Kmod_DumpTypes(lua_State* L)
{
    std::string outDir = luaL_checkstring(L, 1);
    std::vector<std::string> filters;
    if (lua_istable(L, 2))
        for (int i = 1;; i++) { lua_rawgeti(L, 2, i); if (lua_isnil(L, -1)) { lua_pop(L, 1); break; } filters.push_back(Lower(lua_tostring(L, -1))); lua_pop(L, 1); }
    auto wanted = [&](const std::string& bucket) {
        if (filters.empty()) return true;
        std::string b = Lower(bucket);
        for (auto& f : filters) if (b.find(f) != std::string::npos) return true;
        return false;
    };
    std::map<std::string, std::string> files;
    std::set<std::string> used;
    long nClass = 0, nStruct = 0, nEnum = 0, nFunc = 0;
    UClass* ucls = UClass::StaticClass();
    UClass* scls = UScriptStruct::StaticClass();
    UClass* ecls = UEnum::StaticClass();
    ForEachObject([&](UObject* o) {
        bool isClass = o->IsA(ucls), isStruct = !isClass && o->IsA(scls), isEnum = !isClass && !isStruct && o->IsA(ecls);
        if (!isClass && !isStruct && !isEnum) return false;
        std::string bucket = BucketOf(o);
        if (!wanted(bucket)) return false;
        std::string name = LuaIdent(o->GetName());
        if (!used.insert(name).second) return false;                    // same short name in two packages: first one wins
        std::string& out = files[bucket];
        if (isEnum) { out += "---@alias " + name + " integer\n\n"; nEnum++; return false; }
        UStruct* st = static_cast<UStruct*>(o);
        out += "---@class " + name;
        if (st->SuperStruct) { std::string sup = LuaIdent(st->SuperStruct->GetName()); if (sup != name) out += " : " + sup; }
        else if (isClass) out += " : KObject";
        out += "\n";
        std::set<std::string> seen;                                  // a field name may only be declared once per class
        for (FField* f = st->ChildProperties; f; f = f->Next)
        {
            FProperty* p = static_cast<FProperty*>(f);
            std::string fname = LuaIdent(f->Name.ToString());
            if (!seen.insert(fname).second) continue;
            out += "---@field " + fname + " " + LuaType(p) + "\n";
        }
        if (isClass)
        {
            UClass* fcls = UFunction::StaticClass();
            for (UField* f = st->Children; f; f = f->Next)
            {
                if (f->Class != fcls) continue;
                UFunction* fn = static_cast<UFunction*>(f);
                std::string fname = LuaIdent(f->GetName());
                if (!seen.insert(fname).second) continue;
                std::string params, rets;
                std::set<std::string> pseen = {"self"};
                for (auto* ff = fn->ChildProperties; ff; ff = ff->Next)
                {
                    FProperty* p = static_cast<FProperty*>(ff);
                    if (!(p->PropertyFlags & CPF_Parm)) continue;
                    bool ret = (p->PropertyFlags & CPF_ReturnParm) != 0;
                    bool outp = (p->PropertyFlags & CPF_OutParm) && !(p->PropertyFlags & CPF_ConstParm);
                    if (ret) rets = LuaType(p) + (rets.empty() ? "" : ", " + rets);
                    else if (outp) rets += (rets.empty() ? "" : ", ") + LuaType(p);
                    else
                    {
                        std::string pn = LuaIdent(ff->Name.ToString());
                        for (int k = 2; !pseen.insert(pn).second; k++) pn = LuaIdent(ff->Name.ToString()) + "_" + std::to_string(k);
                        params += ", " + pn + ": " + LuaType(p);
                    }
                }
                out += "---@field " + fname + " fun(self: " + name + params + ")" + (rets.empty() ? "" : ": " + rets) + "\n";
                nFunc++;
            }
        }
        out += "\n";
        (isClass ? nClass : nStruct)++;
        return false;
    });
    mkdir(outDir.c_str(), 0777);
    long nFiles = 0;
    for (auto& kv : files)
    {
        std::string path = outDir + "/" + kv.first + ".lua";
        if (FILE* f = fopen(path.c_str(), "wb"))
        {
            std::string head = "---@meta\n-- Generated by kmod `typegen` from the running game's reflection data (" + kv.first + ").\n-- Do not edit; run `typegen` again after a game update.\n\n";
            fwrite(head.data(), 1, head.size(), f);
            fwrite(kv.second.data(), 1, kv.second.size(), f);
            fclose(f);
            nFiles++;
        }
    }
    lua_pushinteger(L, nFiles); lua_pushinteger(L, nClass); lua_pushinteger(L, nStruct); lua_pushinteger(L, nEnum); lua_pushinteger(L, nFunc);
    return 5;
}

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

struct NewObjWatch { std::string cls; UClass* c; int fnRef; std::string owner; bool fast; };
bool g_anyFastWatch = false;                 // some watch asked for the fast scan (every ~16 ms instead of every 50 ms)
double g_nextFastScan = 0;

struct FastTimer { double due; int fnRef; double period; std::string owner; };
std::vector<FastTimer> g_fastTimers;
double FastNow() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
std::vector<NewObjWatch> g_newObjWatches;
int32_t g_lastObjCount = 0;
struct SlotSeen { UObject* p = nullptr; int32_t serial = 0; };
std::vector<SlotSeen> g_slotSeen;
inline int32_t SlotSerial(FUObjectItem* it) { return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(it) + 0x10); }   // FUObjectItem::SerialNumber

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

int G_ExecuteAsync(lua_State* L)   // UE4SS name; kmod runs it on the game thread at the next tick (use Worker for real parallel work)
{
    return G_ExecuteInGameThread(L);
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
    bool fast = lua_toboolean(L, 3) != 0;                        // NotifyOnNewObject(class, fn, fast)
    g_newObjWatches.push_back({cn, nullptr, luaL_ref(L, LUA_REGISTRYINDEX), Owner(L), fast});
    if (fast) g_anyFastWatch = true;
    return 0;
}

// FastLoop(ms, fn): like LoopAsync, but fn runs every few milliseconds on the game thread (not only 20 times a second):
// use it for tiny checks that must happen before the next frame is drawn. ms is clamped to >= 2. Return true to stop.
int G_FastLoop(lua_State* L)
{
    double ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 2) ms = 2;
    lua_pushvalue(L, 2);
    g_fastTimers.push_back({FastNow() + ms / 1000.0, luaL_ref(L, LUA_REGISTRYINDEX), ms / 1000.0, Owner(L)});
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
// Directories: <KModRoot>/mods, <KModRoot>/dev (see KModRoot in kmod_common.h)
// ---------------------------------------------------------------------------------------------
std::string ModsDir() { return KModRoot() + "/mods"; }
std::string DevDir() { return KModRoot() + "/dev"; }

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
// Dev console: runs <root>/dev/console.lua whenever it changes, writes console.out
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

int Ccb_Rmtree(lua_State* L)   // only inside <root>/
{
    std::string path = luaL_checkstring(L, 1);
    std::string root = KModRoot() + "/";
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
        {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject}, {"FastLoop", G_FastLoop}, {"ExecuteAsync", G_ExecuteAsync}};
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
        {"ExecuteInGameThread", G_ExecuteInGameThread}, {"LoopAsync", G_LoopAsync}, {"NotifyOnNewObject", G_NotifyOnNewObject}, {"FastLoop", G_FastLoop},
        {"ExecuteAsync", G_ExecuteAsync},
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

    {   // shared JSON codec: kmod.json (main state) and the global `json` inside workers
        static const WorkerHooks wh = {
            [](const std::string& s) { EmitLine(s); },
            [](lua_State* S, int na, int nr, const char* what, const std::string& owner) { return PCall(S, na, nr, what, owner); },
            [](lua_State* S, const std::string& owner, const std::string& msg) { ReportModError(S, owner, msg); }};
        Worker_Init(L, wh, kJsonLua);
        if (luaL_loadbuffer(L, kJsonLua, strlen(kJsonLua), "=json") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK)
        {
            lua_getglobal(L, "kmod");
            lua_pushvalue(L, -2); lua_setfield(L, -2, "json");
            lua_pop(L, 2);
        }
        else { EmitLine(std::string("[error] json: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?")); lua_pop(L, 1); }
    }
    lua_getglobal(L, "kmod");
    lua_pushcfunction(L, Worker_Create); lua_setfield(L, -2, "worker");
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_pushboolean(S, KMod_MountPak(luaL_checkstring(S, 1))); return 1; });
    lua_setfield(L, -2, "mount_pak");
    lua_pushcfunction(L, Ccb_ListDir); lua_setfield(L, -2, "listdir");
    lua_pushcfunction(L, Ccb_Exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, Ccb_Mkdir); lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, Ccb_ReloadMods); lua_setfield(L, -2, "reload_mods");
    lua_pushcfunction(L, Ccb_Unzip); lua_setfield(L, -2, "unzip");
    lua_pushcfunction(L, Ccb_Rmtree); lua_setfield(L, -2, "rmtree");
    lua_pushcfunction(L, Ccb_ModApi); lua_setfield(L, -2, "mod_api");
    lua_pushcfunction(L, Kmod_Log); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, Kmod_Signatures); lua_setfield(L, -2, "signatures");
    lua_pushcfunction(L, Kmod_FuncParams); lua_setfield(L, -2, "func_params");
    lua_pushcfunction(L, Kmod_Search); lua_setfield(L, -2, "search");
    lua_pushcfunction(L, Kmod_DumpTypes); lua_setfield(L, -2, "dump_types");
#ifdef KMOD_DEV
    lua_pushcfunction(L, Kmod_DebugCrash); lua_setfield(L, -2, "debug_crash");
#endif
    lua_pushstring(L, KMOD_VERSION); lua_setfield(L, -2, "version");
    lua_pushstring(L, KMOD_FLAVOR); lua_setfield(L, -2, "flavor");
    lua_pushinteger(L, KMOD_API_LEVEL); lua_setfield(L, -2, "api");
    lua_newtable(L);
    lua_pushstring(L, ExternalFilesDir().c_str()); lua_setfield(L, -2, "files");        // legacy app files dir
    lua_pushstring(L, KModRoot().c_str()); lua_setfield(L, -2, "root");
    lua_pushstring(L, ModsDir().c_str()); lua_setfield(L, -2, "mods");
    lua_pushstring(L, DevDir().c_str()); lua_setfield(L, -2, "dev");
    lua_pushstring(L, KLogDir().c_str()); lua_setfield(L, -2, "logs");
    lua_setfield(L, -2, "paths");
    lua_pop(L, 1);
    mkdir(KModRoot().c_str(), 0777);
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

    {   // everything that exists now counts as "seen": NotifyOnNewObject only reports objects created from here on
        TUObjectArray* a = InSDKUtils::GetGObjects();
        int32_t cnt = a->Num();
        g_lastObjCount = cnt;
        g_slotSeen.assign(cnt, SlotSeen{});
        for (int32_t i = 0; i < cnt; i++)
        {
            FUObjectItem* it = ObjRef::ItemAt(i);
            if (it) { g_slotSeen[i].p = it->Object; g_slotSeen[i].serial = SlotSerial(it); }
        }
    }
    EmitLine(std::string("kmod ") + KMOD_VERSION + " (" + KMOD_FLAVOR + "): Lua host ready (" + LUA_VERSION + ")");
    KLogInstallCrashHandlers();
    CallLoadMods();
}

void LuaHost_ReloadMods()
{
    // drop everything registered by user mods (owner != ""); built-in scripts (owner "") stay
    if (!L_) return;
    Worker_CloseUser(L_);
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
    for (size_t i = 0; i < g_fastTimers.size();)
        if (!g_fastTimers[i].owner.empty()) { luaL_unref(L_, LUA_REGISTRYINDEX, g_fastTimers[i].fnRef); g_fastTimers.erase(g_fastTimers.begin() + i); } else i++;
    g_anyFastWatch = false;
    for (auto& w : g_newObjWatches) if (w.fast) g_anyFastWatch = true;
    EmitLine("[mod] reloading mods");
    CallLoadMods();
}

bool LuaHost_HasHook(UFunction* fn) { return !g_hooks.empty() && g_hooks.count(fn) != 0; }
void LuaHost_PreHook(UObject* self, UFunction* fn, void* parms) { RunHooks(self, fn, parms, false); }
void LuaHost_PostHook(UObject* self, UFunction* fn, void* parms) { RunHooks(self, fn, parms, true); }

// New-object watches: compare every GObjects slot with what we saw last time. The engine REUSES freed slots, so a
// new object is not only "a slot past the old end": its (pointer, serial number) pair has to differ from before.
static void ScanNewObjects()
{
    if (g_newObjWatches.empty() || !L_) return;
    TUObjectArray* a = InSDKUtils::GetGObjects();
    int32_t cnt = a->Num();
    if ((int32_t)g_slotSeen.size() < cnt) g_slotSeen.resize(cnt);
    for (int32_t i = 0; i < cnt; i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        int32_t sn = it ? SlotSerial(it) : 0;
        SlotSeen& seen = g_slotSeen[i];
        if (o == seen.p && sn == seen.serial) continue;
        if (o && (!ObjRef::ItemUsable(it) || !o->Class)) continue;      // not fully set up yet: look again next time
        seen.p = o; seen.serial = sn;
        if (!o) continue;
        for (size_t wi = 0; wi < g_newObjWatches.size(); wi++)          // by index: a callback may register more watches
        {
            NewObjWatch& w = g_newObjWatches[wi];
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

// Fast lane, called from the ProcessEvent hook every few milliseconds (see core.cpp): FastLoop timers and, when a watch
// asked for it, the new-object scan at ~60 Hz.
bool LuaHost_HasFast() { return !g_fastTimers.empty() || g_anyFastWatch; }
void LuaHost_FastTick(double now)
{
    if (!L_) return;
    if (!g_fastTimers.empty())
    {
        std::vector<FastTimer> due, keep;
        for (auto& t : g_fastTimers) (t.due <= now ? due : keep).push_back(t);
        g_fastTimers = keep;
        for (auto& t : due)
        {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, t.fnRef);
            bool stop = true;
            if (PCall(L_, 0, 1, "fast loop", t.owner)) { stop = lua_toboolean(L_, -1) != 0; lua_pop(L_, 1); }
            if (!stop) { FastTimer n = t; n.due = now + t.period; g_fastTimers.push_back(n); }
            else luaL_unref(L_, LUA_REGISTRYINDEX, t.fnRef);
        }
    }
    if (g_anyFastWatch && now >= g_nextFastScan) { g_nextFastScan = now + 0.016; ScanNewObjects(); }
}

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

    Worker_Pump(L_);

    if (!g_anyFastWatch) ScanNewObjects();        // with a fast watch the scan runs from LuaHost_FastTick instead

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
