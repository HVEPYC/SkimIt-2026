// Registration entry points of the HLE modules.
#pragma once

namespace skim::hle {
void register_libc_mem();     // malloc family, string, ctype, conversions, sorting
void register_libc_io();      // files (with path translation), stdio, printf/scanf, directories
void register_libc_sys();     // time, process, signals, mmap, syscall, dl*, liblog, network (always fails)
void register_pthread();      // threads, mutexes, conditions, keys, semaphores
void register_libm();         // math
void register_libz();         // zlib (host zlib) and SHA1
void register_gles();         // desktop: headless stand-in (gles.cpp); Android: real GLES (gles_android.cpp)

inline void register_all() {
    register_libc_mem();
    register_libc_io();
    register_libc_sys();
    register_pthread();
    register_libm();
    register_libz();
    register_gles();
}
}  // namespace skim::hle
