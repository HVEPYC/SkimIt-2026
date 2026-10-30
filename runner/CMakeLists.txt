cmake_minimum_required(VERSION 3.16)
project(skimrunner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
add_subdirectory(third_party/dynarmic dynarmic EXCLUDE_FROM_ALL)

# The runner core: guest memory and CPU, ELF loader, HLE libraries.
add_library(skimcore STATIC
  src/guest/memory.cpp
  src/guest/cpu.cpp
  src/loader/elf.cpp
  src/hle/registry.cpp
  src/hle/format.cpp
  src/hle/libc_mem.cpp
  src/hle/libc_io.cpp
  src/hle/libc_sys.cpp
  src/hle/pthread.cpp
  src/hle/libm.cpp
  src/hle/libz.cpp
  src/jni/bridge.cpp
  src/runtime/environment.cpp
  src/runtime/zip.cpp
)
target_include_directories(skimcore PUBLIC src)
if(ANDROID)
  target_sources(skimcore PRIVATE src/hle/gles_android.cpp src/jni/host.cpp)
  target_link_libraries(skimcore PUBLIC GLESv2 EGL log)
else()
  target_sources(skimcore PRIVATE src/hle/gles.cpp)
endif()
find_package(ZLIB REQUIRED)
target_link_libraries(skimcore PUBLIC dynarmic ZLIB::ZLIB)
target_compile_options(skimcore PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(ANDROID)
  # The library the Android app loads (M4). 16 KB page alignment for Android 15+ devices.
  add_library(skimrunner SHARED src/android/entry.cpp)
  target_link_libraries(skimrunner PRIVATE skimcore)
  target_link_options(skimrunner PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined)
  add_library(mono_stub SHARED src/android/mono_stub.cpp)
  set_target_properties(mono_stub PROPERTIES OUTPUT_NAME mono)
  target_link_options(mono_stub PRIVATE -Wl,-z,max-page-size=16384)
endif()

# Command-line driver: loads the original libraries from an extracted APK (desktop, or adb shell).
add_executable(skimrun src/main.cpp)
target_link_libraries(skimrun PRIVATE skimcore)

enable_testing()
add_executable(skimtest tests/core_test.cpp)
target_link_libraries(skimtest PRIVATE skimcore)
add_test(NAME core COMMAND skimtest)
