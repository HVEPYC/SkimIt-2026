#include "runtime/zip.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace skim::runtime {

namespace {
std::uint16_t le16(const unsigned char* p) { return p[0] | p[1] << 8; }
std::uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }

bool read_at(std::ifstream& f, std::uint64_t off, void* buf, std::size_t n) {
    f.seekg(static_cast<std::streamoff>(off));
    f.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(f.gcount()) == n;
}
}  // namespace

int unzip(const std::string& zip, const std::vector<std::string>& prefixes, const std::string& dest) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) { std::fprintf(stderr, "[zip] cannot open %s\n", zip.c_str()); return -1; }
    f.seekg(0, std::ios::end);
    const std::uint64_t size = f.tellg();
    // End of central directory: the last 22..65557 bytes.
    const std::uint64_t tail = std::min<std::uint64_t>(size, 65557);
    std::vector<unsigned char> t(tail);
    if (!read_at(f, size - tail, t.data(), tail)) return -1;
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail) - 22; i >= 0; --i)
        if (le32(&t[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { std::fprintf(stderr, "[zip] %s: no end of central directory\n", zip.c_str()); return -1; }
    const std::uint32_t count = le16(&t[eocd + 10]), cd_size = le32(&t[eocd + 12]), cd_off = le32(&t[eocd + 16]);
    std::vector<unsigned char> cd(cd_size);
    if (!read_at(f, cd_off, cd.data(), cd_size)) return -1;

    int written = 0;
    for (std::size_t p = 0, i = 0; i < count && p + 46 <= cd.size(); ++i) {
        const unsigned char* e = &cd[p];
        if (le32(e) != 0x02014b50) return -1;
        const std::uint16_t method = le16(e + 10), name_len = le16(e + 28), extra_len = le16(e + 30), comment_len = le16(e + 32);
        const std::uint32_t csize = le32(e + 20), usize = le32(e + 24), local = le32(e + 42);
        const std::string name(reinterpret_cast<const char*>(e + 46), name_len);
        p += 46 + name_len + extra_len + comment_len;
        if (name.empty() || name.back() == '/') continue;
        bool want = false;
        for (const auto& pre : prefixes) want |= name.compare(0, pre.size(), pre) == 0;
        if (!want || name.find("..") != std::string::npos) continue;

        unsigned char lh[30];
        if (!read_at(f, local, lh, 30) || le32(lh) != 0x04034b50) return -1;
        std::vector<unsigned char> in(csize), out(usize);
        if (!read_at(f, local + 30 + le16(lh + 26) + le16(lh + 28), in.data(), csize)) return -1;
        if (method == 0) {
            out = std::move(in);
        } else if (method == 8) {
            z_stream z{};
            inflateInit2(&z, -MAX_WBITS);
            z.next_in = in.data();
            z.avail_in = csize;
            z.next_out = out.data();
            z.avail_out = usize;
            const int r = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (r != Z_STREAM_END) { std::fprintf(stderr, "[zip] %s: inflate failed\n", name.c_str()); return -1; }
        } else {
            std::fprintf(stderr, "[zip] %s: unsupported method %u\n", name.c_str(), method);
            return -1;
        }
        const std::filesystem::path target = std::filesystem::path(dest) / name;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream o(target, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!o) return -1;
        ++written;
    }
    return written;
}

}  // namespace skim::runtime
