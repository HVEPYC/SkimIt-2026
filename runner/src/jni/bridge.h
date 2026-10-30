// JNI bridge: guest-side JavaVM / JNIEnv tables whose slots are HLE thunks.
//
// The Java side is behind JavaHost. On the desktop it is a "fake Java" world (FakeJava): classes,
// strings, arrays and objects are small integer handles, and method calls go to registered host
// lambdas; anything else is logged once and returns 0/null. The log of what Unity asks for is the
// Java surface the Android build (M4) must provide.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "guest/cpu.h"
#include "guest/memory.h"

namespace skim::jni {

// A Java value as passed through JNI (jvalue): raw 64 bits; objects are handles.
using JValue = std::uint64_t;

struct Native {
    std::string cls, name, sig;
    gaddr fn;
};

class FakeJava {
public:
    using Method = std::function<JValue(std::uint32_t self, const std::vector<JValue>& args)>;

    // Handles (0 = null).
    std::uint32_t find_class(const std::string& name);
    std::uint32_t new_object(const std::string& cls);
    std::uint32_t new_string(const std::string& s);
    std::uint32_t new_array(char elem, std::uint32_t length);  // elem: JNI type char ('B', 'I', 'L', ...)
    std::string class_of(std::uint32_t h) const;          // the object's class ("java/lang/Class" for classes)
    std::string find_class_name(std::uint32_t h) const;   // for a class handle: its name; else the object's class
    const std::string* string_value(std::uint32_t h) const;
    gaddr array_data(std::uint32_t h) const;
    std::uint32_t array_length(std::uint32_t h) const;
    std::uint32_t& object_element(std::uint32_t array, std::uint32_t i);

    std::uint32_t member_id(const std::string& cls, const std::string& name, const std::string& sig, bool is_static, bool field);
    JValue invoke(std::uint32_t id, std::uint32_t self, const std::vector<JValue>& args);
    char return_type(std::uint32_t id) const;  // JNI type char of the method's return / field type
    const std::string& signature(std::uint32_t id) const;

    // Host implementations of Java methods, keyed "pkg/Class.name(sig)".
    void define(const std::string& key, Method m) { methods_[key] = std::move(m); }

    std::vector<Native> natives;
    std::set<std::string> requested;  // every member Unity looked up: "static? Class.name sig"
    std::set<std::string> missing;    // members called without a host implementation

private:
    struct Obj { std::string cls; std::string str; char elem = 0; std::uint32_t length = 0; gaddr data = 0; };
    struct Member { std::string cls, name, sig; bool is_static, field; };
    std::uint32_t add(Obj o);
    std::vector<Obj> objs_{Obj{}};
    std::vector<Member> members_{Member{}};
    std::map<std::string, std::uint32_t> classes_;
    std::map<std::string, std::uint32_t> member_ids_;
    std::map<std::string, Method> methods_;
};

FakeJava& java();

// Builds the guest tables (once); returns the guest JavaVM* and JNIEnv*.
gaddr vm();
gaddr env();

// Calls a registered native with (env, this-or-class, args...).
std::uint64_t call_native(const std::string& cls, const std::string& name, std::uint32_t self, std::initializer_list<std::uint32_t> args);

// The GuestThread of the calling host thread, created on first use (Java threads that call natives).
GuestThread& thread_guest();

#ifdef __ANDROID__
// Android: guest JNI goes to the real JVM instead of the fake Java world. Call after hle::init();
// returns the guest JavaVM* / JNIEnv* to pass to the guest's JNI_OnLoad.
void use_host_java(struct _JavaVM* vm, gaddr* guest_vm, gaddr* guest_env);
#endif

}  // namespace skim::jni
