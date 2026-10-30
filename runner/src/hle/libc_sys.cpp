// libc: time, identity, process, signals, mmap, raw syscalls, dl*, liblog, and the network
// layer (network lockdown layer 3: every socket and DNS call fails).
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "hle/format.h"
#include "hle/modules.h"
#include "hle/registry.h"
#include "loader/elf.h"

namespace skim {

void guest_syscall(GuestThread& t) {
    auto& R = t.regs();
    switch (R[7]) {
    case 0xF0002:  // __ARM_NR_cacheflush(start, end, flags)
        GuestThread::invalidate_all(R[0], R[1] - R[0]);
        R[0] = 0;
        break;
    case 224:  // gettid
        R[0] = t.tid();
        break;
    case 158:  // sched_yield
        std::this_thread::yield();
        R[0] = 0;
        break;
    default:
        std::fprintf(stderr, "[sys] svc #0 syscall %u unsupported -> ENOSYS\n", R[7]);
        R[0] = static_cast<std::uint32_t>(-ENOSYS);
    }
}

namespace hle {
namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;
char* S(gaddr a) { return gstr(a); }

i32 fail(int e) { set_errno(e); return -1; }

// ---- time ----
i32 h_time(gaddr out) {
    const auto t = static_cast<i32>(std::time(nullptr));
    if (out) mem().write<i32>(out, t);
    return t;
}
i32 h_clock() { return static_cast<i32>(std::clock()); }
i32 h_clock_gettime(i32 clk, gaddr ts) {
    timespec t{};
    if (::clock_gettime(clk, &t) != 0) return fail(errno);
    mem().write<i32>(ts, static_cast<i32>(t.tv_sec));
    mem().write<i32>(ts + 4, static_cast<i32>(t.tv_nsec));
    return 0;
}
i32 h_clock_getres(i32 clk, gaddr ts) {
    timespec t{};
    if (::clock_getres(clk, &t) != 0) return fail(errno);
    if (ts) { mem().write<i32>(ts, static_cast<i32>(t.tv_sec)); mem().write<i32>(ts + 4, static_cast<i32>(t.tv_nsec)); }
    return 0;
}
i32 h_gettimeofday(gaddr tv, gaddr) {
    timespec t{};
    ::clock_gettime(CLOCK_REALTIME, &t);
    mem().write<i32>(tv, static_cast<i32>(t.tv_sec));
    mem().write<i32>(tv + 4, static_cast<i32>(t.tv_nsec / 1000));
    return 0;
}
// Sleeps run signal handlers that arrive meanwhile, then keep sleeping.
void guest_sleep(std::chrono::nanoseconds d) {
    GuestThread& t = *GuestThread::current();
    const auto deadline = GuestThread::WaitClock::now() + std::chrono::duration_cast<GuestThread::WaitClock::duration>(d);
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock lk{m};
    t.wait(lk, cv, [] { return false; }, &deadline);
}
i32 h_nanosleep(gaddr req, gaddr) {
    guest_sleep(std::chrono::seconds(mem().read<i32>(req)) + std::chrono::nanoseconds(mem().read<i32>(req + 4)));
    return 0;
}
i32 h_usleep(u32 us) { guest_sleep(std::chrono::microseconds(us)); return 0; }
u32 h_sleep(u32 s) { guest_sleep(std::chrono::seconds(s)); return 0; }

// bionic struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes)
void put_tm(gaddr out, const std::tm& t) {
    const int f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; ++i) mem().write<i32>(out + 4 * i, f[i]);
    mem().write<i32>(out + 36, static_cast<i32>(t.tm_gmtoff));
    static const gaddr zone = mem().static_str("UTC");
    mem().write<gaddr>(out + 40, zone);
}
std::tm get_tm(gaddr in) {
    std::tm t{};
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; ++i) *f[i] = mem().read<i32>(in + 4 * i);
    return t;
}
gaddr h_localtime_r(gaddr tp, gaddr out) {
    const std::time_t t = mem().read<i32>(tp);
    std::tm r{};
    ::localtime_r(&t, &r);
    put_tm(out, r);
    return out;
}
gaddr h_gmtime_r(gaddr tp, gaddr out) {
    const std::time_t t = mem().read<i32>(tp);
    std::tm r{};
    ::gmtime_r(&t, &r);
    put_tm(out, r);
    return out;
}
gaddr h_localtime(gaddr tp) { static const gaddr buf = mem().static_alloc(44); return h_localtime_r(tp, buf); }
gaddr h_gmtime(gaddr tp) { static const gaddr buf = mem().static_alloc(44); return h_gmtime_r(tp, buf); }
i32 h_mktime(gaddr in) {
    std::tm t = get_tm(in);
    const std::time_t r = std::mktime(&t);
    put_tm(in, t);
    return static_cast<i32>(r);
}
u32 h_strftime(gaddr buf, u32 max, gaddr fmt, gaddr tm) {
    const std::tm t = get_tm(tm);
    return static_cast<u32>(std::strftime(S(buf), max, S(fmt), &t));
}

// ---- identity / system info (a typical 2013 dual-core ARMv7 device) ----
i32 h_getpid() { return static_cast<i32>(::getpid()); }
i32 h_gettid() { return static_cast<i32>(GuestThread::current()->tid()); }
constexpr u32 kAppUid = 10071;  // a typical app uid; the value itself is not significant
u32 h_getuid() { return kAppUid; }
i32 h_getresuid(gaddr r, gaddr e, gaddr s) {
    for (gaddr p : {r, e, s}) if (p) mem().write<u32>(p, kAppUid);
    return 0;
}
i32 h_setresuid(u32, u32, u32) { return fail(EPERM); }
gaddr h_null_ptr(gaddr) { return 0; }
i32 h_zero() { return 0; }
i32 h_zero2(i32, i32) { return 0; }
i32 h_zero3(i32, i32, i32) { return 0; }
i32 h_getdtablesize() { return 1024; }
i32 h_getrusage(i32, gaddr out) { std::memset(mem().ptr(out), 0, 72); return 0; }
i32 h_gethostname(gaddr buf, u32 n) { std::strncpy(S(buf), "localhost", n); return 0; }
void h_prctl(Ctx& c) { c.ret(0); }

i32 h_sysconf(i32 name) {
    switch (name) {  // bionic's _SC_* numbering
    case 0x06: return 100;            // _SC_CLK_TCK
    case 0x0B: return 1024;           // _SC_OPEN_MAX
    case 0x27: case 0x28: return 4096;  // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x60: case 0x61: return 2;   // _SC_NPROCESSORS_CONF / _ONLN (2x Cortex-A9)
    case 0x62: return 262144;         // _SC_PHYS_PAGES (1 GiB)
    case 0x63: return 65536;          // _SC_AVPHYS_PAGES
    default:
        std::fprintf(stderr, "[sys] sysconf(0x%x) unknown -> -1\n", name);
        return fail(EINVAL);
    }
}
i32 h_uname(gaddr out) {
    const char* f[6] = {"Linux", "localhost", "3.4.83", "#1 SMP PREEMPT", "armv7l", "localdomain"};
    std::memset(mem().ptr(out), 0, 6 * 65);
    for (int i = 0; i < 6; ++i) std::strncpy(S(out + 65 * i), f[i], 64);
    return 0;
}
gaddr h_setlocale(i32, gaddr) { static const gaddr c = mem().static_str("C"); return c; }
gaddr h_errno_fn() { return GuestThread::current()->errno_addr(); }

// ---- processes: refused (the game never starts processes) ----
i32 h_fork() { std::fprintf(stderr, "[sys] fork refused\n"); return fail(EAGAIN); }
i32 h_exec(gaddr path, gaddr) { std::fprintf(stderr, "[sys] exec(%s) refused\n", S(path)); return fail(EACCES); }
i32 h_exec3(gaddr path, gaddr, gaddr) { return h_exec(path, 0); }
i32 h_system(gaddr cmd) { std::fprintf(stderr, "[sys] system(%s) refused\n", cmd ? S(cmd) : ""); return -1; }
i32 h_waitpid(i32, gaddr, i32) { return fail(ECHILD); }
i32 h_kill(i32 pid, i32 sig) {
    if (pid == ::getpid() && sig == 0) return 0;
    if (pid == ::getpid() && sig > 0 && sig <= 32) {  // to the process: deliver on this thread
        GuestThread::current()->send_signal(sig);
        GuestThread::current()->deliver_pending();
        return 0;
    }
    std::fprintf(stderr, "[sys] kill(%d, %d) refused\n", pid, sig);
    return fail(EPERM);
}

// ---- signals: dispositions here; delivery in GuestThread (sent by kill/tkill/pthread_kill/raise) ----
SigAction sig_table[65];
std::mutex sig_lock;
i32 h_sigaction(i32 sig, gaddr act, gaddr old) {
    if (sig <= 0 || sig > 64) return fail(EINVAL);
    std::lock_guard g{sig_lock};
    if (old) {
        const SigAction& o = sig_table[sig];
        for (u32 i = 0; i < 4; ++i) mem().write<u32>(old + 4 * i, (&o.handler)[i]);
    }
    if (act) {
        SigAction& n = sig_table[sig];
        for (u32 i = 0; i < 4; ++i) (&n.handler)[i] = mem().read<u32>(act + 4 * i);
    }
    return 0;
}
u32 h_signal(i32 sig, u32 handler) {
    if (sig <= 0 || sig > 64) return static_cast<u32>(fail(EINVAL));
    std::lock_guard g{sig_lock};
    const u32 old = sig_table[sig].handler;
    sig_table[sig] = {handler, 0, 0, 0};
    return old;
}
i32 h_sigprocmask(i32 how, gaddr set, gaddr old) {
    GuestThread& t = *GuestThread::current();
    if (old) mem().write<u32>(old, t.blocked());
    if (set) {
        const u32 s = mem().read<u32>(set);
        t.set_blocked(how == 0 ? (t.blocked() | s) : how == 1 ? (t.blocked() & ~s) : s);  // SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK
    }
    if (t.signal_deliverable()) t.deliver_pending();
    return 0;
}
i32 h_sigsuspend(gaddr mask) {
    GuestThread& t = *GuestThread::current();
    const u32 saved = t.blocked();
    t.set_blocked(mem().read<u32>(mask));
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock lk{m};
    while (!t.signal_deliverable()) t.wait(lk, cv, [&] { return t.signal_deliverable(); });
    lk.unlock();
    t.deliver_pending();
    t.set_blocked(saved);
    return fail(EINTR);
}
i32 h_raise(i32 sig) {
    GuestThread& t = *GuestThread::current();
    if (sig < 1 || sig > 32) return fail(EINVAL);
    t.send_signal(sig);
    t.deliver_pending();
    return 0;
}
i32 h_setitimer(i32 which, gaddr, gaddr old) {
    std::fprintf(stderr, "[sys] setitimer(%d) ignored\n", which);
    if (old) std::memset(mem().ptr(old), 0, 16);
    return 0;
}

// setjmp / longjmp: r4-r11, sp, lr in words 0-9; d8-d15 in words 10-25.
void h_setjmp(Ctx& c) {
    const gaddr env = c.u32();
    auto& R = c.t.regs();
    for (int i = 0; i < 8; ++i) mem().write<u32>(env + 4 * i, R[4 + i]);
    mem().write<u32>(env + 32, R[13]);
    mem().write<u32>(env + 36, R[14]);
    const auto& X = c.t.jit().ExtRegs();
    for (int i = 0; i < 16; ++i) mem().write<u32>(env + 40 + 4 * i, X[16 + i]);
    c.ret(0);
}
void h_longjmp(Ctx& c) {
    const gaddr env = c.u32();
    const u32 val = c.u32();
    auto& R = c.t.regs();
    for (int i = 0; i < 8; ++i) R[4 + i] = mem().read<u32>(env + 4 * i);
    R[13] = mem().read<u32>(env + 32);
    auto& X = c.t.jit().ExtRegs();
    for (int i = 0; i < 16; ++i) X[16 + i] = mem().read<u32>(env + 40 + 4 * i);
    R[0] = val ? val : 1;
    c.t.set_pc(mem().read<u32>(env + 36));
    c.no_return = true;
}

// ---- memory mapping ----
constexpr u32 kMapFailed = 0xFFFF'FFFF;
u32 do_mmap(u32 addr, u32 len, i32 flags, i32 fd, std::int64_t off) {
    Memory& m = mem();
    gaddr a;
    if (flags & MAP_FIXED) {
        m.unmap(addr, len);
        a = m.map_fixed(addr, len) ? addr : 0;
    } else {
        a = m.map_anywhere(len);
    }
    if (!a) { set_errno(ENOMEM); return kMapFailed; }
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        const ssize_t n = ::pread(fd, m.ptr(a), len, off);
        if (n < 0) { m.unmap(a, len); set_errno(errno); return kMapFailed; }
    }
    return a;
}
u32 h_mmap(u32 addr, u32 len, i32, i32 flags, i32 fd, i32 off) { return do_mmap(addr, len, flags, fd, off); }
u32 h_mmap64(u32 addr, u32 len, i32, i32 flags, i32 fd, std::int64_t off) { return do_mmap(addr, len, flags, fd, off); }
i32 h_munmap(u32 addr, u32 len) { mem().unmap(addr, len); return 0; }
i32 h_mprotect(u32, u32, i32) { return 0; }  // the arena is always read/write; the JIT reads code itself
i32 h_madvise(u32, u32, i32) { return 0; }
void h_mremap(Ctx& c) {
    const u32 old = c.u32(), old_len = c.u32(), len = c.u32();
    const gaddr a = mem().map_anywhere(len);
    if (!a) { set_errno(ENOMEM); return c.ret(kMapFailed); }
    std::memcpy(mem().ptr(a), mem().ptr(old), std::min(old_len, len));
    mem().unmap(old, old_len);
    c.ret(a);
}
i32 h_cacheflush(u32 start, u32 end, i32) { GuestThread::invalidate_all(start, end - start); return 0; }
void h_syscall(Ctx& c) {
    const u32 n = c.u32();
    auto& R = c.t.regs();
    const u32 a0 = c.u32(), a1 = c.u32(), a2 = c.u32();
    (void)a2;
    switch (n) {
    case 0xF0002: GuestThread::invalidate_all(a0, a1 - a0); return c.ret(0);
    case 224: return c.ret(c.t.tid());
    default:
        std::fprintf(stderr, "[sys] syscall(%u) unsupported -> ENOSYS\n", n);
        set_errno(ENOSYS);
        (void)R;
        c.ret(static_cast<u32>(-1));
    }
}

// ---- dynamic linking: guest libraries we loaded, plus HLE "system" libraries ----
constexpr u32 kHleHandle = 0x7000'0000;
thread_local std::string dl_error;
gaddr h_dlopen(gaddr name, i32) {
    if (!name) return kHleHandle;
    std::string n = S(name);
    if (const auto s = n.rfind('/'); s != std::string::npos) n = n.substr(s + 1);
    const auto& libs = loader::all();
    for (std::size_t i = 0; i < libs.size(); ++i)
        if (libs[i]->name == n) return kHleHandle + 1 + static_cast<u32>(i);
    for (const char* sys : {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libstdc++.so", "libGLESv1_CM.so", "libGLESv2.so", "libEGL.so", "libandroid.so", "libOpenSLES.so"})
        if (n == sys) return kHleHandle;
    std::fprintf(stderr, "[dl] dlopen(%s) -> not found\n", S(name));
    dl_error = "dlopen failed: library \"" + std::string(S(name)) + "\" not found";
    return 0;
}
gaddr h_dlsym(gaddr handle, gaddr name) {
    const std::string n = S(name);
    gaddr r = 0;
    const auto& libs = loader::all();
    if (handle > kHleHandle && handle <= kHleHandle + libs.size()) {
        auto& ex = libs[handle - kHleHandle - 1]->exports;
        if (auto it = ex.find(n); it != ex.end()) r = it->second;
    } else {
        r = loader::lookup(n);
        if (!r && has_function(n)) r = function_thunk(n);
        if (!r) r = data(n);
    }
    if (!r) {
        std::fprintf(stderr, "[dl] dlsym(%s) -> not found\n", n.c_str());
        dl_error = "undefined symbol: " + n;
    }
    return r;
}
i32 h_dlclose(gaddr) { return 0; }
gaddr h_dlerror() {
    static thread_local gaddr buf = 0;
    if (dl_error.empty()) return 0;
    if (!buf) buf = heap::malloc(256);
    std::strncpy(S(buf), dl_error.c_str(), 255);
    S(buf)[255] = 0;
    dl_error.clear();
    return buf;
}
i32 h_dladdr(gaddr addr, gaddr info) {
    const Library* l = loader::containing(addr);
    if (!l) return 0;
    mem().write<gaddr>(info, mem().static_str(l->name));
    mem().write<gaddr>(info + 4, l->base);
    mem().write<gaddr>(info + 8, 0);
    mem().write<gaddr>(info + 12, 0);
    return 1;
}
gaddr h_find_exidx(gaddr pc, gaddr count) {
    const Library* l = loader::containing(pc);
    if (!l || !l->exidx) { mem().write<i32>(count, 0); return 0; }
    mem().write<i32>(count, static_cast<i32>(l->exidx_count));
    return l->exidx;
}

// ---- liblog ----
const char* prio_char(u32 p) { return p < 8 ? &"??VDIWEF"[p] : "?"; }
void log_line(u32 prio, gaddr tag, const std::string& msg) {
    std::fprintf(stderr, "[log] %c/%s: %s%s", *prio_char(prio), tag ? S(tag) : "", msg.c_str(),
                 !msg.empty() && msg.back() == '\n' ? "" : "\n");
}
void h_log_print(Ctx& c) { const u32 p = c.u32(); const gaddr tag = c.u32(), f = c.u32(); CtxArgs a(c); log_line(p, tag, format(S(f), a)); c.ret(1); }
void h_log_vprint(Ctx& c) { const u32 p = c.u32(); const gaddr tag = c.u32(), f = c.u32(); VaListArgs a(c.u32()); log_line(p, tag, format(S(f), a)); c.ret(1); }
i32 h_log_write(u32 p, gaddr tag, gaddr msg) { log_line(p, tag, S(msg)); return 1; }

// ---- network: always fails (lockdown layer 3) ----
void blocked(const char* what) { std::fprintf(stderr, "[net] blocked %s\n", what); }
i32 h_socket(i32 domain, i32 type, i32) {
    std::fprintf(stderr, "[net] blocked socket(domain=%d, type=%d)\n", domain, type);
    return fail(EACCES);
}
#define SKIM_NET_FAIL(name, ...)                                \
    i32 h_##name(__VA_ARGS__) { blocked(#name); return fail(EBADF); }
SKIM_NET_FAIL(connect, i32, gaddr, u32)
SKIM_NET_FAIL(bind, i32, gaddr, u32)
SKIM_NET_FAIL(listen, i32, i32)
SKIM_NET_FAIL(accept, i32, gaddr, gaddr)
SKIM_NET_FAIL(send, i32, gaddr, u32, i32)
SKIM_NET_FAIL(recv, i32, gaddr, u32, i32)
SKIM_NET_FAIL(sendmsg, i32, gaddr, i32)
SKIM_NET_FAIL(recvmsg, i32, gaddr, i32)
SKIM_NET_FAIL(shutdown, i32, i32)
SKIM_NET_FAIL(getpeername, i32, gaddr, gaddr)
SKIM_NET_FAIL(getsockname, i32, gaddr, gaddr)
SKIM_NET_FAIL(getsockopt, i32, i32, i32, gaddr, gaddr)
SKIM_NET_FAIL(setsockopt, i32, i32, i32, gaddr, u32)
SKIM_NET_FAIL(sendfile, i32, i32, gaddr, u32)
#undef SKIM_NET_FAIL
void h_sendto(Ctx& c) { blocked("sendto"); set_errno(EBADF); c.ret(static_cast<u32>(-1)); }
void h_recvfrom(Ctx& c) { blocked("recvfrom"); set_errno(EBADF); c.ret(static_cast<u32>(-1)); }
constexpr i32 kEaiFail = 4;     // bionic EAI_FAIL
constexpr i32 kNoRecovery = 3;  // h_errno NO_RECOVERY
gaddr h_errno_storage = 0;
i32 h_getaddrinfo(gaddr node, gaddr, gaddr, gaddr res) {
    std::fprintf(stderr, "[net] blocked getaddrinfo(%s)\n", node ? S(node) : "");
    if (res) mem().write<gaddr>(res, 0);
    return kEaiFail;
}
void h_freeaddrinfo(gaddr) {}
gaddr h_gai_strerror(i32) { static const gaddr s = mem().static_str("Network access is disabled"); return s; }
gaddr h_gethostbyname(gaddr name) {
    std::fprintf(stderr, "[net] blocked gethostbyname(%s)\n", name ? S(name) : "");
    mem().write<i32>(h_errno_storage, kNoRecovery);
    return 0;
}
gaddr h_gethostbyaddr(gaddr, u32, i32) { blocked("gethostbyaddr"); mem().write<i32>(h_errno_storage, kNoRecovery); return 0; }
gaddr h_get_h_errno() { return h_errno_storage; }
}  // namespace

SigAction sigaction_for(int sig) {
    std::lock_guard g{sig_lock};
    return sig > 0 && sig <= 64 ? sig_table[sig] : SigAction{};
}

void register_libc_sys() {
    def("time", wrap<h_time>); def("clock", wrap<h_clock>); def("clock_gettime", wrap<h_clock_gettime>);
    def("clock_getres", wrap<h_clock_getres>); def("gettimeofday", wrap<h_gettimeofday>); def("nanosleep", wrap<h_nanosleep>);
    def("usleep", wrap<h_usleep>); def("sleep", wrap<h_sleep>); def("localtime", wrap<h_localtime>); def("localtime_r", wrap<h_localtime_r>);
    def("gmtime", wrap<h_gmtime>); def("gmtime_r", wrap<h_gmtime_r>); def("mktime", wrap<h_mktime>); def("strftime", wrap<h_strftime>);
    def("getpid", wrap<h_getpid>); def("gettid", wrap<h_gettid>); def("getuid", wrap<h_getuid>); def("geteuid", wrap<h_getuid>);
    def("getgid", wrap<h_getuid>); def("getegid", wrap<h_getuid>); def("getresuid", wrap<h_getresuid>); def("setresuid", wrap<h_setresuid>);
    def("getpwuid", wrap<h_null_ptr>); def("getpwnam", wrap<h_null_ptr>); def("getgrgid", wrap<h_null_ptr>); def("getgrnam", wrap<h_null_ptr>);
    def("getpriority", wrap<h_zero2>); def("setpriority", wrap<h_zero3>); def("prctl", h_prctl);
    def("getdtablesize", wrap<h_getdtablesize>); def("getrusage", wrap<h_getrusage>); def("gethostname", wrap<h_gethostname>);
    def("sysconf", wrap<h_sysconf>); def("uname", wrap<h_uname>); def("setlocale", wrap<h_setlocale>); def("__errno", wrap<h_errno_fn>);
    def("sched_yield", wrap<h_zero>);
    def("fork", wrap<h_fork>); def("execv", wrap<h_exec>); def("execve", wrap<h_exec3>); def("system", wrap<h_system>);
    def("waitpid", wrap<h_waitpid>); def("kill", wrap<h_kill>);
    def("sigaction", wrap<h_sigaction>); def("signal", wrap<h_signal>); def("bsd_signal", wrap<h_signal>);
    def("sigprocmask", wrap<h_sigprocmask>); def("sigsuspend", wrap<h_sigsuspend>); def("raise", wrap<h_raise>);
    def("setitimer", wrap<h_setitimer>);
    def("setjmp", h_setjmp); def("_setjmp", h_setjmp); def("sigsetjmp", h_setjmp);
    def("longjmp", h_longjmp); def("_longjmp", h_longjmp); def("siglongjmp", h_longjmp);
    def("mmap", wrap<h_mmap>); def("mmap64", wrap<h_mmap64>); def("munmap", wrap<h_munmap>); def("mprotect", wrap<h_mprotect>);
    def("madvise", wrap<h_madvise>); def("mremap", h_mremap); def("cacheflush", wrap<h_cacheflush>); def("syscall", h_syscall);
    def("dlopen", wrap<h_dlopen>); def("dlsym", wrap<h_dlsym>); def("dlclose", wrap<h_dlclose>); def("dlerror", wrap<h_dlerror>);
    def("dladdr", wrap<h_dladdr>); def("__gnu_Unwind_Find_exidx", wrap<h_find_exidx>);
    def("__android_log_print", h_log_print); def("__android_log_vprint", h_log_vprint); def("__android_log_write", wrap<h_log_write>);
    def("socket", wrap<h_socket>); def("connect", wrap<h_connect>); def("bind", wrap<h_bind>); def("listen", wrap<h_listen>);
    def("accept", wrap<h_accept>); def("send", wrap<h_send>); def("recv", wrap<h_recv>); def("sendmsg", wrap<h_sendmsg>);
    def("recvmsg", wrap<h_recvmsg>); def("shutdown", wrap<h_shutdown>); def("getpeername", wrap<h_getpeername>);
    def("getsockname", wrap<h_getsockname>); def("getsockopt", wrap<h_getsockopt>); def("setsockopt", wrap<h_setsockopt>);
    def("sendfile", wrap<h_sendfile>); def("sendto", h_sendto); def("recvfrom", h_recvfrom);
    def("getaddrinfo", wrap<h_getaddrinfo>); def("freeaddrinfo", wrap<h_freeaddrinfo>); def("gai_strerror", wrap<h_gai_strerror>);
    def("gethostbyname", wrap<h_gethostbyname>); def("gethostbyaddr", wrap<h_gethostbyaddr>); def("__get_h_errno", wrap<h_get_h_errno>);
    h_errno_storage = mem().static_alloc(4);

    const gaddr page_size = mem().static_alloc(4);
    mem().write<u32>(page_size, 4096);
    def_data("__page_size", page_size);
}

}  // namespace hle
}  // namespace skim
