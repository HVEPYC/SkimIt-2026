// pthread and semaphores. Guest threads are host threads, each with its own GuestThread (JIT).
// bionic's pthread objects are 4-byte words in guest memory; each maps to a host object keyed by
// its guest address (created on first use, so static initializers work).
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;
using Clock = std::chrono::system_clock;

Clock::time_point abs_time(gaddr ts) {
    const auto s = mem().read<i32>(ts), ns = mem().read<i32>(ts + 4);
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(s) + std::chrono::nanoseconds(ns)));
}

template<typename T>
struct Objects {
    std::mutex lock;
    std::unordered_map<gaddr, T*> map;
    T* get(gaddr a) {
        std::lock_guard g{lock};
        T*& p = map[a];
        if (!p) p = new T(a);
        return p;
    }
    void erase(gaddr a) {
        std::lock_guard g{lock};
        if (auto it = map.find(a); it != map.end()) { delete it->second; map.erase(it); }
    }
};

// ---- mutex (bionic: type in bits 14-15 of the word: 0 normal, 1 recursive, 2 error-check) ----
struct HMutex {
    explicit HMutex(gaddr a) : type((mem().read<u32>(a) >> 14) & 3) {}
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0, count = 0, type;

    int lock(bool try_only) {
        const u32 me = GuestThread::current()->tid();
        std::unique_lock g{m};
        if (owner == me) {
            if (type == 1) { ++count; return 0; }
            if (type == 2 || try_only) return try_only ? EBUSY : EDEADLK;
        }
        if (try_only && owner) return EBUSY;
        GuestThread::current()->wait(g, cv, [&] { return owner == 0; });
        owner = me;
        count = 1;
        return 0;
    }
    int unlock() {
        std::lock_guard g{m};
        if (owner != GuestThread::current()->tid()) return EPERM;
        if (--count == 0) { owner = 0; cv.notify_one(); }
        return 0;
    }
    // For condition waits: drop the mutex entirely and return the recursion count.
    u32 release() {
        std::lock_guard g{m};
        const u32 c = count;
        owner = 0;
        count = 0;
        cv.notify_one();
        return c;
    }
    void reacquire(u32 c) {
        std::unique_lock g{m};
        GuestThread::current()->wait(g, cv, [&] { return owner == 0; });
        owner = GuestThread::current()->tid();
        count = c;
    }
};
Objects<HMutex>& mutexes() { static Objects<HMutex> o; return o; }

i32 h_mutexattr_init(gaddr a) { mem().write<u32>(a, 0); return 0; }
i32 h_mutexattr_destroy(gaddr) { return 0; }
i32 h_mutexattr_settype(gaddr a, i32 type) { mem().write<u32>(a, static_cast<u32>(type)); return 0; }
i32 h_mutexattr_gettype(gaddr a, gaddr out) { mem().write<u32>(out, mem().read<u32>(a)); return 0; }
i32 h_mutex_init(gaddr m, gaddr attr) {
    mutexes().erase(m);
    mem().write<u32>(m, attr ? (mem().read<u32>(attr) & 3) << 14 : 0);
    return 0;
}
i32 h_mutex_destroy(gaddr m) { mutexes().erase(m); return 0; }
i32 h_mutex_lock(gaddr m) { return mutexes().get(m)->lock(false); }
i32 h_mutex_trylock(gaddr m) { return mutexes().get(m)->lock(true); }
i32 h_mutex_unlock(gaddr m) { return mutexes().get(m)->unlock(); }

// ---- condition variables ----
struct HCond {
    explicit HCond(gaddr) {}
    std::mutex m;
    std::condition_variable cv;
    std::uint64_t seq = 0;
};
Objects<HCond>& conds() { static Objects<HCond> o; return o; }

i32 cond_wait(gaddr cond, gaddr mutex, const Clock::time_point* deadline) {
    HCond* c = conds().get(cond);
    HMutex* m = mutexes().get(mutex);
    std::unique_lock g{c->m};
    const std::uint64_t seen = c->seq;
    const u32 count = m->release();
    const bool timed_out = !GuestThread::current()->wait(g, c->cv, [&] { return c->seq != seen; }, deadline);
    g.unlock();
    m->reacquire(count);
    return timed_out ? ETIMEDOUT : 0;
}
i32 h_cond_init(gaddr c, gaddr) { conds().erase(c); mem().write<u32>(c, 0); return 0; }
i32 h_cond_destroy(gaddr c) { conds().erase(c); return 0; }
i32 h_cond_wait(gaddr c, gaddr m) { return cond_wait(c, m, nullptr); }
i32 h_cond_timedwait(gaddr c, gaddr m, gaddr ts) { const auto d = abs_time(ts); return cond_wait(c, m, &d); }
i32 h_cond_signal(gaddr c) { HCond* h = conds().get(c); std::lock_guard g{h->m}; ++h->seq; h->cv.notify_one(); return 0; }
i32 h_cond_broadcast(gaddr c) { HCond* h = conds().get(c); std::lock_guard g{h->m}; ++h->seq; h->cv.notify_all(); return 0; }

// ---- semaphores (bionic sem_t: one word) ----
struct HSem {
    explicit HSem(gaddr a) : count(mem().read<u32>(a)) {}
    std::mutex m;
    std::condition_variable cv;
    u32 count;
};
Objects<HSem>& sems() { static Objects<HSem> o; return o; }
i32 h_sem_init(gaddr s, i32, u32 value) { sems().erase(s); mem().write<u32>(s, value); sems().get(s); return 0; }
i32 h_sem_destroy(gaddr s) { sems().erase(s); return 0; }
i32 h_sem_post(gaddr s) { HSem* h = sems().get(s); std::lock_guard g{h->m}; ++h->count; h->cv.notify_one(); return 0; }
i32 h_sem_wait(gaddr s) {
    HSem* h = sems().get(s);
    std::unique_lock g{h->m};
    GuestThread::current()->wait(g, h->cv, [&] { return h->count > 0; });
    --h->count;
    return 0;
}
i32 h_sem_trywait(gaddr s) {
    HSem* h = sems().get(s);
    std::lock_guard g{h->m};
    if (!h->count) { set_errno(EAGAIN); return -1; }
    --h->count;
    return 0;
}
i32 h_sem_timedwait(gaddr s, gaddr ts) {
    HSem* h = sems().get(s);
    std::unique_lock g{h->m};
    const auto deadline = abs_time(ts);
    if (!GuestThread::current()->wait(g, h->cv, [&] { return h->count > 0; }, &deadline)) { set_errno(ETIMEDOUT); return -1; }
    --h->count;
    return 0;
}
i32 h_sem_getvalue(gaddr s, gaddr out) { HSem* h = sems().get(s); std::lock_guard g{h->m}; mem().write<u32>(out, h->count); return 0; }

// ---- thread-specific data ----
constexpr u32 kMaxKeys = 128;
std::mutex key_lock;
bool key_used[kMaxKeys];
gaddr key_dtor[kMaxKeys];
thread_local u32 key_values[kMaxKeys];
i32 h_key_create(gaddr out, gaddr dtor) {
    std::lock_guard g{key_lock};
    for (u32 k = 0; k < kMaxKeys; ++k)
        if (!key_used[k]) { key_used[k] = true; key_dtor[k] = dtor; mem().write<u32>(out, k); return 0; }
    return EAGAIN;
}
i32 h_key_delete(u32 k) { std::lock_guard g{key_lock}; if (k < kMaxKeys) key_used[k] = false; return 0; }
u32 h_getspecific(u32 k) { return k < kMaxKeys ? key_values[k] : 0; }
i32 h_setspecific(u32 k, u32 v) { if (k >= kMaxKeys) return EINVAL; key_values[k] = v; return 0; }
void run_key_destructors(GuestThread& t) {
    for (int round = 0; round < 4; ++round)
        for (u32 k = 0; k < kMaxKeys; ++k) {
            const u32 v = key_values[k];
            gaddr d;
            { std::lock_guard g{key_lock}; d = key_used[k] ? key_dtor[k] : 0; }
            if (v && d) { key_values[k] = 0; t.call(d, {v}); }
        }
}

// ---- once (states: 0 not run, 1 running, 2 done) ----
i32 h_once(gaddr once, gaddr fn) {
    auto* w = mem().ptr<u32>(once);
    u32 expected = 0;
    if (__atomic_compare_exchange_n(w, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        GuestThread::current()->call(fn, {});
        __atomic_store_n(w, 2u, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(w, __ATOMIC_ACQUIRE) != 2) std::this_thread::yield();
    }
    return 0;
}

// ---- attributes (bionic pthread_attr_t: flags, stack_base, stack_size, guard_size, policy, priority) ----
constexpr u32 kDetached = 1;
i32 h_attr_init(gaddr a) {
    for (u32 i = 0; i < 6; ++i) mem().write<u32>(a + 4 * i, 0);
    mem().write<u32>(a + 8, 1024 * 1024);
    mem().write<u32>(a + 12, 4096);
    return 0;
}
i32 h_attr_destroy(gaddr) { return 0; }
i32 h_attr_setstacksize(gaddr a, u32 s) { mem().write<u32>(a + 8, s); return 0; }
i32 h_attr_getstacksize(gaddr a, gaddr out) { mem().write<u32>(out, mem().read<u32>(a + 8)); return 0; }
i32 h_attr_getstack(gaddr a, gaddr base, gaddr size) {
    mem().write<u32>(base, mem().read<u32>(a + 4));
    mem().write<u32>(size, mem().read<u32>(a + 8));
    return 0;
}
i32 h_attr_setdetachstate(gaddr a, i32 s) { mem().write<u32>(a, s ? kDetached : 0); return 0; }
i32 h_attr_getdetachstate(gaddr a, gaddr out) { mem().write<u32>(out, mem().read<u32>(a) & kDetached); return 0; }
i32 h_attr_ignore(gaddr, i32) { return 0; }

// ---- threads (pthread_t = the thread's TLS block address) ----
struct ThreadRec {
    GuestThread* guest = nullptr;
    std::shared_future<u32> result;
    gaddr stack_lo = 0, stack_hi = 0;
    bool detached = false;
};
std::mutex thread_lock;
std::unordered_map<gaddr, ThreadRec> threads;
struct ThreadExit { u32 value; };

i32 h_create(gaddr out, gaddr attr, gaddr start, gaddr arg) {
    const u32 stack = attr ? mem().read<u32>(attr + 8) : 0;
    const bool detached = attr && (mem().read<u32>(attr) & kDetached);
    auto ready = std::make_shared<std::promise<gaddr>>();
    auto done = std::make_shared<std::promise<u32>>();
    std::shared_future<u32> result = done->get_future().share();
    std::thread([=] {
        GuestThread t(stack);
        if (std::getenv("SKIM_TRACE_THREADS"))
            std::fprintf(stderr, "[thread] pthread_create: tid %u handle 0x%08x start %s\n", t.tid(), t.tls(), describe(start).c_str());
        {
            std::lock_guard g{thread_lock};
            threads[t.tls()] = {&t, result, t.stack_lo(), t.stack_hi(), detached};
        }
        ready->set_value(t.tls());
        u32 ret = 0;
        try {
            ret = static_cast<u32>(t.call(start, {arg}));
        } catch (const ThreadExit& e) {
            ret = e.value;
        }
        run_key_destructors(t);
        {
            std::lock_guard g{thread_lock};
            auto it = threads.find(t.tls());
            if (it != threads.end()) { if (it->second.detached) threads.erase(it); else it->second.guest = nullptr; }
        }
        done->set_value(ret);
    }).detach();
    const gaddr handle = ready->get_future().get();
    mem().write<gaddr>(out, handle);
    return 0;
}
void h_exit(u32 value) { throw ThreadExit{value}; }
i32 h_join(gaddr th, gaddr out) {
    std::shared_future<u32> f;
    {
        std::lock_guard g{thread_lock};
        auto it = threads.find(th);
        if (it == threads.end()) return ESRCH;
        f = it->second.result;
    }
    const u32 v = f.get();
    if (out) mem().write<u32>(out, v);
    std::lock_guard g{thread_lock};
    threads.erase(th);
    return 0;
}
i32 h_detach(gaddr th) {
    std::lock_guard g{thread_lock};
    auto it = threads.find(th);
    if (it == threads.end()) return ESRCH;
    if (!it->second.guest) threads.erase(it); else it->second.detached = true;
    return 0;
}
gaddr h_self() {
    static const bool trace = std::getenv("SKIM_TRACE_THREADS") != nullptr;
    thread_local bool seen = false;
    if (trace && !seen) {
        seen = true;
        std::fprintf(stderr, "[thread] tid %u: pthread_self = 0x%08x (first call, from %s)\n", GuestThread::current()->tid(),
                     GuestThread::current()->tls(), describe(GuestThread::current()->regs()[14]).c_str());
    }
    return GuestThread::current()->tls();
}
i32 h_equal(gaddr a, gaddr b) { return a == b; }
i32 h_getattr_np(gaddr th, gaddr attr) {
    h_attr_init(attr);
    GuestThread* cur = GuestThread::current();
    gaddr lo = 0, hi = 0;
    if (th == cur->tls()) { lo = cur->stack_lo(); hi = cur->stack_hi(); }
    else {
        std::lock_guard g{thread_lock};
        auto it = threads.find(th);
        if (it == threads.end()) return ESRCH;
        lo = it->second.stack_lo;
        hi = it->second.stack_hi;
    }
    mem().write<u32>(attr + 4, lo);
    mem().write<u32>(attr + 8, hi - lo);
    return 0;
}
i32 send_to(GuestThread* target, i32 sig) {
    if (!target) return ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > 32) return EINVAL;
    target->send_signal(sig);
    return 0;
}
i32 h_kill(gaddr th, i32 sig) {
    i32 r;
    {
        auto g = GuestThread::signal_lock();
        r = send_to(GuestThread::by_handle(th), sig);
    }
    GuestThread& self = *GuestThread::current();
    if (th == self.tls() && self.signal_deliverable()) self.deliver_pending();
    return r;
}
i32 h_tkill(i32 tid, i32 sig) {
    i32 r;
    {
        auto g = GuestThread::signal_lock();
        r = send_to(GuestThread::by_tid(static_cast<u32>(tid)), sig);
    }
    GuestThread& self = *GuestThread::current();
    if (static_cast<u32>(tid) == self.tid() && self.signal_deliverable()) self.deliver_pending();
    if (r) { set_errno(r); return -1; }
    return 0;
}
i32 h_getschedparam(gaddr, gaddr policy, gaddr param) {
    if (policy) mem().write<i32>(policy, 0);  // SCHED_OTHER
    if (param) mem().write<i32>(param, 0);
    return 0;
}
i32 h_setschedparam(gaddr, i32, gaddr) { return 0; }  // the original tolerated failures here; priorities are not mapped
i32 h_sched_priority_max(i32) { return 0; }
i32 h_sigmask(i32 how, gaddr set, gaddr old) {
    GuestThread& t = *GuestThread::current();
    if (old) mem().write<u32>(old, t.blocked());
    if (set) {
        const u32 s = mem().read<u32>(set);
        t.set_blocked(how == 0 ? (t.blocked() | s) : how == 1 ? (t.blocked() & ~s) : s);
    }
    if (t.signal_deliverable()) t.deliver_pending();
    return 0;
}

// bionic cleanup handlers: struct { prev, routine, arg }.
thread_local gaddr cleanup_top = 0;
void h_cleanup_push(gaddr c, gaddr routine, gaddr arg) {
    mem().write<gaddr>(c, cleanup_top);
    mem().write<gaddr>(c + 4, routine);
    mem().write<gaddr>(c + 8, arg);
    cleanup_top = c;
}
void h_cleanup_pop(gaddr c, i32 execute) {
    cleanup_top = mem().read<gaddr>(c);
    if (execute) GuestThread::current()->call(mem().read<gaddr>(c + 4), {mem().read<gaddr>(c + 8)});
}
}  // namespace

void register_pthread() {
    def("pthread_mutexattr_init", wrap<h_mutexattr_init>); def("pthread_mutexattr_destroy", wrap<h_mutexattr_destroy>);
    def("pthread_mutexattr_settype", wrap<h_mutexattr_settype>); def("pthread_mutexattr_gettype", wrap<h_mutexattr_gettype>);
    def("pthread_mutex_init", wrap<h_mutex_init>); def("pthread_mutex_destroy", wrap<h_mutex_destroy>);
    def("pthread_mutex_lock", wrap<h_mutex_lock>); def("pthread_mutex_trylock", wrap<h_mutex_trylock>);
    def("pthread_mutex_unlock", wrap<h_mutex_unlock>);
    def("pthread_cond_init", wrap<h_cond_init>); def("pthread_cond_destroy", wrap<h_cond_destroy>);
    def("pthread_cond_wait", wrap<h_cond_wait>); def("pthread_cond_timedwait", wrap<h_cond_timedwait>);
    def("pthread_cond_signal", wrap<h_cond_signal>); def("pthread_cond_broadcast", wrap<h_cond_broadcast>);
    def("sem_init", wrap<h_sem_init>); def("sem_destroy", wrap<h_sem_destroy>); def("sem_post", wrap<h_sem_post>);
    def("sem_wait", wrap<h_sem_wait>); def("sem_trywait", wrap<h_sem_trywait>); def("sem_timedwait", wrap<h_sem_timedwait>);
    def("sem_getvalue", wrap<h_sem_getvalue>);
    def("pthread_key_create", wrap<h_key_create>); def("pthread_key_delete", wrap<h_key_delete>);
    def("pthread_getspecific", wrap<h_getspecific>); def("pthread_setspecific", wrap<h_setspecific>);
    def("pthread_once", wrap<h_once>);
    def("pthread_attr_init", wrap<h_attr_init>); def("pthread_attr_destroy", wrap<h_attr_destroy>);
    def("pthread_attr_setstacksize", wrap<h_attr_setstacksize>); def("pthread_attr_getstacksize", wrap<h_attr_getstacksize>);
    def("pthread_attr_getstack", wrap<h_attr_getstack>); def("pthread_attr_setdetachstate", wrap<h_attr_setdetachstate>);
    def("pthread_attr_getdetachstate", wrap<h_attr_getdetachstate>);
    def("pthread_attr_setschedpolicy", wrap<h_attr_ignore>); def("pthread_attr_setschedparam", wrap<h_attr_ignore>);
    def("pthread_create", wrap<h_create>); def("pthread_exit", wrap<h_exit>); def("pthread_join", wrap<h_join>);
    def("pthread_detach", wrap<h_detach>); def("pthread_self", wrap<h_self>); def("pthread_equal", wrap<h_equal>);
    def("pthread_getattr_np", wrap<h_getattr_np>); def("pthread_kill", wrap<h_kill>); def("tkill", wrap<h_tkill>);
    def("pthread_sigmask", wrap<h_sigmask>);
    def("pthread_getschedparam", wrap<h_getschedparam>); def("pthread_setschedparam", wrap<h_setschedparam>);
    def("sched_get_priority_max", wrap<h_sched_priority_max>); def("sched_get_priority_min", wrap<h_sched_priority_max>);
    def("__pthread_cleanup_push", wrap<h_cleanup_push>); def("__pthread_cleanup_pop", wrap<h_cleanup_pop>);
}

}  // namespace skim::hle
