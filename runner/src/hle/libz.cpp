// libz (host zlib behind the guest's 32-bit z_stream) and bionic's SHA1 functions.
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;

// Guest z_stream (56 bytes): next_in, avail_in, total_in, next_out, avail_out, total_out, msg,
// state, zalloc, zfree, opaque, data_type, adler, reserved. The host stream lives in a map keyed
// by the guest struct's address; each call copies the buffers' positions in and out.
enum : u32 { kNextIn = 0, kAvailIn = 4, kTotalIn = 8, kNextOut = 12, kAvailOut = 16, kTotalOut = 20, kMsg = 24, kState = 28, kDataType = 44, kAdler = 48 };

std::mutex lock;
std::unordered_map<gaddr, z_stream*> streams;

z_stream* host_stream(gaddr g, bool create) {
    std::lock_guard l{lock};
    auto it = streams.find(g);
    if (it != streams.end()) return it->second;
    if (!create) return nullptr;
    auto* z = new z_stream{};
    streams[g] = z;
    return z;
}
void drop_stream(gaddr g) {
    std::lock_guard l{lock};
    if (auto it = streams.find(g); it != streams.end()) { delete it->second; streams.erase(it); }
}

void to_host(gaddr g, z_stream* z) {
    Memory& m = mem();
    const gaddr in = m.read<u32>(g + kNextIn), out = m.read<u32>(g + kNextOut);
    z->next_in = in ? m.ptr<Bytef>(in) : Z_NULL;
    z->avail_in = m.read<u32>(g + kAvailIn);
    z->total_in = m.read<u32>(g + kTotalIn);
    z->next_out = out ? m.ptr<Bytef>(out) : Z_NULL;
    z->avail_out = m.read<u32>(g + kAvailOut);
    z->total_out = m.read<u32>(g + kTotalOut);
}
void to_guest(gaddr g, const z_stream* z) {
    Memory& m = mem();
    m.write<u32>(g + kNextIn, z->next_in ? m.addr(z->next_in) : 0);
    m.write<u32>(g + kAvailIn, z->avail_in);
    m.write<u32>(g + kTotalIn, static_cast<u32>(z->total_in));
    m.write<u32>(g + kNextOut, z->next_out ? m.addr(z->next_out) : 0);
    m.write<u32>(g + kAvailOut, z->avail_out);
    m.write<u32>(g + kTotalOut, static_cast<u32>(z->total_out));
    m.write<u32>(g + kDataType, static_cast<u32>(z->data_type));
    m.write<u32>(g + kAdler, static_cast<u32>(z->adler));
    static std::mutex msg_lock;
    static std::unordered_map<std::string, gaddr> msgs;
    gaddr msg = 0;
    if (z->msg) {
        std::lock_guard l{msg_lock};
        gaddr& slot = msgs[z->msg];
        if (!slot) slot = m.static_str(z->msg);
        msg = slot;
    }
    m.write<u32>(g + kMsg, msg);
}

template<typename F>
i32 with_stream(gaddr g, bool create, F f) {
    z_stream* z = host_stream(g, create);
    if (!z) return Z_STREAM_ERROR;
    to_host(g, z);
    const int r = f(z);
    to_guest(g, z);
    return r;
}

i32 h_deflateInit2_(gaddr g, i32 level, i32 method, i32 bits, i32 memlevel, i32 strategy, gaddr, i32) {
    drop_stream(g);
    return with_stream(g, true, [&](z_stream* z) { return deflateInit2(z, level, method, bits, memlevel, strategy); });
}
i32 h_deflateInit_(gaddr g, i32 level, gaddr, i32) {
    drop_stream(g);
    return with_stream(g, true, [&](z_stream* z) { return deflateInit(z, level); });
}
i32 h_deflate(gaddr g, i32 flush) { return with_stream(g, false, [&](z_stream* z) { return deflate(z, flush); }); }
i32 h_deflateReset(gaddr g) { return with_stream(g, false, [](z_stream* z) { return deflateReset(z); }); }
i32 h_deflateEnd(gaddr g) {
    const i32 r = with_stream(g, false, [](z_stream* z) { return deflateEnd(z); });
    drop_stream(g);
    return r;
}
// Like real zlib, `state` is non-null while a stream is initialised (code may test it).
void set_state(gaddr g, bool live) { mem().write<u32>(g + kState, live ? g : 0); }
i32 h_inflateInit2_(gaddr g, i32 bits, gaddr, i32) {
    drop_stream(g);
    const i32 r = with_stream(g, true, [&](z_stream* z) { return inflateInit2(z, bits); });
    set_state(g, r == Z_OK);
    return r;
}
i32 h_inflateInit_(gaddr g, gaddr, i32) {
    drop_stream(g);
    const i32 r = with_stream(g, true, [](z_stream* z) { return inflateInit(z); });
    set_state(g, r == Z_OK);
    return r;
}
i32 h_inflate(gaddr g, i32 flush) {
    const u32 in = mem().read<u32>(g + kNextIn), avail = mem().read<u32>(g + kAvailIn), total = mem().read<u32>(g + kTotalIn);
    std::uint8_t head[8] = {};
    if (in && avail) std::memcpy(head, mem().ptr(in), std::min<u32>(avail, 8));
    const i32 r = with_stream(g, false, [&](z_stream* z) { return inflate(z, flush); });
    if (r == Z_DATA_ERROR || r == Z_STREAM_ERROR) {
        const GuestThread* t = GuestThread::current();
        std::fprintf(stderr, "[zlib] inflate(0x%x) -> %d on tid %u: next_in 0x%x avail_in %u total_in %u, first bytes %02x %02x %02x %02x %02x %02x %02x %02x, known stream %d\n",
                     g, r, t ? t->tid() : 0, in, avail, total, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], host_stream(g, false) != nullptr);
    }
    return r;
}
i32 h_inflateReset(gaddr g) { return with_stream(g, false, [](z_stream* z) { return inflateReset(z); }); }
i32 h_inflateEnd(gaddr g) {
    const i32 r = with_stream(g, false, [](z_stream* z) { return inflateEnd(z); });
    drop_stream(g);
    set_state(g, false);
    return r;
}
u32 h_crc32(u32 crc, gaddr buf, u32 len) { return static_cast<u32>(::crc32(crc, buf ? mem().ptr<Bytef>(buf) : Z_NULL, len)); }
u32 h_adler32(u32 a, gaddr buf, u32 len) { return static_cast<u32>(::adler32(a, buf ? mem().ptr<Bytef>(buf) : Z_NULL, len)); }

// ---- SHA1 (bionic SHA1_CTX: u32 state[5], u32 count[2] (bits, low/high), u8 buffer[64]) ----
u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
void sha1_block(u32 st[5], const std::uint8_t* p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        const u32 f = i < 20 ? ((b & c) | (~b & d)) + 0x5A827999 : i < 40 ? (b ^ c ^ d) + 0x6ED9EBA1
                    : i < 60 ? ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC : (b ^ c ^ d) + 0xCA62C1D6;
        const u32 t = rol(a, 5) + f + e + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}
void h_sha1_init(gaddr ctx) {
    const u32 init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::memset(mem().ptr(ctx), 0, 92);
    std::memcpy(mem().ptr(ctx), init, sizeof init);
}
void sha1_update(gaddr ctx, const std::uint8_t* data, u32 len) {
    u32* st = mem().ptr<u32>(ctx);
    u32* count = st + 5;
    std::uint8_t* buf = mem().ptr<std::uint8_t>(ctx + 28);
    u32 used = (count[0] >> 3) & 63;
    const std::uint64_t bits = (std::uint64_t(count[1]) << 32 | count[0]) + std::uint64_t(len) * 8;
    count[0] = static_cast<u32>(bits);
    count[1] = static_cast<u32>(bits >> 32);
    while (len) {
        const u32 n = std::min(len, 64 - used);
        std::memcpy(buf + used, data, n);
        used += n; data += n; len -= n;
        if (used == 64) { sha1_block(st, buf); used = 0; }
    }
}
void h_sha1_update(gaddr ctx, gaddr data, u32 len) { sha1_update(ctx, mem().ptr<std::uint8_t>(data), len); }
void h_sha1_final(gaddr digest, gaddr ctx) {
    const u32* count = mem().ptr<u32>(ctx + 20);
    std::uint8_t len_be[8];
    const std::uint64_t bits = std::uint64_t(count[1]) << 32 | count[0];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    const std::uint8_t pad = 0x80, zero = 0;
    sha1_update(ctx, &pad, 1);
    while (((mem().ptr<u32>(ctx + 20)[0] >> 3) & 63) != 56) sha1_update(ctx, &zero, 1);
    sha1_update(ctx, len_be, 8);
    const u32* st = mem().ptr<u32>(ctx);
    for (int i = 0; i < 20; ++i) mem().write<std::uint8_t>(digest + i, static_cast<std::uint8_t>(st[i / 4] >> (24 - 8 * (i % 4))));
}
}  // namespace

void register_libz() {
    def("deflateInit2_", wrap<h_deflateInit2_>); def("deflateInit_", wrap<h_deflateInit_>); def("deflate", wrap<h_deflate>);
    def("deflateReset", wrap<h_deflateReset>); def("deflateEnd", wrap<h_deflateEnd>);
    def("inflateInit2_", wrap<h_inflateInit2_>); def("inflateInit_", wrap<h_inflateInit_>); def("inflate", wrap<h_inflate>);
    def("inflateReset", wrap<h_inflateReset>); def("inflateEnd", wrap<h_inflateEnd>);
    def("crc32", wrap<h_crc32>); def("adler32", wrap<h_adler32>);
    def("SHA1Init", wrap<h_sha1_init>); def("SHA1Update", wrap<h_sha1_update>); def("SHA1Final", wrap<h_sha1_final>);
}

}  // namespace skim::hle
