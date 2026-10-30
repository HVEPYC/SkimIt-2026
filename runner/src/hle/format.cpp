#include "hle/format.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace skim::hle {

std::string format(const char* f, ArgSource& a) {
    std::string out;
    char spec[32], buf[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        const char* start = f++;
        if (*f == '%') { out += '%'; ++f; continue; }
        std::string flags;
        while (*f && std::strchr("-+ #0'", *f)) flags += *f++;
        int width = -1, prec = -1;
        if (*f == '*') { width = static_cast<int>(a.u32()); ++f; }
        else while (std::isdigit(static_cast<unsigned char>(*f))) width = (width < 0 ? 0 : width * 10) + (*f++ - '0');
        if (*f == '.') {
            ++f;
            prec = 0;
            if (*f == '*') { prec = static_cast<int>(a.u32()); ++f; }
            else while (std::isdigit(static_cast<unsigned char>(*f))) prec = prec * 10 + (*f++ - '0');
        }
        int longs = 0;  // 0 = int, 1 = h, 2 = hh, 8 = 64-bit
        for (;;) {
            if (*f == 'l') { longs = (f[1] == 'l') ? 8 : 0; f += (f[1] == 'l') ? 2 : 1; }
            else if (*f == 'h') { longs = (f[1] == 'h') ? 2 : 1; f += (f[1] == 'h') ? 2 : 1; }
            else if (*f == 'q' || *f == 'j' || *f == 'L') { longs = 8; ++f; }
            else if (*f == 'z' || *f == 't') ++f;
            else break;
        }
        const char conv = *f;
        if (!conv) { out.append(start); break; }
        ++f;
        std::string base = "%" + flags;
        if (width >= 0) base += std::to_string(width);
        if (prec >= 0) base += "." + std::to_string(prec);
        switch (conv) {
        case 'd': case 'i': {
            const long long v = longs == 8 ? static_cast<long long>(a.u64()) : longs == 1 ? static_cast<short>(a.u32())
                              : longs == 2 ? static_cast<signed char>(a.u32()) : static_cast<int>(a.u32());
            std::snprintf(spec, sizeof spec, "%slld", base.c_str());
            std::snprintf(buf, sizeof buf, spec, v);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = longs == 8 ? a.u64() : a.u32();
            if (longs == 1) v &= 0xFFFF; else if (longs == 2) v &= 0xFF;
            std::snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, v);
            out += buf;
            break;
        }
        case 'c':
            std::snprintf(spec, sizeof spec, "%sc", base.c_str());
            std::snprintf(buf, sizeof buf, spec, static_cast<int>(a.u32()));
            out += buf;
            break;
        case 's': {
            const gaddr p = a.u32();
            const char* s = p ? gstr(p) : "(null)";
            std::snprintf(spec, sizeof spec, "%ss", base.c_str());
            const int n = std::snprintf(nullptr, 0, spec, s);
            std::string tmp(static_cast<std::size_t>(n) + 1, '\0');
            std::snprintf(tmp.data(), tmp.size(), spec, s);
            tmp.resize(static_cast<std::size_t>(n));
            out += tmp;
            break;
        }
        case 'p':
            std::snprintf(buf, sizeof buf, "0x%x", a.u32());
            out += buf;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const std::uint64_t bits = a.u64();
            double d;
            std::memcpy(&d, &bits, 8);
            std::snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, d);
            out += buf;
            break;
        }
        case 'n': {
            const gaddr p = a.u32();
            if (p) mem().write<std::int32_t>(p, static_cast<std::int32_t>(out.size()));
            break;
        }
        default:
            out.append(start, f);
            break;
        }
    }
    return out;
}

int scan(const char* in, const char* f, ArgSource& a) {
    int assigned = 0;
    const char* const in_start = in;
    while (*f) {
        if (std::isspace(static_cast<unsigned char>(*f))) {
            while (std::isspace(static_cast<unsigned char>(*in))) ++in;
            ++f;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') ++f;
            if (*in != *f) break;
            ++in, ++f;
            continue;
        }
        ++f;
        const bool skip = *f == '*';
        if (skip) ++f;
        int width = 0;
        while (std::isdigit(static_cast<unsigned char>(*f))) width = width * 10 + (*f++ - '0');
        int size = 4;
        bool dbl = false;
        for (;;) {
            if (*f == 'l') { if (f[1] == 'l') { size = 8; f += 2; } else { dbl = true; ++f; } }
            else if (*f == 'h') { if (f[1] == 'h') { size = 1; f += 2; } else { size = 2; ++f; } }
            else if (*f == 'q' || *f == 'j' || *f == 'L') { size = 8; dbl = true; ++f; }
            else if (*f == 'z' || *f == 't') ++f;
            else break;
        }
        const char conv = *f++;
        if (conv != 'c' && conv != '[' && conv != 'n')
            while (std::isspace(static_cast<unsigned char>(*in))) ++in;
        if (!*in && conv != 'n') return assigned ? assigned : -1;
        const std::string field(in, width ? ::strnlen(in, static_cast<std::size_t>(width)) : std::strlen(in));
        auto store_int = [&](gaddr p, unsigned long long v) {
            if (size == 8) mem().write<std::uint64_t>(p, v);
            else if (size == 2) mem().write<std::uint16_t>(p, static_cast<std::uint16_t>(v));
            else if (size == 1) mem().write<std::uint8_t>(p, static_cast<std::uint8_t>(v));
            else mem().write<std::uint32_t>(p, static_cast<std::uint32_t>(v));
        };
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            const int b = conv == 'i' ? 0 : (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : 10;
            char* end;
            const unsigned long long v = (conv == 'd' || conv == 'i') ? static_cast<unsigned long long>(std::strtoll(field.c_str(), &end, b))
                                                                       : std::strtoull(field.c_str(), &end, b);
            if (end == field.c_str()) return assigned;
            in += end - field.c_str();
            if (!skip) { store_int(a.u32(), v); ++assigned; }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            char* end;
            const double v = std::strtod(field.c_str(), &end);
            if (end == field.c_str()) return assigned;
            in += end - field.c_str();
            if (!skip) {
                const gaddr p = a.u32();
                if (dbl) mem().write<double>(p, v); else mem().write<float>(p, static_cast<float>(v));
                ++assigned;
            }
            break;
        }
        case 's': {
            std::size_t n = 0;
            while (n < field.size() && !std::isspace(static_cast<unsigned char>(field[n]))) ++n;
            if (!skip) { const gaddr p = a.u32(); std::memcpy(gstr(p), in, n); gstr(p)[n] = 0; ++assigned; }
            in += n;
            break;
        }
        case 'c': {
            const std::size_t n = width ? static_cast<std::size_t>(width) : 1;
            if (std::strlen(in) < n) return assigned;
            if (!skip) { std::memcpy(gstr(a.u32()), in, n); ++assigned; }
            in += n;
            break;
        }
        case '[': {
            bool negate = *f == '^';
            if (negate) ++f;
            std::string set;
            if (*f == ']') set += *f++;
            while (*f && *f != ']') set += *f++;
            if (*f == ']') ++f;
            std::size_t n = 0;
            while (n < field.size() && ((set.find(field[n]) != std::string::npos) != negate)) ++n;
            if (!n) return assigned;
            if (!skip) { const gaddr p = a.u32(); std::memcpy(gstr(p), in, n); gstr(p)[n] = 0; ++assigned; }
            in += n;
            break;
        }
        case 'n':
            if (!skip) store_int(a.u32(), static_cast<unsigned long long>(in - in_start));
            break;
        default:
            return assigned;
        }
    }
    return assigned;
}

}  // namespace skim::hle
