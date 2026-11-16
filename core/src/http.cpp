#include "http.h"

#include <dlfcn.h>
#include <jni.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
struct Options
{
    std::string method = "GET", url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 15000;
    size_t maxBytes = 8u << 20;
};

struct Response
{
    int id = 0, status = 0;
    std::string body, error;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct Pending { std::string owner; int cbRef; };

WorkerHooks g_hooks{};
std::mutex g_mu;
std::deque<Response> g_done;
std::map<int, Pending> g_pending;        // game thread only
std::atomic<int> g_active{0};
int g_nextId = 1;
const int kMaxActive = 6;

JavaVM* GetVM()
{
    static JavaVM* vm = nullptr;
    if (vm) return vm;
    using Fn = jint (*)(JavaVM**, jsize, jsize*);
    // 1) the loader (libkmod.so) keeps the VM it got in JNI_OnLoad and exports it
    if (void* h = dlopen("libkmod.so", RTLD_NOW | RTLD_NOLOAD))
    {
        using Get = JavaVM* (*)();
        if (Get g = reinterpret_cast<Get>(dlsym(h, "KMod_GetJavaVM"))) vm = g();
    }
    // 2) the public JNI entry point (libnativehelper.so, Android 12+), then whatever the default namespace offers
    for (const char* lib : {"libnativehelper.so", "libart.so", "libandroid_runtime.so"})
    {
        if (vm) break;
        if (void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD))
            if (Fn f = reinterpret_cast<Fn>(dlsym(h, "JNI_GetCreatedJavaVMs"))) { jsize n = 0; f(&vm, 1, &n); }
    }
    if (!vm)
    {
        Fn f = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
        jsize n = 0;
        if (f) f(&vm, 1, &n);
    }
    return vm;
}

// Describes and clears a pending Java exception.
std::string TakeException(JNIEnv* env)
{
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    if (!ex) return "unknown error";
    std::string out = "java exception";
    jclass cObj = env->FindClass("java/lang/Object");
    jmethodID toString = env->GetMethodID(cObj, "toString", "()Ljava/lang/String;");
    jstring s = static_cast<jstring>(env->CallObjectMethod(ex, toString));
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (s) { const char* c = env->GetStringUTFChars(s, nullptr); if (c) { out = c; env->ReleaseStringUTFChars(s, c); } env->DeleteLocalRef(s); }
    env->DeleteLocalRef(ex);
    env->DeleteLocalRef(cObj);
    return out;
}

#define JCHECK() do { if (env->ExceptionCheck()) { r.error = TakeException(env); goto done; } } while (0)

Response Perform(JNIEnv* env, const Options& o)
{
    Response r;
    jclass cURL = nullptr, cConn = nullptr, cIn = nullptr, cOut = nullptr;
    jobject url = nullptr, conn = nullptr, in = nullptr, os = nullptr;
    jbyteArray buf = nullptr;
    jmethodID mGetCode, mDisconnect;
    int status = 0;

    cURL = env->FindClass("java/net/URL");
    cConn = env->FindClass("java/net/HttpURLConnection");
    cIn = env->FindClass("java/io/InputStream");
    cOut = env->FindClass("java/io/OutputStream");
    JCHECK();
    {
        jstring ju = env->NewStringUTF(o.url.c_str());
        url = env->NewObject(cURL, env->GetMethodID(cURL, "<init>", "(Ljava/lang/String;)V"), ju);
        env->DeleteLocalRef(ju);
        JCHECK();
        jobject uc = env->CallObjectMethod(url, env->GetMethodID(cURL, "openConnection", "()Ljava/net/URLConnection;"));
        JCHECK();
        if (!uc || !env->IsInstanceOf(uc, cConn)) { r.error = "not an http(s) connection"; if (uc) env->DeleteLocalRef(uc); goto done; }
        conn = uc;
    }
    {
        jstring jm = env->NewStringUTF(o.method.c_str());
        env->CallVoidMethod(conn, env->GetMethodID(cConn, "setRequestMethod", "(Ljava/lang/String;)V"), jm);
        env->DeleteLocalRef(jm);
        JCHECK();
        env->CallVoidMethod(conn, env->GetMethodID(cConn, "setConnectTimeout", "(I)V"), (jint)o.timeoutMs);
        env->CallVoidMethod(conn, env->GetMethodID(cConn, "setReadTimeout", "(I)V"), (jint)o.timeoutMs);
        jmethodID setProp = env->GetMethodID(cConn, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
        for (auto& h : o.headers)
        {
            jstring k = env->NewStringUTF(h.first.c_str()), v = env->NewStringUTF(h.second.c_str());
            env->CallVoidMethod(conn, setProp, k, v);
            env->DeleteLocalRef(k); env->DeleteLocalRef(v);
            JCHECK();
        }
        if (!o.body.empty())
        {
            env->CallVoidMethod(conn, env->GetMethodID(cConn, "setDoOutput", "(Z)V"), JNI_TRUE);
            os = env->CallObjectMethod(conn, env->GetMethodID(cConn, "getOutputStream", "()Ljava/io/OutputStream;"));
            JCHECK();
            jbyteArray arr = env->NewByteArray((jsize)o.body.size());
            env->SetByteArrayRegion(arr, 0, (jsize)o.body.size(), reinterpret_cast<const jbyte*>(o.body.data()));
            env->CallVoidMethod(os, env->GetMethodID(cOut, "write", "([B)V"), arr);
            env->DeleteLocalRef(arr);
            JCHECK();
            env->CallVoidMethod(os, env->GetMethodID(cOut, "close", "()V"));
            JCHECK();
        }
    }
    mGetCode = env->GetMethodID(cConn, "getResponseCode", "()I");
    status = env->CallIntMethod(conn, mGetCode);
    JCHECK();
    r.status = status;
    {
        // headers: index 0 is the status line
        jmethodID mKey = env->GetMethodID(cConn, "getHeaderFieldKey", "(I)Ljava/lang/String;");
        jmethodID mVal = env->GetMethodID(cConn, "getHeaderField", "(I)Ljava/lang/String;");
        for (int i = 1; i < 80; i++)
        {
            jstring k = static_cast<jstring>(env->CallObjectMethod(conn, mKey, (jint)i));
            jstring v = static_cast<jstring>(env->CallObjectMethod(conn, mVal, (jint)i));
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            if (!k && !v) break;
            std::string ks, vs;
            if (k) { const char* c = env->GetStringUTFChars(k, nullptr); if (c) { ks = c; env->ReleaseStringUTFChars(k, c); } env->DeleteLocalRef(k); }
            if (v) { const char* c = env->GetStringUTFChars(v, nullptr); if (c) { vs = c; env->ReleaseStringUTFChars(v, c); } env->DeleteLocalRef(v); }
            if (!ks.empty()) r.headers.emplace_back(ks, vs);
        }
    }
    in = env->CallObjectMethod(conn, env->GetMethodID(cConn, status >= 400 ? "getErrorStream" : "getInputStream", "()Ljava/io/InputStream;"));
    JCHECK();
    if (in)
    {
        jmethodID mRead = env->GetMethodID(cIn, "read", "([B)I");
        buf = env->NewByteArray(16384);
        for (;;)
        {
            jint n = env->CallIntMethod(in, mRead, buf);
            JCHECK();
            if (n < 0) break;
            size_t old = r.body.size();
            r.body.resize(old + n);
            env->GetByteArrayRegion(buf, 0, n, reinterpret_cast<jbyte*>(&r.body[old]));
            if (r.body.size() > o.maxBytes) { r.body.resize(o.maxBytes); r.error = "response larger than the limit (truncated)"; break; }
        }
    }
done:
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (conn)
    {
        mDisconnect = env->GetMethodID(cConn, "disconnect", "()V");
        env->CallVoidMethod(conn, mDisconnect);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    for (jobject o2 : {url, conn, in, os}) if (o2) env->DeleteLocalRef(o2);
    if (buf) env->DeleteLocalRef(buf);
    for (jclass c : {cURL, cConn, cIn, cOut}) if (c) env->DeleteLocalRef(c);
    return r;
}

void Run(int id, Options o)
{
    Response r;
    JavaVM* vm = GetVM();
    JNIEnv* env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) r.error = "no Java VM";
    else
    {
        r = Perform(env, o);
        vm->DetachCurrentThread();
    }
    r.id = id;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_done.push_back(std::move(r));
    }
    g_active--;
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
}

void Http_Init(const WorkerHooks& hooks) { g_hooks = hooks; }

// kmod.http_request(owner, { method=, url=, headers={}, body=, timeout=ms, max_bytes= }, callback(response)) -> id
//   response = { status = 200, body = "...", headers = { name = value }, error = nil | "message" }
int Http_Request(lua_State* L)
{
    std::string owner = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    Options o;
    lua_getfield(L, 2, "url");    if (lua_isstring(L, -1)) o.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "method"); if (lua_isstring(L, -1)) o.method = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "body");   if (lua_isstring(L, -1)) { size_t n; const char* b = lua_tolstring(L, -1, &n); o.body.assign(b, n); }
    lua_pop(L, 1);
    lua_getfield(L, 2, "timeout"); if (lua_isnumber(L, -1)) { double t = lua_tonumber(L, -1); o.timeoutMs = (int)(t < 1000 ? 1000 : (t > 60000 ? 60000 : t)); }
    lua_pop(L, 1);
    lua_getfield(L, 2, "max_bytes"); if (lua_isnumber(L, -1)) { double m = lua_tonumber(L, -1); if (m > 0 && m < (32 << 20)) o.maxBytes = (size_t)m; }
    lua_pop(L, 1);
    lua_getfield(L, 2, "headers");
    if (lua_istable(L, -1))
    {
        lua_pushnil(L);
        while (lua_next(L, -2))
        {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) o.headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    for (auto& c : o.method) c = (char)toupper((unsigned char)c);
    if (o.method != "GET" && o.method != "POST" && o.method != "PUT" && o.method != "DELETE" && o.method != "HEAD" && o.method != "PATCH")
        return luaL_error(L, "Http: unsupported method '%s'", o.method.c_str());
    if (!StartsWith(o.url, "https://") && !StartsWith(o.url, "http://"))
        return luaL_error(L, "Http: only http:// and https:// URLs are allowed");
    if (g_active.load() >= kMaxActive) return luaL_error(L, "Http: too many requests at once (%d at most)", kMaxActive);

    int id = g_nextId++;
    lua_pushvalue(L, 3);
    g_pending[id] = Pending{owner, luaL_ref(L, LUA_REGISTRYINDEX)};
    g_active++;
    std::thread(Run, id, std::move(o)).detach();
    lua_pushinteger(L, id);
    return 1;
}

void Http_Pump(lua_State* L)
{
    if (g_pending.empty()) return;
    std::deque<Response> batch;
    {
        std::lock_guard<std::mutex> g(g_mu);
        batch.swap(g_done);
    }
    for (auto& r : batch)
    {
        auto it = g_pending.find(r.id);
        if (it == g_pending.end()) continue;                    // cancelled (mod reloaded)
        Pending p = it->second;
        g_pending.erase(it);
        lua_rawgeti(L, LUA_REGISTRYINDEX, p.cbRef);
        luaL_unref(L, LUA_REGISTRYINDEX, p.cbRef);
        lua_newtable(L);
        lua_pushinteger(L, r.status); lua_setfield(L, -2, "status");
        lua_pushlstring(L, r.body.data(), r.body.size()); lua_setfield(L, -2, "body");
        lua_newtable(L);
        for (auto& h : r.headers) { lua_pushstring(L, h.second.c_str()); lua_setfield(L, -2, h.first.c_str()); }
        lua_setfield(L, -2, "headers");
        if (!r.error.empty()) { lua_pushstring(L, r.error.c_str()); lua_setfield(L, -2, "error"); }
        g_hooks.pcall(L, 1, 0, "http callback", p.owner);
    }
}

void Http_CancelUser(lua_State* L)
{
    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        if (!it->second.owner.empty()) { luaL_unref(L, LUA_REGISTRYINDEX, it->second.cbRef); it = g_pending.erase(it); }
        else ++it;
    }
}
