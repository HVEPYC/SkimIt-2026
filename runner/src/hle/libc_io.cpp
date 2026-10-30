// libc: files and directories (through the VFS), stdio, printf/scanf.
// Guest file descriptors are host file descriptors; FILE and DIR are guest handles mapped to host ones.
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hle/format.h"
#include "hle/modules.h"
#include "hle/registry.h"
#include "hle/vfs.h"

namespace skim {

// ---- VFS ----
namespace vfs {
namespace {
struct State {
    std::mutex lock;
    std::vector<std::pair<std::string, std::string>> mounts;  // longest prefix wins
    std::unordered_map<std::string, std::function<std::string()>> files;
    std::string cwd = "/";
};
State& st() {
    static State s;
    return s;
}
std::string normalize(const std::string& p) {
    std::string full = p.empty() || p[0] != '/' ? st().cwd + "/" + p : p;
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= full.size()) {
        const std::size_t j = full.find('/', i);
        const std::string part = full.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}
}  // namespace

void mount(const std::string& g, const std::string& h) { std::lock_guard l{st().lock}; st().mounts.emplace_back(normalize(g), h); }
void add_file(const std::string& g, const std::string& c) { std::lock_guard l{st().lock}; st().files[normalize(g)] = [c] { return c; }; }
void add_generated(const std::string& g, std::function<std::string()> make) { std::lock_guard l{st().lock}; st().files[normalize(g)] = std::move(make); }
void set_cwd(const std::string& d) { std::lock_guard l{st().lock}; st().cwd = d; }
std::string cwd() { std::lock_guard l{st().lock}; return st().cwd; }

std::string translate(const std::string& guest, std::string* content, bool* is_synthetic) {
    std::unique_lock l{st().lock};
    const std::string p = normalize(guest);
    if (is_synthetic) *is_synthetic = false;
    if (auto it = st().files.find(p); it != st().files.end()) {
        auto make = it->second;
        l.unlock();
        if (content) *content = make();
        if (is_synthetic) *is_synthetic = true;
        return p;
    }
    const std::pair<std::string, std::string>* best = nullptr;
    for (auto& m : st().mounts) {
        const auto& pre = m.first;
        if ((p == pre || (p.compare(0, pre.size(), pre) == 0 && (pre == "/" || p[pre.size()] == '/'))) &&
            (!best || pre.size() > best->first.size()))
            best = &m;
    }
    if (!best) return "";
    return best->second + p.substr(best->first == "/" ? 0 : best->first.size());
}
}  // namespace vfs

namespace hle {
namespace {
using u32 = std::uint32_t;
using i32 = std::int32_t;

char* S(gaddr a) { return gstr(a); }

// Host path for a guest path; sets errno and returns "" when not visible.
std::string host_path(gaddr guest, std::string* content = nullptr, bool* synthetic = nullptr) {
    const std::string g = gstr(guest);
    std::string h = vfs::translate(g, content, synthetic);
    if (h.empty()) {
        std::fprintf(stderr, "[vfs] not visible: %s\n", g.c_str());
        set_errno(ENOENT);
    }
    return h;
}

i32 result(long r) {
    if (r < 0) set_errno(errno);
    return static_cast<i32>(r);
}

// ARM open() flags that differ from x86_64 (same values on arm64 hosts).
int open_flags(u32 g) {
    int h = static_cast<int>(g & ~(040000u | 0100000u | 0200000u | 0400000u));
    if (g & 040000) h |= O_DIRECTORY;
    if (g & 0100000) h |= O_NOFOLLOW;
    if (g & 0400000) h |= O_LARGEFILE;
    return h;
}

int open_synthetic(const std::string& content) {
    int p[2];
    if (pipe(p) != 0) return -1;
    if (write(p[1], content.data(), content.size()) < 0) { close(p[0]); close(p[1]); return -1; }
    close(p[1]);
    return p[0];
}

// bionic (arm) struct stat: 104 bytes, the kernel's stat64 layout.
void put_stat(gaddr out, const struct stat& s) {
    Memory& m = mem();
    std::memset(m.ptr(out), 0, 104);
    m.write<std::uint64_t>(out + 0, s.st_dev);
    m.write<u32>(out + 12, static_cast<u32>(s.st_ino));
    m.write<u32>(out + 16, s.st_mode);
    m.write<u32>(out + 20, static_cast<u32>(s.st_nlink));
    m.write<u32>(out + 24, s.st_uid);
    m.write<u32>(out + 28, s.st_gid);
    m.write<std::uint64_t>(out + 32, s.st_rdev);
    m.write<std::int64_t>(out + 48, s.st_size);
    m.write<u32>(out + 56, static_cast<u32>(s.st_blksize));
    m.write<std::uint64_t>(out + 64, static_cast<std::uint64_t>(s.st_blocks));
    m.write<u32>(out + 72, static_cast<u32>(s.st_atim.tv_sec));
    m.write<u32>(out + 76, static_cast<u32>(s.st_atim.tv_nsec));
    m.write<u32>(out + 80, static_cast<u32>(s.st_mtim.tv_sec));
    m.write<u32>(out + 84, static_cast<u32>(s.st_mtim.tv_nsec));
    m.write<u32>(out + 88, static_cast<u32>(s.st_ctim.tv_sec));
    m.write<u32>(out + 92, static_cast<u32>(s.st_ctim.tv_nsec));
    m.write<std::uint64_t>(out + 96, s.st_ino);
}

i32 stat_common(gaddr path, gaddr out, bool follow) {
    std::string content;
    bool syn = false;
    const std::string h = host_path(path, &content, &syn);
    if (h.empty()) return -1;
    struct stat s{};
    if (syn) {
        s.st_mode = S_IFREG | 0444;
        s.st_size = static_cast<off_t>(content.size());
        s.st_nlink = 1;
    } else if ((follow ? ::stat(h.c_str(), &s) : ::lstat(h.c_str(), &s)) != 0) {
        return result(-1);
    }
    put_stat(out, s);
    return 0;
}

// ---- fd-level ----
void h_open(Ctx& c) {
    const gaddr path = c.u32();
    const u32 flags = c.u32();
    const u32 mode = (flags & O_CREAT) ? c.u32() : 0;
    std::string content;
    bool syn = false;
    const std::string h = host_path(path, &content, &syn);
    if (h.empty()) return c.ret(static_cast<u32>(-1));
    if (syn) return c.ret(static_cast<u32>(result(open_synthetic(content))));
    c.ret(static_cast<u32>(result(::open(h.c_str(), open_flags(flags), mode))));
}
i32 h_close(i32 fd) { return fd <= 2 ? 0 : result(::close(fd)); }
i32 h_read(i32 fd, gaddr buf, u32 n) { return result(::read(fd, mem().ptr(buf), n)); }
i32 h_write(i32 fd, gaddr buf, u32 n) { return result(::write(fd, mem().ptr(buf), n)); }
i32 h_lseek(i32 fd, i32 off, i32 whence) { return result(::lseek(fd, off, whence)); }
std::int64_t h_lseek64(i32 fd, std::int64_t off, i32 whence) {
    const off_t r = ::lseek(fd, off, whence);
    if (r < 0) set_errno(errno);
    return r;
}
i32 h_fstat(i32 fd, gaddr out) {
    struct stat s{};
    if (::fstat(fd, &s) != 0) return result(-1);
    put_stat(out, s);
    return 0;
}
i32 h_stat(gaddr p, gaddr out) { return stat_common(p, out, true); }
i32 h_lstat(gaddr p, gaddr out) { return stat_common(p, out, false); }
i32 h_access(gaddr p, i32 mode) {
    bool syn = false;
    const std::string h = host_path(p, nullptr, &syn);
    if (h.empty()) return -1;
    return syn ? 0 : result(::access(h.c_str(), mode));
}
#define SKIM_PATH_FN(name, call)                                   \
    i32 h_##name(gaddr p, u32 arg) {                               \
        const std::string h = host_path(p);                        \
        if (h.empty()) return -1;                                  \
        (void)arg;                                                 \
        return result(call);                                       \
    }
SKIM_PATH_FN(mkdir, ::mkdir(h.c_str(), arg))
SKIM_PATH_FN(chmod, ::chmod(h.c_str(), arg))
SKIM_PATH_FN(rmdir, ::rmdir(h.c_str()))
SKIM_PATH_FN(unlink, ::unlink(h.c_str()))
SKIM_PATH_FN(truncate, ::truncate(h.c_str(), arg))
#undef SKIM_PATH_FN
i32 h_rename(gaddr a, gaddr b) {
    const std::string ha = host_path(a), hb = host_path(b);
    if (ha.empty() || hb.empty()) return -1;
    return result(::rename(ha.c_str(), hb.c_str()));
}
i32 h_utime(gaddr p, gaddr times) {
    const std::string h = host_path(p);
    if (h.empty()) return -1;
    if (!times) return result(::utime(h.c_str(), nullptr));
    utimbuf t{static_cast<time_t>(mem().read<i32>(times)), static_cast<time_t>(mem().read<i32>(times + 4))};
    return result(::utime(h.c_str(), &t));
}
i32 h_chdir(gaddr p) {
    const std::string h = host_path(p);
    if (h.empty()) return -1;
    vfs::set_cwd(gstr(p));
    return 0;
}
gaddr h_getcwd(gaddr buf, u32 size) {
    const std::string c = vfs::cwd();
    if (!buf) buf = heap::malloc(size = static_cast<u32>(c.size() + 1));
    if (c.size() + 1 > size) { set_errno(ERANGE); return 0; }
    std::memcpy(S(buf), c.c_str(), c.size() + 1);
    return buf;
}
i32 h_readlink(gaddr p, gaddr buf, u32 n) {
    const std::string g = gstr(p);
    if (g == "/proc/self/exe") {  // the app_process the game ran in
        const std::string exe = "/system/bin/app_process";
        const u32 len = std::min<u32>(n, static_cast<u32>(exe.size()));
        std::memcpy(S(buf), exe.data(), len);
        return static_cast<i32>(len);
    }
    const std::string h = host_path(p);
    if (h.empty()) return -1;
    return result(::readlink(h.c_str(), S(buf), n));
}
i32 h_ftruncate(i32 fd, i32 len) { return result(::ftruncate(fd, len)); }
i32 h_fsync(i32 fd) { return result(::fsync(fd)); }
i32 h_dup(i32 fd) { return result(::dup(fd)); }
i32 h_dup2(i32 a, i32 b) { return result(::dup2(a, b)); }
i32 h_pipe(gaddr fds) {
    int p[2];
    if (::pipe(p) != 0) return result(-1);
    mem().write<i32>(fds, p[0]);
    mem().write<i32>(fds + 4, p[1]);
    return 0;
}
i32 h_isatty(i32 fd) { return ::isatty(fd); }
void h_fcntl(Ctx& c) {
    const i32 fd = static_cast<i32>(c.u32()), cmd = static_cast<i32>(c.u32());
    const u32 arg = c.u32();
    switch (cmd) {
    case F_GETFD: case F_SETFD: case F_GETFL: case F_DUPFD: case F_DUPFD_CLOEXEC:
        return c.ret(static_cast<u32>(result(::fcntl(fd, cmd, arg))));
    case F_SETFL: return c.ret(static_cast<u32>(result(::fcntl(fd, cmd, open_flags(arg)))));
    default:
        std::fprintf(stderr, "[io] fcntl(%d, %d) unsupported\n", fd, cmd);
        set_errno(EINVAL);
        c.ret(static_cast<u32>(-1));
    }
}
void h_ioctl(Ctx& c) {
    const i32 fd = static_cast<i32>(c.u32());
    const u32 req = c.u32(), arg = c.u32();
    if (req == FIONREAD) {
        int n = 0;
        const int r = ::ioctl(fd, FIONREAD, &n);
        if (r == 0) mem().write<i32>(arg, n);
        return c.ret(static_cast<u32>(result(r)));
    }
    if (req == FIONBIO) {
        int v = mem().read<i32>(arg);
        return c.ret(static_cast<u32>(result(::ioctl(fd, FIONBIO, &v))));
    }
    set_errno(ENOTTY);
    c.ret(static_cast<u32>(-1));
}
i32 h_mkstemp(gaddr tmpl) {
    std::string h = host_path(tmpl);
    if (h.empty()) return -1;
    std::vector<char> buf(h.begin(), h.end());
    buf.push_back(0);
    const int fd = ::mkstemp(buf.data());
    if (fd < 0) return result(-1);
    std::memcpy(S(tmpl) + std::strlen(S(tmpl)) - 6, buf.data() + buf.size() - 7, 6);
    return fd;
}

// ---- directories (bionic struct dirent: d_ino u64, d_off i64, d_reclen u16, d_type u8, d_name[256]) ----
std::mutex dir_lock;
std::unordered_map<gaddr, DIR*> dirs;
gaddr h_opendir(gaddr p) {
    const std::string h = host_path(p);
    if (h.empty()) return 0;
    DIR* d = ::opendir(h.c_str());
    if (!d) { set_errno(errno); return 0; }
    const gaddr g = heap::calloc(1, 280);
    std::lock_guard l{dir_lock};
    dirs[g] = d;
    return g;
}
gaddr h_readdir(gaddr g) {
    DIR* d;
    { std::lock_guard l{dir_lock}; auto it = dirs.find(g); if (it == dirs.end()) { set_errno(EBADF); return 0; } d = it->second; }
    errno = 0;
    const dirent* e = ::readdir(d);
    if (!e) { if (errno) set_errno(errno); return 0; }
    mem().write<std::uint64_t>(g, e->d_ino);
    mem().write<std::int64_t>(g + 8, e->d_off);
    mem().write<std::uint16_t>(g + 16, 280);
    mem().write<std::uint8_t>(g + 18, e->d_type);
    std::strncpy(S(g + 19), e->d_name, 255);
    S(g + 19)[255] = 0;
    return g;
}
i32 h_closedir(gaddr g) {
    std::lock_guard l{dir_lock};
    auto it = dirs.find(g);
    if (it == dirs.end()) return -1;
    ::closedir(it->second);
    dirs.erase(it);
    heap::free(g);
    return 0;
}

// ---- stdio: guest FILE (bionic sizeof(FILE) = 84) -> host FILE* ----
constexpr u32 kFileSize = 84;
std::mutex file_lock;
std::unordered_map<gaddr, FILE*> files;
gaddr sF = 0;  // __sF[3]

FILE* host_file(gaddr g) {
    std::lock_guard l{file_lock};
    auto it = files.find(g);
    return it == files.end() ? nullptr : it->second;
}
gaddr new_file(FILE* f) {
    const gaddr g = heap::calloc(1, kFileSize);
    mem().write<std::int16_t>(g + 14, static_cast<std::int16_t>(fileno(f)));  // _file
    std::lock_guard l{file_lock};
    files[g] = f;
    return g;
}
gaddr h_fopen(gaddr path, gaddr mode) {
    std::string content;
    bool syn = false;
    const std::string h = host_path(path, &content, &syn);
    if (h.empty()) return 0;
    FILE* f = syn ? ::fdopen(open_synthetic(content), "r") : std::fopen(h.c_str(), S(mode));
    if (!f) { set_errno(errno); return 0; }
    return new_file(f);
}
gaddr h_fdopen(i32 fd, gaddr mode) {
    FILE* f = ::fdopen(fd, S(mode));
    if (!f) { set_errno(errno); return 0; }
    return new_file(f);
}
i32 h_fclose(gaddr g) {
    FILE* f = host_file(g);
    if (!f) return -1;
    if (g >= sF && g < sF + 3 * kFileSize) return 0;
    { std::lock_guard l{file_lock}; files.erase(g); }
    heap::free(g);
    return std::fclose(f);
}
u32 h_fread(gaddr buf, u32 size, u32 n, gaddr g) { FILE* f = host_file(g); return f ? static_cast<u32>(std::fread(mem().ptr(buf), size, n, f)) : 0; }
u32 h_fwrite(gaddr buf, u32 size, u32 n, gaddr g) { FILE* f = host_file(g); return f ? static_cast<u32>(std::fwrite(mem().ptr(buf), size, n, f)) : 0; }
gaddr h_fgets(gaddr buf, i32 n, gaddr g) { FILE* f = host_file(g); return f && std::fgets(S(buf), n, f) ? buf : 0; }
i32 h_fputs(gaddr s, gaddr g) { FILE* f = host_file(g); return f ? std::fputs(S(s), f) : -1; }
i32 h_fputc(i32 ch, gaddr g) { FILE* f = host_file(g); return f ? std::fputc(ch, f) : -1; }
i32 h_fgetc(gaddr g) { FILE* f = host_file(g); return f ? std::fgetc(f) : -1; }
i32 h_putchar(i32 ch) { return std::putchar(ch); }
i32 h_puts(gaddr s) { return std::puts(S(s)); }
i32 h_fflush(gaddr g) {
    if (!g) return std::fflush(nullptr);
    FILE* f = host_file(g);
    return f ? std::fflush(f) : -1;
}
i32 h_fseek(gaddr g, i32 off, i32 whence) { FILE* f = host_file(g); return f ? std::fseek(f, off, whence) : -1; }
i32 h_ftell(gaddr g) { FILE* f = host_file(g); return f ? static_cast<i32>(std::ftell(f)) : -1; }
i32 h_fileno(gaddr g) { FILE* f = host_file(g); return f ? ::fileno(f) : -1; }
i32 h_feof(gaddr g) { FILE* f = host_file(g); return f ? std::feof(f) : 1; }
i32 h_ferror(gaddr g) { FILE* f = host_file(g); return f ? std::ferror(f) : 1; }
void h_rewind(gaddr g) { if (FILE* f = host_file(g)) std::rewind(f); }
i32 h_setvbuf(gaddr, gaddr, i32, u32) { return 0; }
i32 h_ungetc(i32 ch, gaddr g) { FILE* f = host_file(g); return f ? std::ungetc(ch, f) : -1; }
// bionic fpos_t is a 32-bit offset.
i32 h_fgetpos(gaddr g, gaddr pos) { FILE* f = host_file(g); if (!f) return -1; mem().write<i32>(pos, static_cast<i32>(std::ftell(f))); return 0; }
i32 h_fsetpos(gaddr g, gaddr pos) { FILE* f = host_file(g); return f ? std::fseek(f, mem().read<i32>(pos), SEEK_SET) : -1; }
i32 h_flock(i32 fd, i32 op) { return result(::flock(fd, op)); }
void h_execl(Ctx& c) { std::fprintf(stderr, "[io] execl(%s) refused\n", S(c.u32())); set_errno(EACCES); c.ret(static_cast<u32>(-1)); }
void h_perror(gaddr s) { std::fprintf(stderr, "%s: %s\n", s ? S(s) : "", std::strerror(mem().read<i32>(GuestThread::current()->errno_addr()))); }

// ---- printf / scanf family ----
i32 put_out(FILE* f, const std::string& s) { return f ? static_cast<i32>(std::fwrite(s.data(), 1, s.size(), f)) : -1; }
i32 put_buf(gaddr buf, u32 size, const std::string& s) {
    if (size) {
        const u32 n = std::min<u32>(size - 1, static_cast<u32>(s.size()));
        std::memcpy(S(buf), s.data(), n);
        S(buf)[n] = 0;
    }
    return static_cast<i32>(s.size());
}
void h_printf(Ctx& c) { const gaddr f = c.u32(); CtxArgs a(c); c.ret(static_cast<u32>(put_out(stdout, format(S(f), a)))); }
void h_vprintf(Ctx& c) { const gaddr f = c.u32(); VaListArgs a(c.u32()); c.ret(static_cast<u32>(put_out(stdout, format(S(f), a)))); }
void h_fprintf(Ctx& c) { FILE* fp = host_file(c.u32()); const gaddr f = c.u32(); CtxArgs a(c); c.ret(static_cast<u32>(put_out(fp, format(S(f), a)))); }
void h_vfprintf(Ctx& c) { FILE* fp = host_file(c.u32()); const gaddr f = c.u32(); VaListArgs a(c.u32()); c.ret(static_cast<u32>(put_out(fp, format(S(f), a)))); }
void h_sprintf(Ctx& c) { const gaddr b = c.u32(), f = c.u32(); CtxArgs a(c); c.ret(static_cast<u32>(put_buf(b, 0x7FFF'FFFF, format(S(f), a)))); }
void h_vsprintf(Ctx& c) { const gaddr b = c.u32(), f = c.u32(); VaListArgs a(c.u32()); c.ret(static_cast<u32>(put_buf(b, 0x7FFF'FFFF, format(S(f), a)))); }
void h_snprintf(Ctx& c) { const gaddr b = c.u32(); const u32 n = c.u32(); const gaddr f = c.u32(); CtxArgs a(c); c.ret(static_cast<u32>(put_buf(b, n, format(S(f), a)))); }
void h_vsnprintf(Ctx& c) { const gaddr b = c.u32(); const u32 n = c.u32(); const gaddr f = c.u32(); VaListArgs a(c.u32()); c.ret(static_cast<u32>(put_buf(b, n, format(S(f), a)))); }
void h_vasprintf(Ctx& c) {
    const gaddr out = c.u32(), f = c.u32();
    VaListArgs a(c.u32());
    const std::string s = format(S(f), a);
    mem().write<gaddr>(out, heap::strdup(s));
    c.ret(static_cast<u32>(s.size()));
}
void h_asprintf(Ctx& c) {
    const gaddr out = c.u32(), f = c.u32();
    CtxArgs a(c);
    const std::string s = format(S(f), a);
    mem().write<gaddr>(out, heap::strdup(s));
    c.ret(static_cast<u32>(s.size()));
}
void h_sscanf(Ctx& c) { const gaddr in = c.u32(), f = c.u32(); CtxArgs a(c); c.ret(static_cast<u32>(scan(S(in), S(f), a))); }
void h_vsscanf(Ctx& c) { const gaddr in = c.u32(), f = c.u32(); VaListArgs a(c.u32()); c.ret(static_cast<u32>(scan(S(in), S(f), a))); }
void h_fscanf(Ctx& c) {
    // Line-oriented approximation: enough for /proc-style parsing.
    FILE* fp = host_file(c.u32());
    const gaddr f = c.u32();
    char line[4096];
    if (!fp || !std::fgets(line, sizeof line, fp)) return c.ret(static_cast<u32>(-1));
    CtxArgs a(c);
    c.ret(static_cast<u32>(scan(line, S(f), a)));
}

gaddr h_popen(gaddr cmd, gaddr) { std::fprintf(stderr, "[io] popen(\"%s\") refused\n", S(cmd)); set_errno(EACCES); return 0; }
i32 h_pclose(gaddr) { return -1; }
}  // namespace

void register_libc_io() {
    def("open", h_open); def("close", wrap<h_close>); def("read", wrap<h_read>); def("write", wrap<h_write>);
    def("lseek", wrap<h_lseek>); def("lseek64", wrap<h_lseek64>); def("fstat", wrap<h_fstat>); def("stat", wrap<h_stat>);
    def("lstat", wrap<h_lstat>); def("access", wrap<h_access>); def("mkdir", wrap<h_mkdir>); def("chmod", wrap<h_chmod>);
    def("rmdir", wrap<h_rmdir>); def("unlink", wrap<h_unlink>); def("remove", wrap<h_unlink>); def("truncate", wrap<h_truncate>);
    def("rename", wrap<h_rename>); def("utime", wrap<h_utime>); def("chdir", wrap<h_chdir>); def("getcwd", wrap<h_getcwd>);
    def("readlink", wrap<h_readlink>); def("ftruncate", wrap<h_ftruncate>); def("fsync", wrap<h_fsync>); def("fdatasync", wrap<h_fsync>);
    def("dup", wrap<h_dup>); def("dup2", wrap<h_dup2>); def("pipe", wrap<h_pipe>); def("isatty", wrap<h_isatty>);
    def("fcntl", h_fcntl); def("ioctl", h_ioctl); def("mkstemp", wrap<h_mkstemp>);
    def("opendir", wrap<h_opendir>); def("readdir", wrap<h_readdir>); def("closedir", wrap<h_closedir>);
    def("fopen", wrap<h_fopen>); def("fdopen", wrap<h_fdopen>); def("fclose", wrap<h_fclose>); def("fread", wrap<h_fread>);
    def("fwrite", wrap<h_fwrite>); def("fgets", wrap<h_fgets>); def("fputs", wrap<h_fputs>); def("fputc", wrap<h_fputc>);
    def("putc", wrap<h_fputc>); def("fgetc", wrap<h_fgetc>); def("getc", wrap<h_fgetc>); def("putchar", wrap<h_putchar>);
    def("puts", wrap<h_puts>); def("fflush", wrap<h_fflush>); def("fseek", wrap<h_fseek>); def("ftell", wrap<h_ftell>);
    def("fileno", wrap<h_fileno>); def("feof", wrap<h_feof>); def("ferror", wrap<h_ferror>); def("rewind", wrap<h_rewind>);
    def("setvbuf", wrap<h_setvbuf>); def("ungetc", wrap<h_ungetc>); def("fgetpos", wrap<h_fgetpos>);
    def("fsetpos", wrap<h_fsetpos>); def("flock", wrap<h_flock>); def("execl", h_execl); def("perror", wrap<h_perror>);
    def("printf", h_printf); def("vprintf", h_vprintf); def("fprintf", h_fprintf); def("vfprintf", h_vfprintf);
    def("sprintf", h_sprintf); def("vsprintf", h_vsprintf); def("snprintf", h_snprintf); def("vsnprintf", h_vsnprintf);
    def("asprintf", h_asprintf); def("vasprintf", h_vasprintf);
    def("sscanf", h_sscanf); def("vsscanf", h_vsscanf); def("fscanf", h_fscanf);
    def("popen", wrap<h_popen>); def("pclose", wrap<h_pclose>);

    sF = mem().static_alloc(3 * kFileSize);
    FILE* std_files[3] = {stdin, stdout, stderr};
    for (u32 i = 0; i < 3; ++i) {
        files[sF + i * kFileSize] = std_files[i];
        mem().write<std::int16_t>(sF + i * kFileSize + 14, static_cast<std::int16_t>(i));
    }
    def_data("__sF", sF);
}

}  // namespace hle
}  // namespace skim
