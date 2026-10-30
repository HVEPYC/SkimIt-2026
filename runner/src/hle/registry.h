// HLE function table. Every import a guest library binds to (implemented or not) gets an index
// and a thunk `svc #(kSvcImport + index)`. Unimplemented imports stop the run on first call
// (or return 0 with SKIM_STUB_UNIMPLEMENTED=1, to discover the next ones faster).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "guest/cpu.h"
#include "guest/memory.h"

namespace skim {

// Reads AAPCS (softfp) arguments of the current import call and writes its result.
class Ctx {
public:
    explicit Ctx(GuestThread& t) : t(t), R(t.regs()) {}

    std::uint32_t u32() {
        if (ncrn_ < 4) return R[ncrn_++];
        const std::uint32_t v = Memory::get().read<std::uint32_t>(R[13] + nsaa_);
        nsaa_ += 4;
        return v;
    }
    std::uint64_t u64() {
        if (ncrn_ < 4) {
            ncrn_ = (ncrn_ + 1) & ~1u;
            if (ncrn_ < 4) {
                const std::uint64_t v = R[ncrn_] | static_cast<std::uint64_t>(R[ncrn_ + 1]) << 32;
                ncrn_ += 2;
                return v;
            }
        }
        nsaa_ = (nsaa_ + 7) & ~7u;
        const std::uint64_t v = Memory::get().read<std::uint64_t>(R[13] + nsaa_);
        nsaa_ += 8;
        return v;
    }
    float f32() { const std::uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64() { const std::uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }

    void ret(std::uint32_t v) { R[0] = v; }
    void ret64(std::uint64_t v) { R[0] = static_cast<std::uint32_t>(v); R[1] = static_cast<std::uint32_t>(v >> 32); }

    GuestThread& t;
    bool no_return = false;  // the HLE function set the PC itself (e.g. siglongjmp)

private:
    std::array<std::uint32_t, 16>& R;
    std::uint32_t ncrn_ = 0, nsaa_ = 0;
};

using HleFn = void (*)(Ctx&);

namespace hle {
// Registration (called by each module's register_*()).
void def(const char* name, HleFn fn);
void def_data(const char* name, gaddr addr);
// Fallback for every import starting with `prefix` that has no specific function (e.g. "gl").
void def_prefix(const char* prefix, HleFn fn);

// Binding (called by the loader).
gaddr function_thunk(const std::string& name);  // thunk address (implemented or a lazy stub)
bool has_function(const std::string& name);
gaddr data(const std::string& name);            // 0 if none

void dispatch(GuestThread& t, std::uint32_t index);
// Debugging: imports named in SKIM_TRACE_IMPORTS (comma-separated) are bound to a thunk that logs
// each call (thread, caller, r0-r3, result) and forwards to the real target.
bool traced(const std::string& name);
gaddr trace_thunk(const std::string& name, gaddr real);
std::string describe(gaddr pc);  // "libmono.so+0x1234" or "thunk:malloc"

// Sets up thunks and kuser helpers and registers every module. Call once before loading.
void init();
std::size_t implemented_count();
std::vector<std::string> function_names();  // implemented functions and data objects

// Guest signal dispositions (bionic arm struct sigaction: handler, mask, flags, restorer).
struct SigAction { std::uint32_t handler = 0, mask = 0, flags = 0, restorer = 0; };
SigAction sigaction_for(int sig);
}  // namespace hle

// ---- typed wrappers: hle::def("strlen", wrap<&my_strlen>) ----
namespace detail {
template<typename T>
T arg(Ctx& c) {
    if constexpr (std::is_same_v<T, float>) return c.f32();
    else if constexpr (std::is_same_v<T, double>) return c.f64();
    else if constexpr (sizeof(T) == 8) return static_cast<T>(c.u64());
    else return static_cast<T>(c.u32());
}

template<typename R>
void set_ret(Ctx& c, R v) {
    if constexpr (std::is_same_v<R, float>) { std::uint32_t u; std::memcpy(&u, &v, 4); c.ret(u); }
    else if constexpr (std::is_same_v<R, double>) { std::uint64_t u; std::memcpy(&u, &v, 8); c.ret64(u); }
    else if constexpr (sizeof(R) == 8) c.ret64(static_cast<std::uint64_t>(v));
    else c.ret(static_cast<std::uint32_t>(v));
}

template<auto F>
struct Wrap;
template<typename R, typename... A, R (*F)(A...)>
struct Wrap<F> {
    static void call(Ctx& c) {
        std::tuple<A...> args{arg<A>(c)...};  // braced init: evaluated left to right
        if constexpr (std::is_void_v<R>) std::apply(F, args);
        else set_ret(c, std::apply(F, args));
    }
};
}  // namespace detail

template<auto F>
constexpr HleFn wrap = &detail::Wrap<F>::call;

// Convenience for HLE code.
inline Memory& mem() { return Memory::get(); }
inline char* gstr(gaddr a) { return a ? mem().ptr<char>(a) : nullptr; }
inline void set_errno(int e) { GuestThread::current()->set_errno(e); }

}  // namespace skim
