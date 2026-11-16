// A draggable floating button drawn with Android views, written entirely in the mod: a small Java class (java/kmodex/FloatButton.java,
// embedded as a dex) is loaded through JNI. The loader provides only the JNI handles (jni_env, app_class_loader, activity).
//   tap        -> toggles ON/OFF (and logs)
//   long press -> logs
// Android calls the click handler on the UI thread; the mod forwards it to the game thread with an atomic and handles it in on_tick.
#include "kmod.h"

#include <jni.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "float_button_dex.h"

static std::atomic<int> g_click{0};               // 0 none, 1 tap, 2 long press (set on the UI thread, read on the game thread)
static jclass g_cls = nullptr;
static jmethodID g_create = nullptr, g_setText = nullptr, g_destroy = nullptr;
static bool g_created = false, g_on = false;

static void JNICALL NativeClick(JNIEnv*, jclass, jint, jboolean longPress) { g_click = longPress ? 2 : 1; }

static std::string ProcessName()                  // the package name
{
    char buf[256] = {};
    if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    return buf;
}

static bool LoadBridge()
{
    const KModApi* api = kmod::g_api;
    JNIEnv* env = static_cast<JNIEnv*>(api->jni_env());
    jobject appLoader = static_cast<jobject>(api->app_class_loader());
    if (!env || !appLoader) return false;

    // the dex must live in private storage and be read-only (shared storage is not executable, Android 14 wants read-only dex)
    std::string dir = "/data/data/" + ProcessName() + "/files/kmod/native/" + api->mod_id;
    std::string path = dir + "/button.dex";
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || (unsigned)st.st_size != kButtonDexSize)
    {
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) { KLOGE("cannot write %s", tmp.c_str()); return false; }
        fwrite(kButtonDex, 1, kButtonDexSize, f); fclose(f);
        chmod(tmp.c_str(), 0400);
        unlink(path.c_str()); rename(tmp.c_str(), path.c_str());
    }

    jclass dcl = env->FindClass("dalvik/system/DexClassLoader");
    jmethodID ctor = env->GetMethodID(dcl, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    jobject loader = env->NewObject(dcl, ctor, env->NewStringUTF(path.c_str()), env->NewStringUTF(dir.c_str()), (jstring) nullptr, appLoader);
    if (!loader) { env->ExceptionClear(); KLOGE("DexClassLoader failed"); return false; }
    jmethodID loadClass = env->GetMethodID(env->FindClass("java/lang/ClassLoader"), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jclass cls = (jclass)env->CallObjectMethod(loader, loadClass, env->NewStringUTF("kmodex.FloatButton"));
    if (!cls) { env->ExceptionClear(); KLOGE("cannot load kmodex.FloatButton"); return false; }
    g_cls = (jclass)env->NewGlobalRef(cls);
    JNINativeMethod nm = {(char*)"nativeClick", (char*)"(IZ)V", (void*)&NativeClick};
    if (env->RegisterNatives(g_cls, &nm, 1) != 0) { env->ExceptionClear(); KLOGE("RegisterNatives failed"); return false; }
    g_create = env->GetStaticMethodID(g_cls, "create", "(Landroid/app/Activity;ILjava/lang/String;II)V");
    g_setText = env->GetStaticMethodID(g_cls, "setText", "(ILjava/lang/String;)V");
    g_destroy = env->GetStaticMethodID(g_cls, "destroy", "(I)V");
    return g_create && g_setText && g_destroy;
}

static void OnTick(double, void*)                 // game thread, about 20 times a second
{
    const KModApi* api = kmod::g_api;
    JNIEnv* env = static_cast<JNIEnv*>(api->jni_env());
    if (!g_created)
    {
        jobject activity = static_cast<jobject>(api->activity());       // null until the game's window exists
        if (!activity) return;
        jstring text = env->NewStringUTF("kmod OFF");
        env->CallStaticVoidMethod(g_cls, g_create, activity, 1, text, 40, 300);
        env->DeleteLocalRef(text);                       // the game thread never returns to Java, so local references must be freed by hand
        g_created = true;
        KLOGI("floating button created");
        return;
    }
    int c = g_click.exchange(0);
    if (c == 1)
    {
        g_on = !g_on;
        jstring text = env->NewStringUTF(g_on ? "kmod ON" : "kmod OFF");
        env->CallStaticVoidMethod(g_cls, g_setText, 1, text);
        env->DeleteLocalRef(text);
        KLOGI("button tapped: now %s", g_on ? "ON" : "OFF");
    }
    else if (c == 2) KLOGI("button long-pressed");
}

static int OnStart()
{
    if (!LoadBridge()) return 1;
    kmod::g_api->on_tick(OnTick, nullptr);
    return 0;
}

static void OnStop()
{
    JNIEnv* env = static_cast<JNIEnv*>(kmod::g_api->jni_env());
    if (env && g_created) env->CallStaticVoidMethod(g_cls, g_destroy, 1);
}

KMOD_MOD(OnStart, OnStop)
