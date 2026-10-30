// The guest's view of the device: VFS mounts and generated /proc files. Shared by the desktop
// driver (skimrun) and the Android entry point.
#pragma once

#include <string>

namespace skim::runtime {

struct Layout {
    std::string apk_guest;    // APK path as Unity sees it (the nativeFile argument)
    std::string apk_host;     // the APK file on the host ("" if there is none)
    std::string assets_host;  // extracted assets/ (bin/Data/Managed and libs/armeabi-v7a at least)
    std::string data_guest;   // the app's data directory as the guest sees it
    std::string data_host;    // where it lives on the host
    std::string private_host; // runner-private host directory (Mono's dirs)
    std::string cpuinfo;      // /proc/cpuinfo ("" = a generic ARMv7 dual core)
};

void setup(const Layout& l);

}  // namespace skim::runtime
