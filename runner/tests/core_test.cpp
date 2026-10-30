// Core tests: JIT calls (ARM and Thumb), import thunks, host->guest->host nesting, exclusives,
// TLS, heap, printf engine, and the network lockdown.
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

#include "guest/cpu.h"
#include "hle/format.h"
#include "hle/registry.h"

using namespace skim;

static int failures = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        const bool ok_ = (cond);                                              \
        std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #cond);                 \
        if (!ok_) ++failures;                                                 \
    } while (0)

static gaddr code(std::initializer_list<std::uint32_t> words) {
    const gaddr a = Memory::get().map_anywhere(0x1000);
    gaddr p = a;
    for (auto w : words) { Memory::get().write(p, w); p += 4; }
    return a;
}

// A 32-bit result is r0 only; r1 holds whatever the callee left there.
static std::uint32_t r0(std::uint64_t v) { return static_cast<std::uint32_t>(v); }

static gaddr g_add = 0;
static void test_nested(Ctx& c) {
    const std::uint32_t x = c.u32();
    c.ret(static_cast<std::uint32_t>(c.t.call(g_add, {x, 100})));
}

int main() {
    hle::def("test_nested", test_nested);
    hle::init();
    GuestThread t;
    Memory& m = Memory::get();

    g_add = code({0xE0800001, 0xE12FFF1E});  // add r0, r0, r1; bx lr
    CHECK(r0(t.call(g_add, {2, 3})) == 5);

    const gaddr thumb = Memory::get().map_anywhere(0x1000);
    m.write<std::uint16_t>(thumb, 0x1840);      // adds r0, r0, r1
    m.write<std::uint16_t>(thumb + 2, 0x4770);  // bx lr
    CHECK(r0(t.call(thumb | 1, {7, 8})) == 15);

    // push {r4, lr}; mov r4, r0; blx r1; add r0, r0, r4; pop {r4, pc}
    const gaddr outer = code({0xE92D4010, 0xE1A04000, 0xE12FFF31, 0xE0800004, 0xE8BD8010});
    CHECK(r0(t.call(outer, {10, hle::function_thunk("test_nested")})) == 120);

    // 1: ldrex r1, [r0]; add r1, r1, #1; strex r2, r1, [r0]; cmp r2, #0; bne 1b; bx lr
    const gaddr inc = code({0xE1901F9F, 0xE2811001, 0xE1802F91, 0xE3520000, 0x1AFFFFFA, 0xE12FFF1E});
    const gaddr counter = heap::calloc(1, 4);
    for (int i = 0; i < 5; ++i) t.call(inc, {counter});
    CHECK(m.read<std::uint32_t>(counter) == 5);

    CHECK(t.call(0xFFFF'0FE0, {}) == t.tls());  // __kuser_get_tls (mrc p15 TPIDRURO)
    const gaddr word = heap::calloc(1, 4);
    m.write<std::uint32_t>(word, 41);
    CHECK(r0(t.call(0xFFFF'0FC0, {41, 42, word})) == 0 && m.read<std::uint32_t>(word) == 42);  // __kuser_cmpxchg

    // Thunks called like guest functions.
    const gaddr p = static_cast<gaddr>(t.call(hle::function_thunk("malloc"), {100}));
    CHECK(p != 0 && heap::usable_size(p) >= 100);
    const gaddr q = static_cast<gaddr>(t.call(hle::function_thunk("realloc"), {p, 5000}));
    CHECK(q != 0 && heap::usable_size(q) >= 5000);
    t.call(hle::function_thunk("free"), {q});
    const gaddr s = m.static_str("hello");
    CHECK(t.call(hle::function_thunk("strlen"), {s}) == 5);

    // printf engine with a guest va_list: "%d %s %5.2f %llx"
    const gaddr va = heap::malloc(64);
    m.write<std::int32_t>(va, -7);
    m.write<gaddr>(va + 4, s);
    m.write<double>(va + 8, 3.14159);
    m.write<std::uint64_t>(va + 16, 0x1234'5678'9ABCull);
    hle::VaListArgs args(va);
    const std::string out = hle::format("%d %s %5.2f %llx", args);
    CHECK(out == "-7 hello  3.14 123456789abc");

    // Network lockdown: sockets and DNS always fail.
    CHECK(static_cast<std::int32_t>(t.call(hle::function_thunk("socket"), {2, 1, 0})) == -1);
    CHECK(m.read<std::int32_t>(t.errno_addr()) == 13);  // EACCES
    const gaddr host = m.static_str("example.com");
    CHECK(t.call(hle::function_thunk("getaddrinfo"), {host, 0, 0, heap::calloc(1, 4)}) == 4);  // EAI_FAIL
    CHECK(t.call(hle::function_thunk("gethostbyname"), {host}) == 0);

    // zlib through the thunks: crc32 check value, then a deflate -> inflate round trip.
    const gaddr digits = m.static_str("123456789");
    CHECK(r0(t.call(hle::function_thunk("crc32"), {0, digits, 9})) == 0xCBF43926u);
    const std::string text(4000, 'x');
    const gaddr src = heap::strdup(text), packed = heap::malloc(8192), unpacked = heap::malloc(8192);
    const gaddr zs = heap::calloc(1, 56), version = m.static_str(ZLIB_VERSION);
    CHECK(r0(t.call(hle::function_thunk("deflateInit_"), {zs, 9, version, 56})) == 0);
    m.write<gaddr>(zs, src); m.write<std::uint32_t>(zs + 4, 4000);
    m.write<gaddr>(zs + 12, packed); m.write<std::uint32_t>(zs + 16, 8192);
    CHECK(r0(t.call(hle::function_thunk("deflate"), {zs, 4})) == 1);  // Z_FINISH -> Z_STREAM_END
    const std::uint32_t packed_len = m.read<std::uint32_t>(zs + 20);
    t.call(hle::function_thunk("deflateEnd"), {zs});
    std::memset(m.ptr(zs), 0, 56);
    CHECK(r0(t.call(hle::function_thunk("inflateInit_"), {zs, version, 56})) == 0);
    m.write<gaddr>(zs, packed); m.write<std::uint32_t>(zs + 4, packed_len);
    m.write<gaddr>(zs + 12, unpacked); m.write<std::uint32_t>(zs + 16, 8192);
    CHECK(r0(t.call(hle::function_thunk("inflate"), {zs, 4})) == 1);
    CHECK(m.read<std::uint32_t>(zs + 20) == 4000 && std::memcmp(m.ptr(unpacked), text.data(), 4000) == 0 && packed_len < 100);
    t.call(hle::function_thunk("inflateEnd"), {zs});

    // SHA1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
    const gaddr ctx = heap::calloc(1, 92), digest = heap::calloc(1, 20), abc = m.static_str("abc");
    t.call(hle::function_thunk("SHA1Init"), {ctx});
    t.call(hle::function_thunk("SHA1Update"), {ctx, abc, 3});
    t.call(hle::function_thunk("SHA1Final"), {digest, ctx});
    const std::uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                   0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(std::memcmp(m.ptr(digest), want, 20) == 0);

    // Signals: thread B blocks in sem_wait; we pthread_kill it with SIGUSR1; its guest handler
    // (ldr r1, [pc, #4]; str r0, [r1]; bx lr; .word flag) records the signal number; sem_post wakes it.
    const gaddr flag = heap::calloc(1, 4), sem = heap::calloc(1, 4);
    const gaddr handler = code({0xE59F1004, 0xE5810000, 0xE12FFF1E, flag});
    const gaddr act = heap::calloc(1, 16);
    m.write<gaddr>(act, handler);
    t.call(hle::function_thunk("sigaction"), {10, act, 0});
    std::atomic<gaddr> b_handle{0};
    std::atomic<bool> b_done{false};
    std::thread b([&] {
        GuestThread gt;
        b_handle = gt.tls();
        gt.call(hle::function_thunk("sem_wait"), {sem});
        b_done = true;
    });
    while (!b_handle) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let B block
    CHECK(r0(t.call(hle::function_thunk("pthread_kill"), {b_handle.load(), 10})) == 0);
    for (int i = 0; i < 200 && m.read<std::uint32_t>(flag) != 10; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(m.read<std::uint32_t>(flag) == 10 && !b_done);  // handler ran; B still waiting
    t.call(hle::function_thunk("sem_post"), {sem});
    b.join();
    CHECK(b_done);

    std::printf(failures ? "\nFAILED: %d\n" : "\nALL PASSED\n", failures);
    return failures ? 1 : 0;
}
