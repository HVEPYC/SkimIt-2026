#include "hle/registry.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <vector>

#include "hle/modules.h"
#include "loader/elf.h"

namespace skim::hle {

namespace {
constexpr std::uint32_t kSvc = 0xEF00'0000;   // svc #imm24 (ARM)
constexpr std::uint32_t kBxLr = 0xE12F'FF1E;
constexpr gaddr kFirstThunk = layout::kThunks + 8;

struct Entry {
    std::string name;
    HleFn fn = nullptr;
    bool warned = false;
};

struct Table {
    std::mutex lock;
    std::deque<Entry> entries;                       // index -> entry; thunk = kFirstThunk + 8 * index
    std::unordered_map<std::string, std::uint32_t> index;
    std::unordered_map<std::string, HleFn> functions;
    std::unordered_map<std::string, gaddr> data;
    std::vector<std::pair<std::string, HleFn>> prefixes;
};

HleFn lookup_fn(Table& t, const std::string& name) {
    if (auto it = t.functions.find(name); it != t.functions.end()) return it->second;
    for (auto& [p, fn] : t.prefixes)
        if (name.compare(0, p.size(), p) == 0) return fn;
    return nullptr;
}

Table& T() {
    static Table t;
    return t;
}

bool stub_unimplemented() {
    static const bool on = std::getenv("SKIM_STUB_UNIMPLEMENTED") != nullptr;
    return on;
}

// Linux ARM "kuser helpers" at the top of the address space; old bionic and libgcc may call them.
void write_kuser_page() {
    Memory& m = Memory::get();
    m.map_fixed(layout::kKuser, 0x1000);
    auto put = [&](gaddr a, std::initializer_list<std::uint32_t> code) {
        for (std::uint32_t w : code) { m.write(a, w); a += 4; }
    };
    put(0xFFFF'0FA0, {0xF57F'F05B, kBxLr});  // __kuser_memory_barrier: dmb ish
    put(0xFFFF'0FC0, {0xE192'3F9F, 0xE053'3000, 0x0182'3F91, 0x0333'0001, 0x0AFF'FFFA, 0xE273'0000, kBxLr});  // __kuser_cmpxchg
    put(0xFFFF'0FE0, {0xEE1D'0F70, kBxLr});  // __kuser_get_tls: mrc p15, 0, r0, c13, c0, 3
    m.write<std::uint32_t>(0xFFFF'0FFC, 3);  // __kuser_helper_version
}
}  // namespace

void def(const char* name, HleFn fn) { T().functions[name] = fn; }
void def_data(const char* name, gaddr addr) { T().data[name] = addr; }
void def_prefix(const char* prefix, HleFn fn) { T().prefixes.emplace_back(prefix, fn); }
bool has_function(const std::string& name) { return lookup_fn(T(), name) != nullptr; }
std::size_t implemented_count() { return T().functions.size(); }
std::vector<std::string> function_names() {
    std::vector<std::string> v;
    for (auto& [n, f] : T().functions) v.push_back(n);
    for (auto& [n, a] : T().data) v.push_back(n);
    return v;
}

gaddr data(const std::string& name) {
    auto it = T().data.find(name);
    return it == T().data.end() ? 0 : it->second;
}

gaddr function_thunk(const std::string& name) {
    Table& t = T();
    std::lock_guard g{t.lock};
    if (auto it = t.index.find(name); it != t.index.end()) return kFirstThunk + 8 * it->second;
    const auto i = static_cast<std::uint32_t>(t.entries.size());
    const gaddr thunk = kFirstThunk + 8 * i;
    if (thunk + 8 > layout::kThunksEnd) { std::fprintf(stderr, "[hle] thunk area full\n"); std::abort(); }
    t.entries.push_back({name, lookup_fn(t, name)});
    t.index.emplace(name, i);
    Memory::get().write<std::uint32_t>(thunk, kSvc | (kSvcImport + i));
    Memory::get().write<std::uint32_t>(thunk + 4, kBxLr);
    return thunk;
}

void dispatch(GuestThread& t, std::uint32_t index) {
    Table& tab = T();
    Entry* e;
    {
        std::lock_guard g{tab.lock};
        if (index >= tab.entries.size()) t.fatal("svc for unknown import %u", index);
        e = &tab.entries[index];  // entries only grow at load time, before guest threads run
    }
    Ctx c(t);
    if (e->fn) {
        e->fn(c);
    } else if (stub_unimplemented()) {
        if (!e->warned) std::fprintf(stderr, "[hle] unimplemented %s called from %s; returning 0\n", e->name.c_str(), describe(t.regs()[14]).c_str());
        e->warned = true;
        c.ret(0);
    } else {
        t.fatal("unimplemented import %s called from %s", e->name.c_str(), describe(t.regs()[14]).c_str());
    }
    if (!c.no_return) t.set_pc(t.regs()[14]);
}

namespace {
std::unordered_map<std::string, gaddr> trace_targets;

void trace_call(Ctx& c) {
    auto& R = c.t.regs();
    const std::string self = describe(R[15] - 4);  // "thunk:<name>@trace"
    const std::string name = self.substr(6, self.size() - 6 - 6);
    const std::uint32_t a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], lr = R[14];
    const gaddr real = trace_targets.at(name);
    const auto r = c.t.call(real, {a0, a1, a2, a3});
    std::fprintf(stderr, "[trace] tid %u (pthread 0x%08x) %s(0x%x, 0x%x, 0x%x, 0x%x) from %s -> 0x%x\n", c.t.tid(), c.t.tls(),
                 name.c_str(), a0, a1, a2, a3, describe(lr).c_str(), static_cast<std::uint32_t>(r));
    c.ret64(r);
}
}  // namespace

bool traced(const std::string& name) {
    static const std::string list = [] { const char* e = std::getenv("SKIM_TRACE_IMPORTS"); return e ? "," + std::string(e) + "," : std::string(); }();
    return !list.empty() && list.find("," + name + ",") != std::string::npos;
}

gaddr trace_thunk(const std::string& name, gaddr real) {
    trace_targets[name] = real;
    T().functions[name + "@trace"] = trace_call;
    return function_thunk(name + "@trace");
}

std::string describe(gaddr pc) {
    char buf[160];
    if (pc >= kFirstThunk && pc < layout::kThunksEnd) {
        const std::uint32_t i = (pc - kFirstThunk) / 8;
        if (i < T().entries.size()) return "thunk:" + T().entries[i].name;
    }
    if (const Library* l = loader::containing(pc)) {
        std::snprintf(buf, sizeof buf, "%s+0x%x", l->name.c_str(), pc - l->base);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "0x%08x", pc);
    return buf;
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    Memory::get().write<std::uint32_t>(kReturnThunk, kSvc | kSvcReturn);
    write_kuser_page();
    register_all();
}

}  // namespace skim::hle
