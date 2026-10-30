// Guest address space: one 4 GiB host reservation, so a guest address is an offset from `base`.
// dynarmic uses the same reservation for fastmem; unmapped pages stay PROT_NONE and fault.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace skim {

using gaddr = std::uint32_t;

// Fixed guest layout.
namespace layout {
constexpr gaddr kThunks = 0x0001'0000;      // import thunks, 8 bytes each
constexpr gaddr kThunksEnd = 0x0010'0000;
constexpr gaddr kStatic = 0x0010'0000;      // HLE data objects (bump allocated, never freed)
constexpr gaddr kStaticEnd = 0x0100'0000;
constexpr gaddr kLibraries = 0x1000'0000;   // ELF images
constexpr gaddr kLibrariesEnd = 0x2000'0000;
constexpr gaddr kMmap = 0x2000'0000;        // guest mmap, heap slabs, thread stacks
constexpr gaddr kMmapEnd = 0xF000'0000;
constexpr gaddr kKuser = 0xFFFF'0000;       // Linux ARM kuser helpers page
}  // namespace layout

class Memory {
public:
    static Memory& get();

    std::uint8_t* base() const { return base_; }
    template<typename T = void>
    T* ptr(gaddr a) const { return reinterpret_cast<T*>(base_ + a); }
    gaddr addr(const void* p) const { return static_cast<gaddr>(static_cast<const std::uint8_t*>(p) - base_); }

    template<typename T>
    T read(gaddr a) const { T v; std::memcpy(&v, base_ + a, sizeof v); return v; }
    template<typename T>
    void write(gaddr a, T v) { std::memcpy(base_ + a, &v, sizeof v); }

    std::string cstr(gaddr a) const { return a ? std::string(ptr<char>(a)) : std::string(); }

    bool is_mapped(gaddr a) const;
    std::size_t host_page() const { return host_page_; }

    // Maps [a, a+size) read/write and zeroed. Fails if any page is already mapped.
    bool map_fixed(gaddr a, std::uint32_t size);
    // Maps a free range in the mmap region; returns 0 when out of space.
    gaddr map_anywhere(std::uint32_t size, std::uint32_t align = 0x1000);
    void unmap(gaddr a, std::uint32_t size);

    // Small permanent objects for HLE (FILE structs, data imports, strings).
    gaddr static_alloc(std::uint32_t size, std::uint32_t align = 8);
    gaddr static_str(const std::string& s);

    // Mapped ranges, for /proc/self/maps and diagnostics.
    std::vector<std::pair<gaddr, std::uint32_t>> mapped_ranges() const;

private:
    Memory();
    std::uint32_t round(std::uint32_t size) const;
    void commit(gaddr a, std::uint32_t size);

    std::uint8_t* base_ = nullptr;
    std::size_t host_page_ = 4096;
    mutable std::mutex lock_;
    std::vector<std::uint8_t> mapped_;           // one byte per 4 KiB guest page
    std::map<gaddr, std::uint32_t> free_;        // free ranges of the mmap region: start -> size
    gaddr static_next_ = layout::kStatic;
};

// Guest malloc: size classes from slabs, large blocks straight from map_anywhere.
namespace heap {
gaddr malloc(std::uint32_t size);
gaddr calloc(std::uint32_t n, std::uint32_t size);
gaddr realloc(gaddr p, std::uint32_t size);
gaddr memalign(std::uint32_t align, std::uint32_t size);
void free(gaddr p);
std::uint32_t usable_size(gaddr p);
gaddr strdup(const std::string& s);
}  // namespace heap

}  // namespace skim
