#include "jni/bridge.h"
#include "jni/internal.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "hle/format.h"
#include "hle/registry.h"

namespace skim::jni {

namespace {
using u32 = std::uint32_t;
std::recursive_mutex lock;  // the fake Java world is shared by all guest threads
}  // namespace

namespace detail {
std::uint32_t elem_size(char e) {
    switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;  // I, F, and object handles
    }
}
}  // namespace detail
using detail::elem_size;
using detail::Form;
using detail::read_args;

FakeJava& java() {
    static FakeJava j;
    return j;
}

// ---- fake Java world ----
std::uint32_t FakeJava::add(Obj o) {
    objs_.push_back(std::move(o));
    return static_cast<u32>(objs_.size() - 1);
}
std::uint32_t FakeJava::find_class(const std::string& name) {
    std::lock_guard g{lock};
    auto& h = classes_[name];
    if (!h) h = add({"java/lang/Class", name});
    return h;
}
std::uint32_t FakeJava::new_object(const std::string& cls) { std::lock_guard g{lock}; return add({cls}); }
std::uint32_t FakeJava::new_string(const std::string& s) { std::lock_guard g{lock}; return add({"java/lang/String", s}); }
std::uint32_t FakeJava::new_array(char elem, std::uint32_t length) {
    std::lock_guard g{lock};
    const gaddr data = heap::calloc(length ? length : 1, elem_size(elem));
    return add({std::string("[") + elem, {}, elem, length, data});
}
std::string FakeJava::class_of(std::uint32_t h) const {
    std::lock_guard g{lock};
    return h < objs_.size() ? objs_[h].cls : "";
}
const std::string* FakeJava::string_value(std::uint32_t h) const {
    std::lock_guard g{lock};
    return h && h < objs_.size() && objs_[h].cls == "java/lang/String" ? &objs_[h].str : nullptr;
}
gaddr FakeJava::array_data(std::uint32_t h) const { std::lock_guard g{lock}; return h < objs_.size() ? objs_[h].data : 0; }
std::uint32_t FakeJava::array_length(std::uint32_t h) const { std::lock_guard g{lock}; return h < objs_.size() ? objs_[h].length : 0; }
std::uint32_t& FakeJava::object_element(std::uint32_t array, std::uint32_t i) {
    return *Memory::get().ptr<u32>(array_data(array) + 4 * i);
}

std::uint32_t FakeJava::member_id(const std::string& cls, const std::string& name, const std::string& sig, bool is_static, bool field) {
    std::lock_guard g{lock};
    const std::string key = cls + "." + name + (field ? ":" : "") + sig;
    requested.insert(std::string(is_static ? "static " : "") + (field ? "field " : "") + key);
    auto& id = member_ids_[key];
    if (!id) {
        members_.push_back({cls, name, sig, is_static, field});
        id = static_cast<u32>(members_.size() - 1);
    }
    return id;
}
const std::string& FakeJava::signature(std::uint32_t id) const { std::lock_guard g{lock}; return members_.at(id).sig; }
char FakeJava::return_type(std::uint32_t id) const {
    std::lock_guard g{lock};
    const std::string& sig = members_.at(id).sig;
    const auto p = sig.rfind(')');
    const char c = sig[p == std::string::npos ? 0 : p + 1];
    return c == '[' ? 'L' : c;
}
JValue FakeJava::invoke(std::uint32_t id, std::uint32_t self, const std::vector<JValue>& args) {
    Method m;
    std::string key;
    {
        std::lock_guard g{lock};
        const Member& mem = members_.at(id);
        key = mem.cls + "." + mem.name + (mem.field ? ":" : "") + mem.sig;
        auto it = methods_.find(key);
        if (it == methods_.end()) {
            if (missing.insert(key).second) std::fprintf(stderr, "[java] no fake for %s -> 0/null\n", key.c_str());
            return 0;
        }
        m = it->second;
    }
    return m(self, args);
}

// ---- JNIEnv / JavaVM functions ----
namespace {
Memory& M() { return Memory::get(); }
std::string S(gaddr a) { return a ? std::string(M().ptr<char>(a)) : std::string(); }
gaddr g_vm = 0, g_env = 0;

}  // namespace

std::vector<JValue> detail::read_args(const std::string& sig, Form form, Ctx& c) {
    std::vector<JValue> out;
    hle::CtxArgs dots(c);
    gaddr src = form == Form::Dots ? 0 : c.u32();
    hle::VaListArgs va(src);
    hle::ArgSource& a = form == Form::VaList ? static_cast<hle::ArgSource&>(va) : dots;
    for (std::size_t i = 1; i < sig.size() && sig[i] != ')'; ++i) {
        char t = sig[i];
        if (t == '[') { while (sig[i] == '[') ++i; t = 'L'; if (sig[i] != 'L') goto value; }
        if (sig[i] == 'L') { while (sig[i] != ';') ++i; t = 'L'; }
    value:
        if (form == Form::Array) {
            const JValue v = M().read<std::uint64_t>(src);
            src += 8;
            out.push_back(elem_size(t) == 8 ? v : v & 0xFFFF'FFFF);
        } else if (t == 'J' || t == 'D') {
            out.push_back(a.u64());
        } else if (t == 'F') {  // promoted to double through varargs
            const std::uint64_t bits = a.u64();
            double d; std::memcpy(&d, &bits, 8);
            const float f = static_cast<float>(d);
            u32 fb; std::memcpy(&fb, &f, 4);
            out.push_back(fb);
        } else {
            out.push_back(a.u32());
        }
    }
    return out;
}

namespace {
void put_result(Ctx& c, char ret, JValue v) {
    if (ret == 'V') return;
    if (ret == 'J' || ret == 'D') c.ret64(v);
    else c.ret(static_cast<u32>(v));
}

enum class Kind { Virtual, Nonvirtual, Static, Constructor };
template<Kind K, Form F>
void call_method(Ctx& c) {
    c.u32();  // env
    const u32 self = c.u32();
    if constexpr (K == Kind::Nonvirtual) c.u32();  // clazz
    const u32 mid = c.u32();
    FakeJava& j = java();
    const auto args = read_args(j.signature(mid), F, c);
    if constexpr (K == Kind::Constructor) {  // self is the class handle
        const u32 obj = j.new_object(j.find_class_name(self));
        j.invoke(mid, obj, args);
        c.ret(obj);
    } else {
        put_result(c, j.return_type(mid), j.invoke(mid, self, args));
    }
}

template<bool Static>
void get_field(Ctx& c) {
    c.u32();
    const u32 self = c.u32(), fid = c.u32();
    put_result(c, java().return_type(fid), java().invoke(fid, Static ? 0 : self, {}));
}
void set_field(Ctx& c) {
    c.u32();
    c.u32();
    const u32 fid = c.u32();
    std::fprintf(stderr, "[java] field set ignored: %s\n", java().signature(fid).c_str());
}

#define ENV_FN(name) void env_##name(Ctx& c)
ENV_FN(GetVersion) { c.ret(0x00010006); }
ENV_FN(FindClass) { c.u32(); c.ret(java().find_class(S(c.u32()))); }
ENV_FN(Zero) { c.ret(0); }
ENV_FN(Identity) { c.u32(); c.ret(c.u32()); }  // NewGlobalRef, NewLocalRef, PopLocalFrame(result)
ENV_FN(One) { c.ret(1); }
ENV_FN(IsSameObject) { c.u32(); const u32 a = c.u32(), b = c.u32(); c.ret(a == b); }
ENV_FN(GetObjectClass) { c.u32(); c.ret(java().find_class(java().class_of(c.u32()))); }
ENV_FN(GetSuperclass) { c.ret(java().find_class("java/lang/Object")); }
template<bool Static, bool Field>
void get_member_id(Ctx& c) {
    c.u32();
    const u32 cls = c.u32();
    const std::string name = S(c.u32()), sig = S(c.u32());
    c.ret(java().member_id(java().find_class_name(cls), name, sig, Static, Field));
}
ENV_FN(NewStringUTF) { c.u32(); const gaddr s = c.u32(); c.ret(s ? java().new_string(S(s)) : 0); }
ENV_FN(GetStringUTFChars) {
    c.u32();
    const u32 str = c.u32(), is_copy = c.u32();
    const std::string* v = java().string_value(str);
    if (is_copy) M().write<std::uint8_t>(is_copy, 1);
    c.ret(heap::strdup(v ? *v : ""));
}
ENV_FN(ReleaseStringUTFChars) { c.u32(); c.u32(); heap::free(c.u32()); }
ENV_FN(GetStringUTFLength) { c.u32(); const std::string* v = java().string_value(c.u32()); c.ret(v ? static_cast<u32>(v->size()) : 0); }
ENV_FN(GetStringUTFRegion) {
    c.u32();
    const std::string* v = java().string_value(c.u32());
    const u32 start = c.u32(), len = c.u32();
    const gaddr buf = c.u32();
    const std::string sub = v ? v->substr(start, len) : "";
    std::memcpy(M().ptr(buf), sub.c_str(), sub.size() + 1);
}
ENV_FN(GetArrayLength) { c.u32(); c.ret(java().array_length(c.u32())); }
template<char E>
void new_array(Ctx& c) { c.u32(); c.ret(java().new_array(E, c.u32())); }
ENV_FN(NewObjectArray) {
    c.u32();
    const u32 n = c.u32();
    c.u32();
    const u32 init = c.u32();
    const u32 arr = java().new_array('L', n);
    for (u32 i = 0; i < n; ++i) java().object_element(arr, i) = init;
    c.ret(arr);
}
ENV_FN(GetObjectArrayElement) { c.u32(); const u32 a = c.u32(), i = c.u32(); c.ret(java().object_element(a, i)); }
ENV_FN(SetObjectArrayElement) { c.u32(); const u32 a = c.u32(), i = c.u32(); java().object_element(a, i) = c.u32(); }
ENV_FN(GetArrayElements) {
    c.u32();
    const u32 a = c.u32(), is_copy = c.u32();
    if (is_copy) M().write<std::uint8_t>(is_copy, 0);
    c.ret(java().array_data(a));
}
ENV_FN(ReleaseArrayElements) {}
template<char E, bool Set>
void array_region(Ctx& c) {
    c.u32();
    const u32 a = c.u32(), start = c.u32(), len = c.u32();
    const gaddr buf = c.u32();
    const gaddr data = java().array_data(a) + start * elem_size(E);
    if (Set) std::memmove(M().ptr(data), M().ptr(buf), len * elem_size(E));
    else std::memmove(M().ptr(buf), M().ptr(data), len * elem_size(E));
}
ENV_FN(RegisterNatives) {
    c.u32();
    const u32 cls = c.u32();
    const gaddr methods = c.u32();
    const u32 n = c.u32();
    const std::string name = java().find_class_name(cls);
    for (u32 i = 0; i < n; ++i) {
        const gaddr m = methods + 12 * i;
        java().natives.push_back({name, S(M().read<gaddr>(m)), S(M().read<gaddr>(m + 4)), M().read<gaddr>(m + 8)});
    }
    c.ret(0);
}
ENV_FN(GetJavaVM) { c.u32(); M().write<gaddr>(c.u32(), g_vm); c.ret(0); }
ENV_FN(Throw) { std::fprintf(stderr, "[java] guest threw a Java exception (ignored)\n"); c.ret(0); }
ENV_FN(ThrowNew) { c.u32(); c.u32(); std::fprintf(stderr, "[java] ThrowNew: %s (ignored)\n", S(c.u32()).c_str()); c.ret(0); }
ENV_FN(FatalError) { c.u32(); c.t.fatal("JNI FatalError: %s", S(c.u32()).c_str()); }
ENV_FN(NoOp) {}

// JavaVM
ENV_FN(VmGetEnv) { c.u32(); M().write<gaddr>(c.u32(), g_env); c.ret(0); }
ENV_FN(VmAttach) { c.u32(); M().write<gaddr>(c.u32(), g_env); c.ret(0); }

void def_env(const std::string& name, HleFn fn) { hle::def(("JNIEnv::" + name).c_str(), fn); }
}  // namespace

std::string FakeJava::find_class_name(std::uint32_t h) const {
    std::lock_guard g{lock};
    return h < objs_.size() ? (objs_[h].cls == "java/lang/Class" ? objs_[h].str : objs_[h].cls) : "";
}

namespace {
void register_env() {
    static const char* types[][2] = {{"Object", "L"}, {"Boolean", "Z"}, {"Byte", "B"}, {"Char", "C"}, {"Short", "S"},
                                     {"Int", "I"}, {"Long", "J"}, {"Float", "F"}, {"Double", "D"}, {"Void", "V"}};
    for (auto& t : types) {
        const std::string T = t[0];
        def_env("Call" + T + "Method", call_method<Kind::Virtual, Form::Dots>);
        def_env("Call" + T + "MethodV", call_method<Kind::Virtual, Form::VaList>);
        def_env("Call" + T + "MethodA", call_method<Kind::Virtual, Form::Array>);
        def_env("CallNonvirtual" + T + "Method", call_method<Kind::Nonvirtual, Form::Dots>);
        def_env("CallNonvirtual" + T + "MethodV", call_method<Kind::Nonvirtual, Form::VaList>);
        def_env("CallNonvirtual" + T + "MethodA", call_method<Kind::Nonvirtual, Form::Array>);
        def_env("CallStatic" + T + "Method", call_method<Kind::Static, Form::Dots>);
        def_env("CallStatic" + T + "MethodV", call_method<Kind::Static, Form::VaList>);
        def_env("CallStatic" + T + "MethodA", call_method<Kind::Static, Form::Array>);
        if (T != "Void") {
            def_env("Get" + T + "Field", get_field<false>);
            def_env("GetStatic" + T + "Field", get_field<true>);
            def_env("Set" + T + "Field", set_field);
            def_env("SetStatic" + T + "Field", set_field);
        }
    }
    def_env("NewObject", call_method<Kind::Constructor, Form::Dots>);
    def_env("NewObjectV", call_method<Kind::Constructor, Form::VaList>);
    def_env("NewObjectA", call_method<Kind::Constructor, Form::Array>);
    def_env("GetVersion", env_GetVersion); def_env("FindClass", env_FindClass);
    for (const char* n : {"ExceptionOccurred", "ExceptionCheck", "PushLocalFrame", "EnsureLocalCapacity", "MonitorEnter", "MonitorExit", "UnregisterNatives"})
        def_env(n, env_Zero);
    for (const char* n : {"ExceptionDescribe", "ExceptionClear", "DeleteGlobalRef", "DeleteLocalRef", "DeleteWeakGlobalRef"}) def_env(n, env_NoOp);
    for (const char* n : {"NewGlobalRef", "NewLocalRef", "PopLocalFrame", "NewWeakGlobalRef"}) def_env(n, env_Identity);
    def_env("IsInstanceOf", env_One); def_env("IsAssignableFrom", env_One); def_env("GetObjectRefType", env_One);
    def_env("IsSameObject", env_IsSameObject); def_env("GetObjectClass", env_GetObjectClass); def_env("GetSuperclass", env_GetSuperclass);
    def_env("AllocObject", env_GetObjectClass);
    def_env("GetMethodID", get_member_id<false, false>); def_env("GetStaticMethodID", get_member_id<true, false>);
    def_env("GetFieldID", get_member_id<false, true>); def_env("GetStaticFieldID", get_member_id<true, true>);
    def_env("NewStringUTF", env_NewStringUTF); def_env("GetStringUTFChars", env_GetStringUTFChars);
    def_env("ReleaseStringUTFChars", env_ReleaseStringUTFChars); def_env("GetStringUTFLength", env_GetStringUTFLength);
    def_env("GetStringLength", env_GetStringUTFLength); def_env("GetStringUTFRegion", env_GetStringUTFRegion);
    def_env("GetArrayLength", env_GetArrayLength); def_env("NewObjectArray", env_NewObjectArray);
    def_env("GetObjectArrayElement", env_GetObjectArrayElement); def_env("SetObjectArrayElement", env_SetObjectArrayElement);
#define ARR(T, E)                                                                   \
    def_env("New" T "Array", new_array<E>);                                         \
    def_env("Get" T "ArrayElements", env_GetArrayElements);                         \
    def_env("Release" T "ArrayElements", env_ReleaseArrayElements);                 \
    def_env("Get" T "ArrayRegion", array_region<E, false>);                         \
    def_env("Set" T "ArrayRegion", array_region<E, true>);
    ARR("Boolean", 'Z') ARR("Byte", 'B') ARR("Char", 'C') ARR("Short", 'S') ARR("Int", 'I') ARR("Long", 'J') ARR("Float", 'F') ARR("Double", 'D')
#undef ARR
    def_env("GetPrimitiveArrayCritical", env_GetArrayElements); def_env("ReleasePrimitiveArrayCritical", env_ReleaseArrayElements);
    def_env("RegisterNatives", env_RegisterNatives); def_env("GetJavaVM", env_GetJavaVM);
    def_env("Throw", env_Throw); def_env("ThrowNew", env_ThrowNew); def_env("FatalError", env_FatalError);
    hle::def("JavaVM::GetEnv", env_VmGetEnv); hle::def("JavaVM::AttachCurrentThread", env_VmAttach);
    hle::def("JavaVM::AttachCurrentThreadAsDaemon", env_VmAttach); hle::def("JavaVM::DetachCurrentThread", env_Zero);
    hle::def("JavaVM::DestroyJavaVM", env_Zero);
}
}  // namespace

void detail::build_guest_tables(gaddr& vm_out, gaddr& env_out) {
    Memory& m = M();
    const gaddr env_table = m.static_alloc(4 * 240), vm_table = m.static_alloc(4 * 10);
#define JNI_ENV_SLOT(i, name) m.write<gaddr>(env_table + 4 * (i), hle::function_thunk("JNIEnv::" #name));
#define JNI_VM_SLOT(i, name) m.write<gaddr>(vm_table + 4 * (i), hle::function_thunk("JavaVM::" #name));
#include "jni/jni_table.inc"
#undef JNI_ENV_SLOT
#undef JNI_VM_SLOT
    env_out = m.static_alloc(4);
    m.write<gaddr>(env_out, env_table);
    vm_out = m.static_alloc(4);
    m.write<gaddr>(vm_out, vm_table);
}

static void build_tables() {
    if (g_env) return;
    register_env();
    detail::build_guest_tables(g_vm, g_env);
}

GuestThread& thread_guest() {
    thread_local std::unique_ptr<GuestThread> own;
    if (!GuestThread::current()) own = std::make_unique<GuestThread>();
    return *GuestThread::current();
}

gaddr vm() { build_tables(); return g_vm; }
gaddr env() { build_tables(); return g_env; }

std::uint64_t call_native(const std::string& cls, const std::string& name, std::uint32_t self, std::initializer_list<std::uint32_t> args) {
    for (const Native& n : java().natives)
        if (n.cls == cls && n.name == name) {
            std::vector<std::uint32_t> a{env(), self};
            a.insert(a.end(), args);
            return GuestThread::current()->call(n.fn, a);
        }
    std::fprintf(stderr, "[jni] native %s.%s not registered\n", cls.c_str(), name.c_str());
    return 0;
}

}  // namespace skim::jni
