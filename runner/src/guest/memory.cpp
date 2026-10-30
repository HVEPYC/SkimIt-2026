#include "guest/memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace skim {

namespace {
constexpr std::uint64_t kArenaSize = 1ull << 32;
constexpr std::uint32_t kGuestPage = 0x1000;

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "[mem] fatal: %s\n", what);
    std::abort();
}
}  // namespace

Memory& Memory::get() {
    static Memory m;
    return m;
}

Memory::Memory() {
    host_page_ = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* p = mmap(nullptr, kArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) die("cannot reserve the 4 GiB guest arena");
    base_ = static_cast<std::uint8_t*>(p);
    mapped_.assign(kArenaSize / kGuestPage, 0);
    free_[layout::kMmap] = layout::kMmapEnd - layout::kMmap;
    commit(layout::kThunks, layout::kThunksEnd - layout::kThunks);
    commit(layout::kStatic, layout::kStaticEnd - layout::kStatic);
}

std::uint32_t Memory::round(std::uint32_t size) const {
    const std::uint32_t page = static_cast<std::uint32_t>(std::max<std::size_t>(host_page_, kGuestPage));
    return (size + page - 1) & ~(page - 1);
}

void Memory::commit(gaddr a, std::uint32_t size) {
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(base_ + a) & ~(host_page_ - 1);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(base_ + a) + size + host_page_ - 1) & ~(host_page_ - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE) != 0) die("mprotect commit");
    for (gaddr p = a / kGuestPage; p < (static_cast<std::uint64_t>(a) + size + kGuestPage - 1) / kGuestPage; ++p) mapped_[p] = 1;
}

bool Memory::is_mapped(gaddr a) const { return mapped_[a / kGuestPage] != 0; }

bool Memory::map_fixed(gaddr a, std::uint32_t size) {
    std::lock_guard g{lock_};
    size = round(size);
    for (gaddr p = a / kGuestPage; p < (static_cast<std::uint64_t>(a) + size) / kGuestPage; ++p)
        if (mapped_[p]) return false;
    // Carve the range out of the free list if it lies in the mmap region.
    if (a >= layout::kMmap && a < layout::kMmapEnd) {
        auto it = free_.upper_bound(a);
        if (it == free_.begin()) return false;
        --it;
        const gaddr fs = it->first, fe = it->first + it->second;
        if (a < fs || a + size > fe) return false;
        free_.erase(it);
        if (a > fs) free_[fs] = a - fs;
        if (a + size < fe) free_[a + size] = fe - (a + size);
    }
    commit(a, size);
    return true;
}

gaddr Memory::map_anywhere(std::uint32_t size, std::uint32_t align) {
    std::lock_guard g{lock_};
    size = round(size);
    align = std::max<std::uint32_t>(align, static_cast<std::uint32_t>(std::max<std::size_t>(host_page_, kGuestPage)));
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const gaddr fs = it->first, fe = it->first + it->second;
        const gaddr a = (fs + align - 1) & ~(align - 1);
        if (a < fs || static_cast<std::uint64_t>(a) + size > fe) continue;
        free_.erase(it);
        if (a > fs) free_[fs] = a - fs;
        if (a + size < fe) free_[a + size] = fe - (a + size);
        commit(a, size);
        return a;
    }
    return 0;
}

void Memory::unmap(gaddr a, std::uint32_t size) {
    std::lock_guard g{lock_};
    size = round(size);
    void* host = base_ + a;
    // Fresh anonymous PROT_NONE pages: drops the contents and makes later accesses fault.
    mmap(host, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    for (gaddr p = a / kGuestPage; p < (static_cast<std::uint64_t>(a) + size) / kGuestPage; ++p) mapped_[p] = 0;
    if (a < layout::kMmap || a >= layout::kMmapEnd) return;
    gaddr s = a, e = a + size;
    auto next = free_.lower_bound(s);
    if (next != free_.end() && next->first == e) { e += next->second; free_.erase(next); }
    auto prev = free_.lower_bound(s);
    if (prev != free_.begin()) {
        --prev;
        if (prev->first + prev->second == s) { s = prev->first; free_.erase(prev); }
    }
    free_[s] = e - s;
}

gaddr Memory::static_alloc(std::uint32_t size, std::uint32_t align) {
    std::lock_guard g{lock_};
    const gaddr a = (static_next_ + align - 1) & ~(align - 1);
    if (a + size > layout::kStaticEnd) die("static region exhausted");
    static_next_ = a + size;
    std::memset(base_ + a, 0, size);
    return a;
}

gaddr Memory::static_str(const std::string& s) {
    const gaddr a = static_alloc(static_cast<std::uint32_t>(s.size() + 1), 1);
    std::memcpy(base_ + a, s.c_str(), s.size() + 1);
    return a;
}

std::vector<std::pair<gaddr, std::uint32_t>> Memory::mapped_ranges() const {
    std::lock_guard g{lock_};
    std::vector<std::pair<gaddr, std::uint32_t>> out;
    for (std::size_t p = 0; p < mapped_.size(); ++p) {
        if (!mapped_[p]) continue;
        std::size_t q = p;
        while (q < mapped_.size() && mapped_[q]) ++q;
        out.emplace_back(static_cast<gaddr>(p * kGuestPage), static_cast<std::uint32_t>((q - p) * kGuestPage));
        p = q;
    }
    return out;
}

// ---- guest heap ----
namespace heap {
namespace {
constexpr std::uint32_t kHeader = 8;
constexpr std::uint32_t kMagic = 0x5A000000;
constexpr std::uint32_t kLarge = kMagic | 0xFFFF;
constexpr std::uint32_t kAligned = kMagic | 0xFFFE;
constexpr std::uint32_t kSlab = 256 * 1024;
constexpr std::uint32_t kMaxSmall = 32768;

struct Heap {
    std::mutex lock;
    std::vector<std::uint32_t> sizes;   // block size per class, header included
    std::vector<gaddr> free_list;       // per class
    std::vector<gaddr> bump, bump_end;  // per class: current slab
    Heap() {
        for (std::uint32_t s = 16; s <= 128; s += 16) sizes.push_back(s);
        for (std::uint32_t p = 128; p < kMaxSmall; p *= 2)
            for (std::uint32_t q = 1; q <= 4; ++q) sizes.push_back(p + p * q / 4);
        free_list.assign(sizes.size(), 0);
        bump.assign(sizes.size(), 0);
        bump_end.assign(sizes.size(), 0);
    }
    int class_for(std::uint32_t block) const {
        auto it = std::lower_bound(sizes.begin(), sizes.end(), block);
        return it == sizes.end() ? -1 : static_cast<int>(it - sizes.begin());
    }
};

Heap& H() {
    static Heap h;
    return h;
}

Memory& M() { return Memory::get(); }

void set_header(gaddr user, std::uint32_t kind, std::uint32_t extra) {
    M().write<std::uint32_t>(user - 8, kind);
    M().write<std::uint32_t>(user - 4, extra);
}
}  // namespace

gaddr malloc(std::uint32_t size) {
    if (size > 0xF000'0000u) return 0;
    const std::uint32_t block = std::max<std::uint32_t>(size + kHeader, 16);
    Heap& h = H();
    const int c = block <= kMaxSmall ? h.class_for(block) : -1;
    if (c < 0) {
        const std::uint32_t len = size + kHeader;
        const gaddr raw = M().map_anywhere(len);
        if (!raw) return 0;
        set_header(raw + kHeader, kLarge, len);
        return raw + kHeader;
    }
    std::lock_guard g{h.lock};
    gaddr b = h.free_list[c];
    if (b) {
        h.free_list[c] = M().read<gaddr>(b + kHeader);
    } else {
        if (h.bump[c] + h.sizes[c] > h.bump_end[c]) {
            const gaddr slab = M().map_anywhere(kSlab);
            if (!slab) return 0;
            h.bump[c] = slab;
            h.bump_end[c] = slab + kSlab;
        }
        b = h.bump[c];
        h.bump[c] += h.sizes[c];
    }
    set_header(b + kHeader, kMagic | static_cast<std::uint32_t>(c), 0);
    return b + kHeader;
}

std::uint32_t usable_size(gaddr p) {
    if (!p) return 0;
    const std::uint32_t kind = M().read<std::uint32_t>(p - 8), extra = M().read<std::uint32_t>(p - 4);
    if (kind == kLarge) return extra - kHeader;
    if (kind == kAligned) return usable_size(p - extra) - extra;
    return H().sizes[kind & 0xFFFF] - kHeader;
}

void free(gaddr p) {
    if (!p) return;
    const std::uint32_t kind = M().read<std::uint32_t>(p - 8), extra = M().read<std::uint32_t>(p - 4);
    if ((kind & 0xFF000000) != kMagic) {
        std::fprintf(stderr, "[heap] free of non-heap pointer 0x%08x ignored\n", p);
        return;
    }
    if (kind == kLarge) { M().unmap(p - kHeader, extra); return; }
    if (kind == kAligned) { free(p - extra); return; }
    Heap& h = H();
    const std::uint32_t c = kind & 0xFFFF;
    std::lock_guard g{h.lock};
    M().write<gaddr>(p, h.free_list[c]);
    h.free_list[c] = p - kHeader;
}

gaddr calloc(std::uint32_t n, std::uint32_t size) {
    const std::uint64_t total = static_cast<std::uint64_t>(n) * size;
    if (total > 0xF000'0000u) return 0;
    const gaddr p = malloc(static_cast<std::uint32_t>(total));
    if (p) std::memset(M().ptr(p), 0, total);
    return p;
}

gaddr realloc(gaddr p, std::uint32_t size) {
    if (!p) return malloc(size);
    if (size == 0) { free(p); return 0; }
    const std::uint32_t have = usable_size(p);
    if (have >= size) return p;
    const gaddr q = malloc(size);
    if (!q) return 0;
    std::memcpy(M().ptr(q), M().ptr(p), have);
    free(p);
    return q;
}

gaddr memalign(std::uint32_t align, std::uint32_t size) {
    if (align <= 8) return malloc(size);
    const gaddr raw = malloc(size + align + kHeader);
    if (!raw) return 0;
    gaddr p = (raw + align - 1) & ~(align - 1);
    if (p == raw) return raw;
    if (p - raw < kHeader) p += align;
    set_header(p, kAligned, p - raw);
    return p;
}

gaddr strdup(const std::string& s) {
    const gaddr p = malloc(static_cast<std::uint32_t>(s.size() + 1));
    std::memcpy(M().ptr(p), s.c_str(), s.size() + 1);
    return p;
}
}  // namespace heap

}  // namespace skim
