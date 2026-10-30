// printf/scanf engines over guest arguments (register/stack arguments or an ARM va_list).
#pragma once

#include <string>

#include "hle/registry.h"

namespace skim::hle {

class ArgSource {
public:
    virtual ~ArgSource() = default;
    virtual std::uint32_t u32() = 0;
    virtual std::uint64_t u64() = 0;
};

class CtxArgs final : public ArgSource {
public:
    explicit CtxArgs(Ctx& c) : c_(c) {}
    std::uint32_t u32() override { return c_.u32(); }
    std::uint64_t u64() override { return c_.u64(); }
private:
    Ctx& c_;
};

// AAPCS va_list: a pointer to the next argument on the guest stack.
class VaListArgs final : public ArgSource {
public:
    explicit VaListArgs(gaddr ap) : ap_(ap) {}
    std::uint32_t u32() override { const auto v = mem().read<std::uint32_t>(ap_); ap_ += 4; return v; }
    std::uint64_t u64() override { ap_ = (ap_ + 7) & ~7u; const auto v = mem().read<std::uint64_t>(ap_); ap_ += 8; return v; }
private:
    gaddr ap_;
};

std::string format(const char* fmt, ArgSource& args);
// Returns the number of assigned conversions, or -1 on input failure before the first one.
int scan(const char* input, const char* fmt, ArgSource& args);

}  // namespace skim::hle
