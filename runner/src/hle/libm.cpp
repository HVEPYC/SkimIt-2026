// libm (softfp: doubles in r0:r1, floats in r0), plus fd waiting and address parsing helpers.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/select.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;

#define SKIM_M1(n) double m_##n(double x) { return std::n(x); } float m_##n##f(float x) { return std::n(x); }
#define SKIM_M2(n) double m_##n(double x, double y) { return std::n(x, y); } float m_##n##f(float x, float y) { return std::n(x, y); }
SKIM_M1(acos) SKIM_M1(asin) SKIM_M1(atan) SKIM_M1(cos) SKIM_M1(sin) SKIM_M1(tan) SKIM_M1(cosh) SKIM_M1(sinh) SKIM_M1(tanh)
SKIM_M1(exp) SKIM_M1(log) SKIM_M1(log10) SKIM_M1(sqrt) SKIM_M1(floor) SKIM_M1(ceil) SKIM_M1(rint) SKIM_M1(round)
SKIM_M1(trunc) SKIM_M1(fabs)
SKIM_M2(atan2) SKIM_M2(pow) SKIM_M2(fmod) SKIM_M2(hypot)
#undef SKIM_M1
#undef SKIM_M2
double m_ldexp(double x, i32 e) { return std::ldexp(x, e); }
double m_frexp(double x, gaddr e) { int v; const double r = std::frexp(x, &v); mem().write<i32>(e, v); return r; }
double m_modf(double x, gaddr ip) { double i; const double r = std::modf(x, &i); mem().write<double>(ip, i); return r; }

// ---- fd waiting (struct pollfd and fd_set have the same layout on ARM and the host) ----
i32 h_poll(gaddr fds, u32 n, i32 timeout) {
    const i32 r = ::poll(mem().ptr<pollfd>(fds), n, timeout);
    if (r < 0) set_errno(errno);
    return r;
}
void h_select(Ctx& c) {
    const i32 n = static_cast<i32>(c.u32());
    const gaddr r = c.u32(), w = c.u32(), e = c.u32(), tv = c.u32();
    timeval t{};
    if (tv) { t.tv_sec = mem().read<i32>(tv); t.tv_usec = mem().read<i32>(tv + 4); }
    auto set = [](gaddr a) { return a ? mem().ptr<fd_set>(a) : nullptr; };
    const i32 res = ::select(n, set(r), set(w), set(e), tv ? &t : nullptr);
    if (res < 0) set_errno(errno);
    c.ret(static_cast<u32>(res));
}
// ARM EABI struct epoll_event is 16 bytes (events, pad, u64 data); x86_64's is packed (12 bytes).
i32 h_epoll_create(i32 size) { const int r = ::epoll_create(size); if (r < 0) set_errno(errno); return r; }
i32 h_epoll_ctl(i32 ep, i32 op, i32 fd, gaddr ev) {
    epoll_event e{};
    if (ev) { e.events = mem().read<u32>(ev); e.data.u64 = mem().read<std::uint64_t>(ev + 8); }
    const int r = ::epoll_ctl(ep, op, fd, ev ? &e : nullptr);
    if (r < 0) set_errno(errno);
    return r;
}
i32 h_epoll_wait(i32 ep, gaddr out, i32 max, i32 timeout) {
    std::vector<epoll_event> evs(static_cast<std::size_t>(max > 0 ? max : 1));
    const int r = ::epoll_wait(ep, evs.data(), max, timeout);
    if (r < 0) { set_errno(errno); return r; }
    for (int i = 0; i < r; ++i) {
        mem().write<u32>(out + 16 * i, evs[i].events);
        mem().write<std::uint64_t>(out + 16 * i + 8, evs[i].data.u64);
    }
    return r;
}

// ---- address parsing (no network access involved) ----
i32 h_inet_pton(i32 af, gaddr src, gaddr dst) { return ::inet_pton(af, gstr(src), mem().ptr(dst)); }
gaddr h_inet_ntop(i32 af, gaddr src, gaddr dst, u32 size) { return ::inet_ntop(af, mem().ptr(src), gstr(dst), size) ? dst : 0; }
u32 h_inet_addr(gaddr s) { return ::inet_addr(gstr(s)); }
i32 h_inet_aton(gaddr s, gaddr out) { return ::inet_aton(gstr(s), mem().ptr<in_addr>(out)); }
gaddr h_inet_ntoa(u32 a) {
    static const gaddr buf = mem().static_alloc(16);
    in_addr in{a};
    std::strncpy(gstr(buf), ::inet_ntoa(in), 15);
    return buf;
}
}  // namespace

void register_libm() {
#define SKIM_DEF(n) def(#n, wrap<m_##n>); def(#n "f", wrap<m_##n##f>);
    SKIM_DEF(acos) SKIM_DEF(asin) SKIM_DEF(atan) SKIM_DEF(cos) SKIM_DEF(sin) SKIM_DEF(tan) SKIM_DEF(cosh) SKIM_DEF(sinh)
    SKIM_DEF(tanh) SKIM_DEF(exp) SKIM_DEF(log) SKIM_DEF(log10) SKIM_DEF(sqrt) SKIM_DEF(floor) SKIM_DEF(ceil) SKIM_DEF(rint)
    SKIM_DEF(round) SKIM_DEF(trunc) SKIM_DEF(fabs) SKIM_DEF(atan2) SKIM_DEF(pow) SKIM_DEF(fmod) SKIM_DEF(hypot)
#undef SKIM_DEF
    def("ldexp", wrap<m_ldexp>); def("frexp", wrap<m_frexp>); def("modf", wrap<m_modf>);
    def("poll", wrap<h_poll>); def("select", h_select);
    def("epoll_create", wrap<h_epoll_create>); def("epoll_ctl", wrap<h_epoll_ctl>); def("epoll_wait", wrap<h_epoll_wait>);
    def("inet_pton", wrap<h_inet_pton>); def("inet_ntop", wrap<h_inet_ntop>); def("inet_addr", wrap<h_inet_addr>);
    def("inet_aton", wrap<h_inet_aton>); def("inet_ntoa", wrap<h_inet_ntoa>);
}

}  // namespace skim::hle
