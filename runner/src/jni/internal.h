// Shared by the fake Java world (bridge.cpp) and the Android host bridge (host.cpp).
#pragma once

#include <string>
#include <vector>

#include "hle/registry.h"
#include "jni/bridge.h"

namespace skim::jni::detail {
// How a Call*Method passes its Java arguments: varargs, a va_list or a jvalue array.
enum class Form { Dots, VaList, Array };
// Reads the arguments described by method signature `sig`. Floats come back as their 32-bit
// pattern, longs and doubles as 64 bits, everything else (including object handles) as 32 bits.
std::vector<JValue> read_args(const std::string& sig, Form form, Ctx& c);
std::uint32_t elem_size(char e);  // JNI type char -> element size in guest memory
// Builds the guest JavaVM / JNIEnv tables from the JNIEnv::* / JavaVM::* HLE functions.
void build_guest_tables(gaddr& vm, gaddr& env);
void register_host_env();  // Android: JNIEnv::* / JavaVM::* forwarding to the real JVM (host.cpp)
}  // namespace skim::jni::detail
