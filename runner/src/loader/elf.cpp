#include "loader/elf.h"

#include <elf.h>

#include <cstdio>
#include <fstream>
#include <iterator>

#include "guest/cpu.h"
#include "hle/registry.h"

namespace skim::loader {

namespace {
constexpr std::uint32_t kLibAlign = 0x10'0000;

std::vector<std::unique_ptr<Library>>& libs() {
    static std::vector<std::unique_ptr<Library>> v;
    return v;
}

gaddr next_base() {
    gaddr b = layout::kLibraries;
    for (const auto& l : libs()) b = std::max(b, (l->end + kLibAlign - 1) & ~(kLibAlign - 1));
    return b;
}

bool fail(const std::string& path, const char* why) {
    std::fprintf(stderr, "[loader] %s: %s\n", path.c_str(), why);
    return false;
}
}  // namespace

const std::vector<std::unique_ptr<Library>>& all() { return libs(); }

Library* find(const std::string& name) {
    for (auto& l : libs())
        if (l->name == name) return l.get();
    return nullptr;
}

Library* containing(gaddr a) {
    for (auto& l : libs())
        if (a >= l->base && a < l->end) return l.get();
    return nullptr;
}

gaddr lookup(const std::string& symbol) {
    for (auto& l : libs())
        if (auto it = l->exports.find(symbol); it != l->exports.end()) return it->second;
    return 0;
}

Library* load(const std::string& host_path, const std::string& name) {
    std::ifstream f(host_path, std::ios::binary);
    if (!f) { fail(host_path, "cannot open"); return nullptr; }
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(f), {}};
    if (file.size() < sizeof(Elf32_Ehdr)) { fail(host_path, "too small"); return nullptr; }
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(file.data());
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM) {
        fail(host_path, "not an ELF32 ARM file");
        return nullptr;
    }
    const auto* ph = reinterpret_cast<const Elf32_Phdr*>(file.data() + eh->e_phoff);

    auto lib = std::make_unique<Library>();
    lib->name = name;
    std::uint32_t span = 0;
    const Elf32_Phdr* dyn_ph = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) span = std::max(span, ph[i].p_vaddr + ph[i].p_memsz);
        if (ph[i].p_type == PT_DYNAMIC) dyn_ph = &ph[i];
    }
    Memory& m = Memory::get();
    lib->base = next_base();
    lib->end = lib->base + span;
    if (!m.map_fixed(lib->base, span)) { fail(host_path, "cannot map"); return nullptr; }
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) {
            std::memcpy(m.ptr(lib->base + ph[i].p_vaddr), file.data() + ph[i].p_offset, ph[i].p_filesz);
            const gaddr s = (lib->base + ph[i].p_vaddr) & ~0xFFFu, e = (lib->base + ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFu;
            lib->segments.push_back({s, e, (ph[i].p_flags & PF_W) != 0, (ph[i].p_flags & PF_X) != 0});
        }
        if (ph[i].p_type == PT_ARM_EXIDX) { lib->exidx = lib->base + ph[i].p_vaddr; lib->exidx_count = ph[i].p_memsz / 8; }
    }
    if (!dyn_ph) { fail(host_path, "no PT_DYNAMIC"); return nullptr; }

    // Dynamic section.
    gaddr symtab = 0, strtab = 0, hash = 0, rel = 0, jmprel = 0, init_array = 0;
    std::uint32_t relsz = 0, pltrelsz = 0, init_arraysz = 0;
    for (gaddr d = lib->base + dyn_ph->p_vaddr;; d += 8) {
        const auto tag = m.read<std::int32_t>(d);
        const auto val = m.read<std::uint32_t>(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SYMTAB: symtab = lib->base + val; break;
        case DT_STRTAB: strtab = lib->base + val; break;
        case DT_HASH: hash = lib->base + val; break;
        case DT_REL: rel = lib->base + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = lib->base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT: lib->init = lib->base + val; break;
        case DT_INIT_ARRAY: init_array = lib->base + val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = val; break;
        default: break;
        }
    }
    if (!symtab || !strtab || !hash) { fail(host_path, "missing DT_SYMTAB/DT_STRTAB/DT_HASH"); return nullptr; }
    const std::uint32_t nsyms = m.read<std::uint32_t>(hash + 4);
    auto sym = [&](std::uint32_t i) { return m.read<Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    auto sym_name = [&](const Elf32_Sym& s) { return std::string(m.ptr<char>(strtab + s.st_name)); };

    for (std::uint32_t i = 1; i < nsyms; ++i) {
        const Elf32_Sym s = sym(i);
        const int bind = ELF32_ST_BIND(s.st_info);
        if (s.st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK)) lib->exports.emplace(sym_name(s), lib->base + s.st_value);
    }

    // Symbol resolution: self, earlier libraries, HLE data, HLE functions, lazy stub.
    std::unordered_map<std::uint32_t, gaddr> cache;
    auto resolve = [&](std::uint32_t index) -> gaddr {
        if (index == 0) return 0;
        if (auto it = cache.find(index); it != cache.end()) return it->second;
        const Elf32_Sym s = sym(index);
        const std::string n = sym_name(s);
        gaddr v = 0;
        if (s.st_shndx != SHN_UNDEF) v = lib->base + s.st_value;
        else if (gaddr g = lookup(n)) v = hle::traced(n) ? hle::trace_thunk(n, g) : g;
        else if (gaddr d = hle::data(n)) v = d;
        else if (hle::has_function(n)) v = hle::function_thunk(n);
        else if (ELF32_ST_BIND(s.st_info) == STB_WEAK) v = 0;
        else if (ELF32_ST_TYPE(s.st_info) == STT_OBJECT) {
            v = m.static_alloc(std::max<std::uint32_t>(s.st_size, 64));
            std::fprintf(stderr, "[loader] %s: data import %s has no HLE object; bound to zeroed memory\n", name.c_str(), n.c_str());
        } else {
            v = hle::function_thunk(n);
            lib->unresolved.push_back(n);
        }
        cache.emplace(index, v);
        return v;
    };

    auto relocate = [&](gaddr table, std::uint32_t size) -> bool {
        for (gaddr r = table; r < table + size; r += sizeof(Elf32_Rel)) {
            const auto e = m.read<Elf32_Rel>(r);
            const gaddr where = lib->base + e.r_offset;
            const std::uint32_t type = ELF32_R_TYPE(e.r_info), si = ELF32_R_SYM(e.r_info);
            switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: m.write<std::uint32_t>(where, m.read<std::uint32_t>(where) + lib->base); break;
            case R_ARM_ABS32: m.write<std::uint32_t>(where, m.read<std::uint32_t>(where) + resolve(si)); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: m.write<std::uint32_t>(where, resolve(si)); break;
            default:
                std::fprintf(stderr, "[loader] %s: unsupported relocation type %u at 0x%x\n", name.c_str(), type, e.r_offset);
                return false;
            }
        }
        return true;
    };
    if (rel && !relocate(rel, relsz)) return nullptr;
    if (jmprel && !relocate(jmprel, pltrelsz)) return nullptr;

    for (std::uint32_t i = 0; i < init_arraysz / 4; ++i) {
        const gaddr fn = m.read<gaddr>(init_array + 4 * i);
        if (fn != 0 && fn != 0xFFFF'FFFF) lib->init_array.push_back(fn);
    }

    std::fprintf(stderr, "[loader] %s at 0x%08x-0x%08x: %zu exports, %zu constructors, %zu unimplemented imports\n",
                 name.c_str(), lib->base, lib->end, lib->exports.size(), lib->init_array.size() + (lib->init ? 1 : 0),
                 lib->unresolved.size());
    libs().push_back(std::move(lib));
    return libs().back().get();
}

void run_constructors(Library& lib, GuestThread& t) {
    if (lib.init) t.call(lib.init, {});
    for (gaddr fn : lib.init_array) t.call(fn, {});
}

}  // namespace skim::loader
