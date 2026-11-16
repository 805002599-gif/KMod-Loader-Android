#include "kmod_log.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "kmod_common.h"
#include "kmod_version.h"

namespace
{
std::mutex g_mu;
FILE* g_file = nullptr;
int g_crashFd = -1;
std::string g_dir;
bool g_inited = false;
constexpr int kKeepFiles = 5;

KLogLevel MinLevel() { return kDevBuild ? KLOG_DEBUG : KLOG_INFO; }
char LevelChar(KLogLevel l) { return l == KLOG_DEBUG ? 'D' : l == KLOG_INFO ? 'I' : l == KLOG_WARN ? 'W' : 'E'; }
int AndroidPrio(KLogLevel l) { return l == KLOG_DEBUG ? ANDROID_LOG_DEBUG : l == KLOG_INFO ? ANDROID_LOG_INFO : l == KLOG_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_ERROR; }

void MkdirAllLocal(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
}

std::string Prop(const char* name)
{
    char buf[PROP_VALUE_MAX] = {};
    __system_property_get(name, buf);
    return buf;
}

void WriteLine(KLogLevel level, const char* tag, const std::string& msg)
{
    __android_log_print(AndroidPrio(level), "KMod", "[%s] %s", tag, msg.c_str());
    if (level < MinLevel() || !g_file) return;
    timeval tv; gettimeofday(&tv, nullptr);
    tm t; localtime_r(&tv.tv_sec, &t);
    char ts[40];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
    fprintf(g_file, "%s %c/%s: %s\n", ts, LevelChar(level), tag, msg.c_str());
    fflush(g_file);
    if (level == KLOG_ERROR) fsync(fileno(g_file));
}
} // namespace

const std::string& KLogDir() { return g_dir; }

void KLogMsg(KLogLevel level, const char* tag, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_mu);
    WriteLine(level, tag, msg);
}

void KLog(KLogLevel level, const char* tag, const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    KLogMsg(level, tag, buf);
}

void KLogInit()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_inited) return;
    g_inited = true;
    {   // one-time migration of the pre-rename data directory (CCB -> kmod)
        struct stat st;
        std::string oldDir = ExternalFilesDir() + "/CCB", newDir = ExternalFilesDir() + "/kmod";
        if (stat(oldDir.c_str(), &st) == 0 && stat(newDir.c_str(), &st) != 0) rename(oldDir.c_str(), newDir.c_str());
    }
    g_dir = ExternalFilesDir() + "/kmod/logs";
    MkdirAllLocal(g_dir);

    // rotate: kmod.log -> kmod.1.log -> ... -> kmod.(N-1).log, oldest dropped
    auto name = [](int i) { return g_dir + (i == 0 ? "/kmod.log" : "/kmod." + std::to_string(i) + ".log"); };
    unlink(name(kKeepFiles - 1).c_str());
    for (int i = kKeepFiles - 2; i >= 0; i--) rename(name(i).c_str(), name(i + 1).c_str());

    g_file = fopen(name(0).c_str(), "w");
    if (g_file) g_crashFd = dup(fileno(g_file));

    WriteLine(KLOG_INFO, "kmod", std::string("=== kmod loader ") + KMOD_VERSION + " (" + KMOD_FLAVOR + ") started ===");
    WriteLine(KLOG_INFO, "kmod", "package " + std::string(kPackageName) + ", pid " + std::to_string(getpid()));
    WriteLine(KLOG_INFO, "kmod", "device " + Prop("ro.product.manufacturer") + " " + Prop("ro.product.model") + ", Android " +
                                      Prop("ro.build.version.release") + " (API " + Prop("ro.build.version.sdk") + "), abi " + Prop("ro.product.cpu.abi"));
    WriteLine(KLOG_INFO, "kmod", "log dir " + g_dir);
}

// ------------------------------------------------------------------------------------------------
// Crash handler. Only async-signal-safe-ish primitives: write() to a pre-opened fd, hand-rolled number formatting.
// ------------------------------------------------------------------------------------------------
namespace
{
struct sigaction g_old[32];
bool g_haveOld[32] = {};

void SafeAppend(char* buf, size_t cap, size_t& n, const char* s) { while (*s && n + 1 < cap) buf[n++] = *s++; }
void SafeHex(char* buf, size_t cap, size_t& n, uintptr_t v)
{
    char tmp[20]; int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v && k < 18) { int d = v & 0xF; tmp[k++] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; }
    SafeAppend(buf, cap, n, "0x");
    while (k) { if (n + 1 < cap) buf[n++] = tmp[--k]; else break; }
}
void SafeDec(char* buf, size_t cap, size_t& n, long v)
{
    char tmp[24]; int k = 0;
    if (v < 0) { SafeAppend(buf, cap, n, "-"); v = -v; }
    if (v == 0) tmp[k++] = '0';
    while (v && k < 22) { tmp[k++] = '0' + (v % 10); v /= 10; }
    while (k) { if (n + 1 < cap) buf[n++] = tmp[--k]; else break; }
}

struct Trace { uintptr_t pcs[48]; int n; };
_Unwind_Reason_Code UnwindCb(struct _Unwind_Context* ctx, void* arg)
{
    Trace* t = static_cast<Trace*>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && t->n < 48) t->pcs[t->n++] = pc;
    return t->n >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

void CrashHandler(int sig, siginfo_t* info, void* ctx)
{
    if (g_crashFd >= 0)
    {
        char line[400]; size_t n = 0;
        char tname[20] = {}; prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(tname), 0, 0, 0);
        SafeAppend(line, sizeof(line), n, "\n!!! FATAL signal "); SafeDec(line, sizeof(line), n, sig);
        SafeAppend(line, sizeof(line), n, " code "); SafeDec(line, sizeof(line), n, info ? info->si_code : 0);
        SafeAppend(line, sizeof(line), n, " fault addr "); SafeHex(line, sizeof(line), n, info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
        SafeAppend(line, sizeof(line), n, " tid "); SafeDec(line, sizeof(line), n, gettid());
        SafeAppend(line, sizeof(line), n, " thread '"); SafeAppend(line, sizeof(line), n, tname); SafeAppend(line, sizeof(line), n, "' (kmod ");
        SafeAppend(line, sizeof(line), n, KMOD_VERSION); SafeAppend(line, sizeof(line), n, " "); SafeAppend(line, sizeof(line), n, KMOD_FLAVOR);
        SafeAppend(line, sizeof(line), n, ")\n");
        write(g_crashFd, line, n);

        // the exact faulting instruction (pc) and the caller (lr) from the signal context: the first frames of the unwound
        // trace are the handler itself, so these two lines are the quickest way to find the crash site
#if defined(__aarch64__)
        if (ctx)
        {
            const ucontext_t* uc = static_cast<const ucontext_t*>(ctx);
            const uintptr_t at[2] = { static_cast<uintptr_t>(uc->uc_mcontext.pc), static_cast<uintptr_t>(uc->uc_mcontext.regs[30]) };
            const char* what[2] = { "  fault pc ", "  caller lr " };
            for (int i = 0; i < 2; i++)
            {
                n = 0;
                SafeAppend(line, sizeof(line), n, what[i]);
                Dl_info di;
                if (dladdr(reinterpret_cast<void*>(at[i]), &di) && di.dli_fbase)
                {
                    SafeHex(line, sizeof(line), n, at[i] - reinterpret_cast<uintptr_t>(di.dli_fbase));
                    SafeAppend(line, sizeof(line), n, " "); SafeAppend(line, sizeof(line), n, di.dli_fname ? di.dli_fname : "?");
                    if (di.dli_sname) { SafeAppend(line, sizeof(line), n, " ("); SafeAppend(line, sizeof(line), n, di.dli_sname); SafeAppend(line, sizeof(line), n, ")"); }
                }
                else SafeHex(line, sizeof(line), n, at[i]);
                SafeAppend(line, sizeof(line), n, "\n");
                write(g_crashFd, line, n);
            }
        }
#endif
        Trace t; t.n = 0;
        _Unwind_Backtrace(UnwindCb, &t);
        for (int i = 0; i < t.n; i++)
        {
            n = 0;
            SafeAppend(line, sizeof(line), n, "  #"); SafeDec(line, sizeof(line), n, i);
            Dl_info di;
            if (dladdr(reinterpret_cast<void*>(t.pcs[i]), &di) && di.dli_fbase)
            {
                SafeAppend(line, sizeof(line), n, " pc "); SafeHex(line, sizeof(line), n, t.pcs[i] - reinterpret_cast<uintptr_t>(di.dli_fbase));
                SafeAppend(line, sizeof(line), n, " "); SafeAppend(line, sizeof(line), n, di.dli_fname ? di.dli_fname : "?");
                if (di.dli_sname) { SafeAppend(line, sizeof(line), n, " ("); SafeAppend(line, sizeof(line), n, di.dli_sname); SafeAppend(line, sizeof(line), n, ")"); }
            }
            else { SafeAppend(line, sizeof(line), n, " pc "); SafeHex(line, sizeof(line), n, t.pcs[i]); }
            SafeAppend(line, sizeof(line), n, "\n");
            write(g_crashFd, line, n);
        }
        fsync(g_crashFd);
    }

    // hand over to whoever was installed before us (the engine / Android's debuggerd), or die with the default action
    if (sig > 0 && sig < 32 && g_haveOld[sig])
    {
        const struct sigaction& o = g_old[sig];
        if (o.sa_flags & SA_SIGINFO) { o.sa_sigaction(sig, info, ctx); return; }
        if (o.sa_handler != SIG_DFL && o.sa_handler != SIG_IGN) { o.sa_handler(sig); return; }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
} // namespace

void KLogInstallCrashHandlers()
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    int installed = 0;
    for (int sig : sigs)
    {
        struct sigaction cur;
        if (sigaction(sig, nullptr, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == &CrashHandler) continue;   // still ours
        struct sigaction sa; memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &CrashHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        struct sigaction old;
        if (sigaction(sig, &sa, &old) == 0)
        {
            if (!(old.sa_flags & SA_SIGINFO && old.sa_sigaction == &CrashHandler)) { g_old[sig] = old; g_haveOld[sig] = true; }
            installed++;
        }
    }
    if (installed) KLog(KLOG_DEBUG, "kmod", "crash handlers installed for %d signal(s)", installed);
}
