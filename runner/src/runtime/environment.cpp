#include "runtime/environment.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "guest/cpu.h"
#include "hle/vfs.h"
#include "loader/elf.h"

namespace skim::runtime {

namespace fs = std::filesystem;

namespace {
// A dual-core Cortex-A9 class CPU (VFPv3 + NEON), typical of the phones the game was made for.
const char* kGenericCpuinfo =
    "Processor\t: ARMv7 Processor rev 10 (v7l)\nprocessor\t: 0\nBogoMIPS\t: 1199.30\n\nprocessor\t: 1\nBogoMIPS\t: 1199.30\n\n"
    "Features\t: swp half thumb fastmult vfp edsp thumbee neon vfpv3 tls\nCPU implementer\t: 0x41\nCPU architecture: 7\n"
    "CPU variant\t: 0x2\nCPU part\t: 0xc09\nCPU revision\t: 10\n\nHardware\t: Generic ARMv7\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
}  // namespace

void setup(const Layout& l) {
    const fs::path priv = l.private_host;
    fs::create_directories(priv / "mono-root/lib/mono");
    fs::create_directories(priv / "mono-etc");
    fs::create_directories(l.data_host);
    // Mono's directories; corlib in <root>/mono/2.0 (the game's Managed folder).
    vfs::mount("/mono", (priv / "mono-root").string());
    vfs::mount("/mono/lib/mono/2.0", (fs::path(l.assets_host) / "bin/Data/Managed").string());
    vfs::mount("/mono/etc", (priv / "mono-etc").string());
    vfs::mount("/dev/null", "/dev/null");
    vfs::mount(l.data_guest, l.data_host);
    vfs::mount("/assets", l.assets_host);
    // The APK itself (Unity reads its data files from the zip), and "<apk>/assets/..." which Unity
    // gives Mono as the assembly directory: served from the extracted assets.
    if (!l.apk_host.empty()) vfs::mount(l.apk_guest, l.apk_host);
    vfs::mount(l.apk_guest + "/assets", l.assets_host);

    vfs::add_file("/proc/cpuinfo", l.cpuinfo.empty() ? kGenericCpuinfo : l.cpuinfo);
    vfs::add_file("/sys/devices/system/cpu/present", "0-1\n");
    vfs::add_file("/sys/devices/system/cpu/possible", "0-1\n");
    // Boehm GC counts the cpuN lines and rejects files shorter than 100 bytes.
    vfs::add_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0 0 0\ncpu1 0 0 0 0 0 0 0 0 0 0\nintr 0\nctxt 0\nbtime 0\nprocesses 1\nprocs_running 1\nprocs_blocked 0\n");
    // /proc/self/maps from the guest's own mappings: libraries by segment, the reading thread's
    // stack, the rest anonymous.
    vfs::add_generated("/proc/self/maps", [] {
        std::string out;
        char line[256];
        auto emit = [&](gaddr s, gaddr e, const char* perms, const std::string& name) {
            std::snprintf(line, sizeof line, "%08x-%08x %s 00000000 00:00 0          %s\n", s, e, perms, name.c_str());
            out += line;
        };
        std::vector<std::pair<gaddr, gaddr>> named;
        for (const auto& lib : loader::all())
            for (const auto& seg : lib->segments) {
                emit(seg.start, seg.end, seg.write ? "rw-p" : (seg.exec ? "r-xp" : "r--p"), "/data/app-lib/flippinpixels.skim-1/" + lib->name);
                named.emplace_back(seg.start, seg.end);
            }
        const GuestThread* t = GuestThread::current();
        for (auto [start, size] : Memory::get().mapped_ranges()) {
            const gaddr end = start + size;
            if (start < layout::kMmap || std::any_of(named.begin(), named.end(), [&](auto r) { return start < r.second && end > r.first; })) continue;
            emit(start, end, "rw-p", t && start <= t->stack_lo() && end >= t->stack_hi() ? "[stack]" : "");
        }
        return out;
    });
    vfs::add_generated("/proc/self/stat", [] {
        const GuestThread* t = GuestThread::current();
        char buf[512];
        std::snprintf(buf, sizeof buf, "%d (app_process) R 1 %d %d 0 -1 4202752 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 4294967295 "
                      "32768 32768 %u 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n", getpid(), getpid(), getpid(), t ? t->stack_hi() - 16 : 0);
        return std::string(buf);
    });
}

}  // namespace skim::runtime
