// libc: memory, strings, ctype, number conversion, sorting, environment, exit.
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <strings.h>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;

void* P(gaddr a) { return mem().ptr(a); }
char* S(gaddr a) { return gstr(a); }
gaddr G(const void* p) { return p ? mem().addr(p) : 0; }

// ---- malloc family ----
gaddr h_malloc(u32 n) { return heap::malloc(n); }
gaddr h_calloc(u32 n, u32 s) { return heap::calloc(n, s); }
gaddr h_realloc(gaddr p, u32 n) { return heap::realloc(p, n); }
void h_free(gaddr p) { heap::free(p); }
gaddr h_memalign(u32 a, u32 n) { return heap::memalign(a, n); }
i32 h_posix_memalign(gaddr out, u32 a, u32 n) {
    const gaddr p = heap::memalign(a, n);
    if (!p) return ENOMEM;
    mem().write<gaddr>(out, p);
    return 0;
}
u32 h_malloc_usable_size(gaddr p) { return heap::usable_size(p); }
void h_operator_delete(gaddr p) { heap::free(p); }

// ---- memory / strings ----
gaddr h_memcpy(gaddr d, gaddr s, u32 n) { std::memmove(P(d), P(s), n); return d; }
gaddr h_memmove(gaddr d, gaddr s, u32 n) { std::memmove(P(d), P(s), n); return d; }
gaddr h_memset(gaddr d, i32 c, u32 n) { std::memset(P(d), c, n); return d; }
i32 h_memcmp(gaddr a, gaddr b, u32 n) { return std::memcmp(P(a), P(b), n); }
gaddr h_memchr(gaddr s, i32 c, u32 n) { return G(std::memchr(P(s), c, n)); }
gaddr h_memrchr(gaddr s, i32 c, u32 n) { return G(::memrchr(P(s), c, n)); }
// __aeabi_memcpy etc. take (dest, src, n) / (dest, n, c): the order differs from memset.
void h_aeabi_memcpy(gaddr d, gaddr s, u32 n) { std::memmove(P(d), P(s), n); }
void h_aeabi_memset(gaddr d, u32 n, i32 c) { std::memset(P(d), c, n); }
void h_aeabi_memclr(gaddr d, u32 n) { std::memset(P(d), 0, n); }

u32 h_strlen(gaddr s) { return static_cast<u32>(std::strlen(S(s))); }
u32 h_strnlen(gaddr s, u32 n) { return static_cast<u32>(::strnlen(S(s), n)); }
i32 h_strcmp(gaddr a, gaddr b) { return std::strcmp(S(a), S(b)); }
i32 h_strncmp(gaddr a, gaddr b, u32 n) { return std::strncmp(S(a), S(b), n); }
i32 h_strcasecmp(gaddr a, gaddr b) { return ::strcasecmp(S(a), S(b)); }
i32 h_strncasecmp(gaddr a, gaddr b, u32 n) { return ::strncasecmp(S(a), S(b), n); }
i32 h_strcoll(gaddr a, gaddr b) { return std::strcmp(S(a), S(b)); }
gaddr h_strcpy(gaddr d, gaddr s) { std::memmove(S(d), S(s), std::strlen(S(s)) + 1); return d; }
gaddr h_strncpy(gaddr d, gaddr s, u32 n) { std::strncpy(S(d), S(s), n); return d; }
gaddr h_strcat(gaddr d, gaddr s) { std::strcat(S(d), S(s)); return d; }
gaddr h_strncat(gaddr d, gaddr s, u32 n) { std::strncat(S(d), S(s), n); return d; }
u32 h_strlcpy(gaddr d, gaddr s, u32 n) {
    const std::size_t len = std::strlen(S(s));
    if (n) { const std::size_t c = len < n - 1 ? len : n - 1; std::memmove(S(d), S(s), c); S(d)[c] = 0; }
    return static_cast<u32>(len);
}
u32 h_strlcat(gaddr d, gaddr s, u32 n) {
    const std::size_t dl = ::strnlen(S(d), n);
    return dl == n ? static_cast<u32>(n + std::strlen(S(s))) : dl + h_strlcpy(d + static_cast<u32>(dl), s, n - static_cast<u32>(dl));
}
gaddr h_strchr(gaddr s, i32 c) { return G(std::strchr(S(s), c)); }
gaddr h_strrchr(gaddr s, i32 c) { return G(std::strrchr(S(s), c)); }
gaddr h_strstr(gaddr a, gaddr b) { return G(std::strstr(S(a), S(b))); }
gaddr h_strpbrk(gaddr a, gaddr b) { return G(std::strpbrk(S(a), S(b))); }
u32 h_strspn(gaddr a, gaddr b) { return static_cast<u32>(std::strspn(S(a), S(b))); }
u32 h_strcspn(gaddr a, gaddr b) { return static_cast<u32>(std::strcspn(S(a), S(b))); }
gaddr h_strdup(gaddr s) { return heap::strdup(S(s)); }
gaddr h_strndup(gaddr s, u32 n) { return heap::strdup(std::string(S(s), ::strnlen(S(s), n))); }
gaddr h_strtok_r(gaddr s, gaddr delim, gaddr save) {
    char* saved = s ? nullptr : S(mem().read<gaddr>(save));
    char* r = ::strtok_r(s ? S(s) : nullptr, S(delim), &saved);
    mem().write<gaddr>(save, G(saved));
    return G(r);
}
gaddr h_strtok(gaddr s, gaddr delim) {
    static gaddr save = mem().static_alloc(4);
    return h_strtok_r(s, delim, save);
}
gaddr h_strerror(i32 e) {
    static std::map<i32, gaddr> cache;
    static std::mutex lock;
    std::lock_guard g{lock};
    auto& slot = cache[e];
    if (!slot) slot = mem().static_str(std::strerror(e));
    return slot;
}

// ---- ctype (bionic: functions, "C" locale) ----
i32 h_isalnum(i32 c) { return std::isalnum(c & 0xFF) != 0 && c >= 0 && c < 128; }
i32 h_isalpha(i32 c) { return c >= 0 && c < 128 && std::isalpha(c); }
i32 h_isdigit(i32 c) { return c >= '0' && c <= '9'; }
i32 h_isspace(i32 c) { return c >= 0 && c < 128 && std::isspace(c); }
i32 h_isxdigit(i32 c) { return c >= 0 && c < 128 && std::isxdigit(c); }
i32 h_isupper(i32 c) { return c >= 'A' && c <= 'Z'; }
i32 h_islower(i32 c) { return c >= 'a' && c <= 'z'; }
i32 h_isprint(i32 c) { return c >= 0x20 && c < 0x7F; }
i32 h_ispunct(i32 c) { return c >= 0 && c < 128 && std::ispunct(c); }
i32 h_iscntrl(i32 c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
i32 h_tolower(i32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
i32 h_toupper(i32 c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// ---- number conversion ----
template<typename F>
auto with_end(gaddr s, gaddr endp, F f) {
    char* end = nullptr;
    errno = 0;
    auto v = f(S(s), &end);
    if (endp) mem().write<gaddr>(endp, G(end));
    if (errno) set_errno(errno);
    return v;
}
i32 h_strtol(gaddr s, gaddr e, i32 b) {
    return with_end(s, e, [b](const char* p, char** end) {
        const long v = std::strtol(p, end, b);
        if (v > INT32_MAX) { errno = ERANGE; return INT32_MAX; }
        if (v < INT32_MIN) { errno = ERANGE; return INT32_MIN; }
        return static_cast<i32>(v);
    });
}
u32 h_strtoul(gaddr s, gaddr e, i32 b) {
    return with_end(s, e, [b](const char* p, char** end) {
        const unsigned long v = std::strtoul(p, end, b);
        const long sv = static_cast<long>(v);
        if (sv > static_cast<long>(UINT32_MAX) || sv < -static_cast<long>(UINT32_MAX)) { errno = ERANGE; return UINT32_MAX; }
        return static_cast<u32>(v);
    });
}
std::int64_t h_strtoll(gaddr s, gaddr e, i32 b) { return with_end(s, e, [b](const char* p, char** end) { return std::strtoll(p, end, b); }); }
std::uint64_t h_strtoull(gaddr s, gaddr e, i32 b) { return with_end(s, e, [b](const char* p, char** end) { return std::strtoull(p, end, b); }); }
double h_strtod(gaddr s, gaddr e) { return with_end(s, e, [](const char* p, char** end) { return std::strtod(p, end); }); }
float h_strtof(gaddr s, gaddr e) { return with_end(s, e, [](const char* p, char** end) { return std::strtof(p, end); }); }
i32 h_atoi(gaddr s) { return std::atoi(S(s)); }
i32 h_atol(gaddr s) { return static_cast<i32>(std::atol(S(s))); }
std::int64_t h_atoll(gaddr s) { return std::atoll(S(s)); }
double h_atof(gaddr s) { return std::atof(S(s)); }
i32 h_abs(i32 v) { return v < 0 ? -v : v; }
i32 h_labs(i32 v) { return v < 0 ? -v : v; }
// div_t div(int, int): returned in r0 (quot) and r1 (rem); bionic's div_t is 8 bytes, returned in memory.
void h_div(Ctx& c) {
    const gaddr out = c.u32();
    const i32 n = static_cast<i32>(c.u32()), d = static_cast<i32>(c.u32());
    mem().write<i32>(out, n / d);
    mem().write<i32>(out + 4, n % d);
}

// ---- sorting (calls back into the guest comparator) ----
void h_qsort(gaddr base, u32 n, u32 size, gaddr cmp) {
    GuestThread& t = *GuestThread::current();
    auto less = [&](u32 i, u32 j) { return static_cast<i32>(t.call(cmp, {base + i * size, base + j * size})); };
    std::vector<std::uint8_t> tmp(size);
    // Insertion sort for small arrays, heap sort otherwise: both in place, comparator-only.
    auto swap = [&](u32 i, u32 j) {
        std::memcpy(tmp.data(), P(base + i * size), size);
        std::memmove(P(base + i * size), P(base + j * size), size);
        std::memcpy(P(base + j * size), tmp.data(), size);
    };
    if (n < 16) {
        for (u32 i = 1; i < n; ++i)
            for (u32 j = i; j > 0 && less(j - 1, j) > 0; --j) swap(j - 1, j);
        return;
    }
    auto sift = [&](u32 start, u32 end) {
        u32 root = start;
        while (2 * root + 1 < end) {
            u32 child = 2 * root + 1;
            if (child + 1 < end && less(child, child + 1) < 0) ++child;
            if (less(root, child) >= 0) return;
            swap(root, child);
            root = child;
        }
    };
    for (u32 i = n / 2; i-- > 0;) sift(i, n);
    for (u32 end = n - 1; end > 0; --end) { swap(0, end); sift(0, end); }
}
gaddr h_bsearch(gaddr key, gaddr base, u32 n, u32 size, gaddr cmp) {
    GuestThread& t = *GuestThread::current();
    u32 lo = 0, hi = n;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        const i32 r = static_cast<i32>(t.call(cmp, {key, base + mid * size}));
        if (r == 0) return base + mid * size;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

// ---- environment ----
struct Env {
    std::mutex lock;
    std::map<std::string, gaddr> vars;  // name -> "name=value" in the guest heap
    gaddr environ_var = 0;              // char** environ (data import)
    void publish() {
        const gaddr arr = heap::calloc(static_cast<u32>(vars.size() + 1), 4);
        u32 i = 0;
        for (auto& [k, v] : vars) mem().write<gaddr>(arr + 4 * i++, v);
        mem().write<gaddr>(environ_var, arr);
    }
};
Env& env() {
    static Env e;
    return e;
}
gaddr h_getenv(gaddr name) {
    Env& e = env();
    std::lock_guard g{e.lock};
    auto it = e.vars.find(S(name));
    return it == e.vars.end() ? 0 : it->second + static_cast<u32>(it->first.size() + 1);
}
i32 h_setenv(gaddr name, gaddr value, i32 overwrite) {
    Env& e = env();
    std::lock_guard g{e.lock};
    if (!overwrite && e.vars.count(S(name))) return 0;
    e.vars[S(name)] = heap::strdup(std::string(S(name)) + "=" + S(value));
    e.publish();
    return 0;
}
i32 h_unsetenv(gaddr name) {
    Env& e = env();
    std::lock_guard g{e.lock};
    e.vars.erase(S(name));
    e.publish();
    return 0;
}

// ---- exit ----
struct AtExit { gaddr fn, arg; bool with_arg; };
std::vector<AtExit>& atexit_list() {
    static std::vector<AtExit> v;
    return v;
}
i32 h_atexit(gaddr fn) { atexit_list().push_back({fn, 0, false}); return 0; }
i32 h_cxa_atexit(gaddr fn, gaddr arg, gaddr) { atexit_list().push_back({fn, arg, true}); return 0; }
void h_exit(i32 code) {
    GuestThread& t = *GuestThread::current();
    auto& l = atexit_list();
    while (!l.empty()) {
        const AtExit e = l.back();
        l.pop_back();
        e.with_arg ? t.call(e.fn, {e.arg}) : t.call(e.fn, {});
    }
    std::fflush(nullptr);
    std::fprintf(stderr, "[hle] guest exit(%d)\n", code);
    std::_Exit(code);
}
void h__exit(i32 code) { std::fprintf(stderr, "[hle] guest _exit(%d)\n", code); std::_Exit(code); }
void h_abort() { GuestThread::current()->fatal("guest called abort()"); }
void h_stack_chk_fail() { GuestThread::current()->fatal("guest stack protector failure"); }
void h_cxa_pure_virtual() { GuestThread::current()->fatal("guest pure virtual call"); }

// ---- 64-bit / division helpers that may be imported ----
std::int64_t h_aeabi_ldivmod_unused(std::int64_t a, std::int64_t b) { return b ? a / b : 0; }

// ---- more strings, wide characters (bionic wchar_t is 32-bit), rand48 ----
gaddr h_memmem(gaddr h, u32 hl, gaddr n, u32 nl) { return G(::memmem(P(h), hl, P(n), nl)); }
gaddr h_strcasestr(gaddr h, gaddr n) { return G(::strcasestr(S(h), S(n))); }
std::uint32_t* W(gaddr a) { return mem().ptr<std::uint32_t>(a); }
u32 h_wcslen(gaddr s) { u32 n = 0; while (W(s)[n]) ++n; return n; }
i32 h_wcscmp(gaddr a, gaddr b) {
    for (u32 i = 0;; ++i) {
        const u32 x = W(a)[i], y = W(b)[i];
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
gaddr h_wcsncpy(gaddr d, gaddr s, u32 n) {
    u32 i = 0;
    for (; i < n && W(s)[i]; ++i) W(d)[i] = W(s)[i];
    for (; i < n; ++i) W(d)[i] = 0;
    return d;
}
gaddr h_wmemcpy(gaddr d, gaddr s, u32 n) { std::memmove(P(d), P(s), n * 4ull); return d; }
gaddr h_wmemset(gaddr d, u32 c, u32 n) { for (u32 i = 0; i < n; ++i) W(d)[i] = c; return d; }
// Wide ctype: ASCII semantics, as bionic of that era had no real wide-character locale support.
#define SKIM_ISW(name, test) i32 h_##name(u32 c) { return c < 128 && (test); }
SKIM_ISW(iswalpha, std::isalpha(static_cast<int>(c))) SKIM_ISW(iswcntrl, std::iscntrl(static_cast<int>(c)))
SKIM_ISW(iswdigit, c >= '0' && c <= '9') SKIM_ISW(iswlower, c >= 'a' && c <= 'z') SKIM_ISW(iswupper, c >= 'A' && c <= 'Z')
SKIM_ISW(iswprint, c >= 0x20 && c < 0x7F) SKIM_ISW(iswpunct, std::ispunct(static_cast<int>(c)))
SKIM_ISW(iswspace, std::isspace(static_cast<int>(c))) SKIM_ISW(iswxdigit, std::isxdigit(static_cast<int>(c)))
#undef SKIM_ISW
u32 h_towlower(u32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
u32 h_towupper(u32 c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
std::mutex rand48_lock;
std::uint64_t rand48_state = 0x1234ABCD330Eull;
i32 h_lrand48() {
    std::lock_guard g{rand48_lock};
    rand48_state = (rand48_state * 0x5DEECE66Dull + 0xB) & ((1ull << 48) - 1);
    return static_cast<i32>(rand48_state >> 17);
}
void h_srand48(i32 seed) { std::lock_guard g{rand48_lock}; rand48_state = (static_cast<std::uint64_t>(static_cast<u32>(seed)) << 16) | 0x330E; }

// ---- C++ ABI: function-local static guards (ARM EABI: bit 0 of the 32-bit guard word) ----
std::recursive_mutex guard_lock;
i32 h_cxa_guard_acquire(gaddr g) {
    if (mem().read<u32>(g) & 1) return 0;
    guard_lock.lock();
    if (mem().read<u32>(g) & 1) { guard_lock.unlock(); return 0; }
    return 1;  // the caller runs the initializer, then __cxa_guard_release
}
void h_cxa_guard_release(gaddr g) { mem().write<u32>(g, 1); guard_lock.unlock(); }
void h_cxa_guard_abort(gaddr) { guard_lock.unlock(); }
void h_cxa_finalize(gaddr) {}
i32 h_fpclassifyf(float f) {  // bionic's FP_* values
    switch (std::fpclassify(f)) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;  // FP_ZERO
    }
}

// bionic's ctype tables (data imports): _ctype_[1 + 256] flags, _tolower_tab_/_toupper_tab_[1 + 256] shorts.
void make_ctype_tables() {
    const gaddr ct = mem().static_alloc(1 + 256), lo = mem().static_alloc(2 * 257), up = mem().static_alloc(2 * 257);
    mem().write<std::int16_t>(lo, -1);
    mem().write<std::int16_t>(up, -1);
    for (int c = 0; c < 256; ++c) {
        std::uint8_t f = 0;
        if (c < 128) {
            if (std::isupper(c)) f |= 0x01;
            if (std::islower(c)) f |= 0x02;
            if (std::isdigit(c)) f |= 0x04;
            if (std::isspace(c)) f |= 0x08;
            if (std::ispunct(c)) f |= 0x10;
            if (std::iscntrl(c)) f |= 0x20;
            if (std::isxdigit(c) && !std::isdigit(c)) f |= 0x40;
            if (c == ' ') f |= 0x80;
        }
        mem().write<std::uint8_t>(ct + 1 + c, f);
        mem().write<std::int16_t>(lo + 2 + 2 * c, static_cast<std::int16_t>(h_tolower(c)));
        mem().write<std::int16_t>(up + 2 + 2 * c, static_cast<std::int16_t>(h_toupper(c)));
    }
    def_data("_ctype_", ct);
    def_data("_tolower_tab_", lo);
    def_data("_toupper_tab_", up);
}
}  // namespace

void register_libc_mem() {
    def("malloc", wrap<h_malloc>); def("calloc", wrap<h_calloc>); def("realloc", wrap<h_realloc>);
    def("free", wrap<h_free>); def("memalign", wrap<h_memalign>); def("posix_memalign", wrap<h_posix_memalign>);
    def("malloc_usable_size", wrap<h_malloc_usable_size>); def("dlmalloc_usable_size", wrap<h_malloc_usable_size>);
    def("_Znwj", wrap<h_malloc>); def("_Znaj", wrap<h_malloc>);
    def("_ZdlPv", wrap<h_operator_delete>); def("_ZdaPv", wrap<h_operator_delete>);
    def("memcpy", wrap<h_memcpy>); def("memmove", wrap<h_memmove>); def("memset", wrap<h_memset>);
    def("memcmp", wrap<h_memcmp>); def("bcmp", wrap<h_memcmp>); def("memchr", wrap<h_memchr>); def("memrchr", wrap<h_memrchr>);
    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8", "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"})
        def(n, wrap<h_aeabi_memcpy>);
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) def(n, wrap<h_aeabi_memset>);
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) def(n, wrap<h_aeabi_memclr>);
    def("strlen", wrap<h_strlen>); def("strnlen", wrap<h_strnlen>); def("strcmp", wrap<h_strcmp>); def("strncmp", wrap<h_strncmp>);
    def("strcasecmp", wrap<h_strcasecmp>); def("strncasecmp", wrap<h_strncasecmp>); def("strcoll", wrap<h_strcoll>);
    def("strcpy", wrap<h_strcpy>); def("strncpy", wrap<h_strncpy>); def("strcat", wrap<h_strcat>); def("strncat", wrap<h_strncat>);
    def("strlcpy", wrap<h_strlcpy>); def("strlcat", wrap<h_strlcat>); def("strchr", wrap<h_strchr>); def("strrchr", wrap<h_strrchr>);
    def("strstr", wrap<h_strstr>); def("strpbrk", wrap<h_strpbrk>); def("strspn", wrap<h_strspn>); def("strcspn", wrap<h_strcspn>);
    def("strdup", wrap<h_strdup>); def("strndup", wrap<h_strndup>); def("strtok", wrap<h_strtok>); def("strtok_r", wrap<h_strtok_r>);
    def("strerror", wrap<h_strerror>);
    def("isalnum", wrap<h_isalnum>); def("isalpha", wrap<h_isalpha>); def("isdigit", wrap<h_isdigit>); def("isspace", wrap<h_isspace>);
    def("isxdigit", wrap<h_isxdigit>); def("isupper", wrap<h_isupper>); def("islower", wrap<h_islower>); def("isprint", wrap<h_isprint>);
    def("ispunct", wrap<h_ispunct>); def("iscntrl", wrap<h_iscntrl>); def("tolower", wrap<h_tolower>); def("toupper", wrap<h_toupper>);
    def("strtol", wrap<h_strtol>); def("strtoul", wrap<h_strtoul>); def("strtoll", wrap<h_strtoll>); def("strtoull", wrap<h_strtoull>);
    def("strtod", wrap<h_strtod>); def("strtof", wrap<h_strtof>); def("atoi", wrap<h_atoi>); def("atol", wrap<h_atol>);
    def("atoll", wrap<h_atoll>); def("atof", wrap<h_atof>); def("abs", wrap<h_abs>); def("labs", wrap<h_labs>);
    def("div", h_div);
    def("qsort", wrap<h_qsort>); def("bsearch", wrap<h_bsearch>);
    def("getenv", wrap<h_getenv>); def("setenv", wrap<h_setenv>); def("unsetenv", wrap<h_unsetenv>);
    def("atexit", wrap<h_atexit>); def("__cxa_atexit", wrap<h_cxa_atexit>); def("__aeabi_atexit", wrap<h_cxa_atexit>);
    def("exit", wrap<h_exit>); def("_exit", wrap<h__exit>); def("abort", wrap<h_abort>);
    def("__stack_chk_fail", wrap<h_stack_chk_fail>); def("__cxa_pure_virtual", wrap<h_cxa_pure_virtual>);
    (void)h_aeabi_ldivmod_unused;
    def("memmem", wrap<h_memmem>); def("strcasestr", wrap<h_strcasestr>);
    def("wcslen", wrap<h_wcslen>); def("wcscmp", wrap<h_wcscmp>); def("wcsncpy", wrap<h_wcsncpy>);
    def("wmemcpy", wrap<h_wmemcpy>); def("wmemmove", wrap<h_wmemcpy>); def("wmemset", wrap<h_wmemset>);
    def("iswalpha", wrap<h_iswalpha>); def("iswcntrl", wrap<h_iswcntrl>); def("iswdigit", wrap<h_iswdigit>);
    def("iswlower", wrap<h_iswlower>); def("iswupper", wrap<h_iswupper>); def("iswprint", wrap<h_iswprint>);
    def("iswpunct", wrap<h_iswpunct>); def("iswspace", wrap<h_iswspace>); def("iswxdigit", wrap<h_iswxdigit>);
    def("towlower", wrap<h_towlower>); def("towupper", wrap<h_towupper>);
    def("lrand48", wrap<h_lrand48>); def("srand48", wrap<h_srand48>);
    def("__cxa_guard_acquire", wrap<h_cxa_guard_acquire>); def("__cxa_guard_release", wrap<h_cxa_guard_release>);
    def("__cxa_guard_abort", wrap<h_cxa_guard_abort>); def("__cxa_finalize", wrap<h_cxa_finalize>);
    def("__fpclassifyf", wrap<h_fpclassifyf>);
    make_ctype_tables();

    Env& e = env();
    e.environ_var = mem().static_alloc(4);
    def_data("environ", e.environ_var);
    const gaddr guard = mem().static_alloc(4);
    mem().write<u32>(guard, 0x5EC0'1D1Eu);
    def_data("__stack_chk_guard", guard);
    // Mono: no SIGSEGV-based null checks (the runner does not deliver faults as signals yet).
    e.vars["MONO_DEBUG"] = heap::strdup("MONO_DEBUG=explicit-null-checks");
    e.publish();
}

}  // namespace skim::hle
