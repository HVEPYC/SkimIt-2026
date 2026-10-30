// Android host bridge: the guest's JNIEnv / JavaVM functions forward to the real JVM (ART).
//
// - Java references cross into the guest as 32-bit handles. Each handle owns a host global ref.
//   Handles created during a host->guest native call are "local": they're released when the call
//   returns, like JNI local refs. Outside native calls (threads the guest attached itself), locals
//   live until the guest deletes them.
// - Method and field IDs become small indices that also remember the signature, which the
//   Call*/Get*/Set* functions need to decode guest arguments and results.
// - Primitive array elements, strings and direct buffers are copied into guest memory (the
//   guest can only address its 4 GiB arena) and copied back on release / when the native returns.
// - Guest natives are registered with the real JNIEnv through generated trampolines
//   (natives.inc, scripts/gen_natives.py), one set per signature.
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jni/internal.h"

namespace skim::jni {

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using detail::Form;

JavaVM* host_vm = nullptr;
gaddr g_vm = 0, g_env = 0;
Memory& M() { return Memory::get(); }
std::string S(gaddr a) { return a ? std::string(M().ptr<char>(a)) : std::string(); }

thread_local JNIEnv* t_env = nullptr;
JNIEnv* E() {
    if (!t_env && host_vm->GetEnv(reinterpret_cast<void**>(&t_env), JNI_VERSION_1_6) != JNI_OK) {
        host_vm->AttachCurrentThread(&t_env, nullptr);
    }
    return t_env;
}

// ---- handles ----
std::mutex handle_lock;
std::vector<jobject> objs{nullptr};
std::vector<u32> free_handles;
thread_local std::vector<std::vector<u32>> t_frames;  // local handles per native call / PushLocalFrame

u32 add_handle(jobject global) {
    std::lock_guard g{handle_lock};
    if (!free_handles.empty()) {
        const u32 h = free_handles.back();
        free_handles.pop_back();
        objs[h] = global;
        return h;
    }
    objs.push_back(global);
    return static_cast<u32>(objs.size() - 1);
}
jobject obj(u32 h) {
    std::lock_guard g{handle_lock};
    return h < objs.size() ? objs[h] : nullptr;
}
void release(u32 h) {
    jobject o;
    {
        std::lock_guard g{handle_lock};
        if (!h || h >= objs.size() || !objs[h]) return;
        o = objs[h];
        objs[h] = nullptr;
        free_handles.push_back(h);
    }
    E()->DeleteGlobalRef(o);
}
// A new handle for any host reference; tracked as local unless `global`.
u32 handle_for(jobject o, bool global = false) {
    if (!o) return 0;
    const u32 h = add_handle(E()->NewGlobalRef(o));
    if (!global && !t_frames.empty()) t_frames.back().push_back(h);
    return h;
}
// For JNI results (host local refs): converted to a handle, the host local ref is dropped.
u32 take_local(jobject o) {
    const u32 h = handle_for(o);
    if (o) E()->DeleteLocalRef(o);
    return h;
}
void delete_local(u32 h) {
    if (!t_frames.empty()) {
        auto& f = t_frames.back();
        for (auto it = f.rbegin(); it != f.rend(); ++it)
            if (*it == h) { f.erase(std::next(it).base()); break; }
    }
    release(h);
}
void pop_frame(u32 keep = 0) {
    if (t_frames.empty()) return;
    std::vector<u32> f = std::move(t_frames.back());
    t_frames.pop_back();
    for (u32 h : f)
        if (h != keep) release(h);
    if (keep && !t_frames.empty()) t_frames.back().push_back(keep);
}

// ---- member IDs ----
struct Member { jmethodID m = nullptr; jfieldID f = nullptr; std::string sig; };
std::mutex member_lock;
std::vector<Member> members{Member{}};
std::map<const void*, u32> member_index;
u32 member_id(jmethodID m, jfieldID f, const std::string& sig) {
    if (!m && !f) return 0;
    std::lock_guard g{member_lock};
    const void* key = m ? static_cast<const void*>(m) : static_cast<const void*>(f);
    auto& id = member_index[key];
    if (!id) {
        members.push_back({m, f, sig});
        id = static_cast<u32>(members.size() - 1);
    }
    return id;
}
Member member(u32 id) {
    std::lock_guard g{member_lock};
    return id < members.size() ? members[id] : Member{};
}

char ret_type(const std::string& sig) {
    const auto p = sig.rfind(')');
    const char c = p == std::string::npos ? sig[0] : sig[p + 1];
    return c == '[' ? 'L' : c;
}
std::vector<char> param_types(const std::string& sig) {
    std::vector<char> out;
    for (std::size_t i = 1; i < sig.size() && sig[i] != ')'; ++i) {
        const char t = sig[i];
        while (sig[i] == '[') ++i;
        if (sig[i] == 'L') i = sig.find(';', i);
        out.push_back(t == '[' ? 'L' : t);
    }
    return out;
}
std::vector<jvalue> to_jvalues(const std::string& sig, const std::vector<JValue>& raw) {
    const auto types = param_types(sig);
    std::vector<jvalue> out(types.size() + 1);
    for (std::size_t i = 0; i < types.size() && i < raw.size(); ++i) {
        const u64 v = raw[i];
        const u32 lo = static_cast<u32>(v);
        jvalue& j = out[i];
        switch (types[i]) {
        case 'Z': j.z = lo != 0; break;
        case 'B': j.b = static_cast<jbyte>(lo); break;
        case 'C': j.c = static_cast<jchar>(lo); break;
        case 'S': j.s = static_cast<jshort>(lo); break;
        case 'I': j.i = static_cast<jint>(lo); break;
        case 'J': j.j = static_cast<jlong>(v); break;
        case 'F': std::memcpy(&j.f, &lo, 4); break;
        case 'D': std::memcpy(&j.d, &v, 8); break;
        default: j.l = obj(lo); break;
        }
    }
    return out;
}
template<typename T> u64 bits(T v) {
    if constexpr (std::is_same_v<T, jfloat>) { u32 u; std::memcpy(&u, &v, 4); return u; }
    else if constexpr (std::is_same_v<T, jdouble>) { u64 u; std::memcpy(&u, &v, 8); return u; }
    else return static_cast<u64>(static_cast<std::int64_t>(v));
}
void put(Ctx& c, char t, u64 v) {
    if (t == 'V') return;
    if (t == 'J' || t == 'D') c.ret64(v);
    else c.ret(static_cast<u32>(v));
}

// ---- Call*Method / NewObject ----
enum class Kind { Virtual, Nonvirtual, Static, Constructor };
template<Kind K, Form F>
void call_method(Ctx& c) {
    c.u32();  // env
    const u32 self = c.u32();
    const u32 cls = K == Kind::Nonvirtual ? c.u32() : 0;
    const Member m = member(c.u32());
    const auto jv = to_jvalues(m.sig, detail::read_args(m.sig, F, c));
    JNIEnv* e = E();
    jobject o = obj(self);
    const jvalue* a = jv.data();
    if constexpr (K == Kind::Constructor) {
        c.ret(take_local(e->NewObjectA(static_cast<jclass>(o), m.m, a)));
        return;
    }
    const char t = ret_type(m.sig);
#define CALL(Type, ctype)                                                                         \
    if constexpr (K == Kind::Virtual) r = bits<ctype>(e->Call##Type##MethodA(o, m.m, a));         \
    else if constexpr (K == Kind::Static) r = bits<ctype>(e->CallStatic##Type##MethodA(static_cast<jclass>(o), m.m, a)); \
    else r = bits<ctype>(e->CallNonvirtual##Type##MethodA(o, static_cast<jclass>(obj(cls)), m.m, a));
    u64 r = 0;
    switch (t) {
    case 'V':
        if constexpr (K == Kind::Virtual) e->CallVoidMethodA(o, m.m, a);
        else if constexpr (K == Kind::Static) e->CallStaticVoidMethodA(static_cast<jclass>(o), m.m, a);
        else e->CallNonvirtualVoidMethodA(o, static_cast<jclass>(obj(cls)), m.m, a);
        break;
    case 'Z': { CALL(Boolean, jboolean) break; }
    case 'B': { CALL(Byte, jbyte) break; }
    case 'C': { CALL(Char, jchar) break; }
    case 'S': { CALL(Short, jshort) break; }
    case 'I': { CALL(Int, jint) break; }
    case 'J': { CALL(Long, jlong) break; }
    case 'F': { CALL(Float, jfloat) break; }
    case 'D': { CALL(Double, jdouble) break; }
    default: {
        jobject res;
        if constexpr (K == Kind::Virtual) res = e->CallObjectMethodA(o, m.m, a);
        else if constexpr (K == Kind::Static) res = e->CallStaticObjectMethodA(static_cast<jclass>(o), m.m, a);
        else res = e->CallNonvirtualObjectMethodA(o, static_cast<jclass>(obj(cls)), m.m, a);
        r = take_local(res);
    }
    }
#undef CALL
    put(c, t, r);
}

// ---- fields ----
template<bool Static>
void get_field(Ctx& c) {
    c.u32();
    jobject o = obj(c.u32());
    const Member m = member(c.u32());
    JNIEnv* e = E();
    const char t = m.sig[0] == '[' ? 'L' : m.sig[0];
    auto cls = static_cast<jclass>(o);
    u64 r = 0;
#define GET(Type, ctype) r = bits<ctype>(Static ? e->GetStatic##Type##Field(cls, m.f) : e->Get##Type##Field(o, m.f)); break;
    switch (t) {
    case 'Z': GET(Boolean, jboolean) case 'B': GET(Byte, jbyte) case 'C': GET(Char, jchar) case 'S': GET(Short, jshort)
    case 'I': GET(Int, jint) case 'J': GET(Long, jlong) case 'F': GET(Float, jfloat) case 'D': GET(Double, jdouble)
    default: r = take_local(Static ? e->GetStaticObjectField(cls, m.f) : e->GetObjectField(o, m.f));
    }
#undef GET
    put(c, t, r);
}
template<bool Static>
void set_field(Ctx& c) {
    c.u32();
    jobject o = obj(c.u32());
    const Member m = member(c.u32());
    JNIEnv* e = E();
    const char t = m.sig[0] == '[' ? 'L' : m.sig[0];
    auto cls = static_cast<jclass>(o);
    const u64 v = (t == 'J' || t == 'D') ? c.u64() : c.u32();
    const u32 lo = static_cast<u32>(v);
    float f; std::memcpy(&f, &lo, 4);
    double d; std::memcpy(&d, &v, 8);
#define SET(Type, val) if (Static) e->SetStatic##Type##Field(cls, m.f, val); else e->Set##Type##Field(o, m.f, val); break;
    switch (t) {
    case 'Z': SET(Boolean, static_cast<jboolean>(lo != 0)) case 'B': SET(Byte, static_cast<jbyte>(lo))
    case 'C': SET(Char, static_cast<jchar>(lo)) case 'S': SET(Short, static_cast<jshort>(lo))
    case 'I': SET(Int, static_cast<jint>(lo)) case 'J': SET(Long, static_cast<jlong>(v))
    case 'F': SET(Float, f) case 'D': SET(Double, d)
    default: SET(Object, obj(lo))
    }
#undef SET
}

template<bool Static, bool Field>
void get_member_id(Ctx& c) {
    c.u32();
    auto cls = static_cast<jclass>(obj(c.u32()));
    const std::string name = S(c.u32()), sig = S(c.u32());
    JNIEnv* e = E();
    if constexpr (Field) c.ret(member_id(nullptr, Static ? e->GetStaticFieldID(cls, name.c_str(), sig.c_str()) : e->GetFieldID(cls, name.c_str(), sig.c_str()), sig));
    else c.ret(member_id(Static ? e->GetStaticMethodID(cls, name.c_str(), sig.c_str()) : e->GetMethodID(cls, name.c_str(), sig.c_str()), nullptr, sig));
}

// ---- reflection (Unity's AndroidJavaObject finds members through java.lang.reflect) ----
std::string descriptor(JNIEnv* e, jclass c) {
    jclass cc = e->GetObjectClass(c);
    auto name_obj = static_cast<jstring>(e->CallObjectMethod(c, e->GetMethodID(cc, "getName", "()Ljava/lang/String;")));
    const bool prim = e->CallBooleanMethod(c, e->GetMethodID(cc, "isPrimitive", "()Z"));
    e->DeleteLocalRef(cc);
    const char* chars = e->GetStringUTFChars(name_obj, nullptr);
    std::string n = chars;
    e->ReleaseStringUTFChars(name_obj, chars);
    e->DeleteLocalRef(name_obj);
    if (prim) {
        static const std::map<std::string, std::string> p{{"int", "I"}, {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
                                                           {"long", "J"}, {"float", "F"}, {"double", "D"}, {"void", "V"}};
        return p.at(n);
    }
    std::replace(n.begin(), n.end(), '.', '/');
    return n[0] == '[' ? n : "L" + n + ";";
}
// JNI signature of a java.lang.reflect.Method / Constructor / Field.
std::string reflected_sig(JNIEnv* e, jobject m, bool field) {
    jclass mc = e->GetObjectClass(m);
    std::string sig;
    if (field) {
        auto t = static_cast<jclass>(e->CallObjectMethod(m, e->GetMethodID(mc, "getType", "()Ljava/lang/Class;")));
        sig = descriptor(e, t);
        e->DeleteLocalRef(t);
    } else {
        auto params = static_cast<jobjectArray>(e->CallObjectMethod(m, e->GetMethodID(mc, "getParameterTypes", "()[Ljava/lang/Class;")));
        sig = "(";
        for (jsize i = 0, n = e->GetArrayLength(params); i < n; ++i) {
            auto p = static_cast<jclass>(e->GetObjectArrayElement(params, i));
            sig += descriptor(e, p);
            e->DeleteLocalRef(p);
        }
        sig += ")";
        e->DeleteLocalRef(params);
        const jmethodID get_ret = e->GetMethodID(mc, "getReturnType", "()Ljava/lang/Class;");
        if (e->ExceptionCheck() || !get_ret) {  // a Constructor
            e->ExceptionClear();
            sig += "V";
        } else {
            auto r = static_cast<jclass>(e->CallObjectMethod(m, get_ret));
            sig += descriptor(e, r);
            e->DeleteLocalRef(r);
        }
    }
    e->DeleteLocalRef(mc);
    return sig;
}
void env_FromReflectedMethod(Ctx& c) {
    c.u32();
    jobject m = obj(c.u32());
    JNIEnv* e = E();
    c.ret(m ? member_id(e->FromReflectedMethod(m), nullptr, reflected_sig(e, m, false)) : 0);
}
void env_FromReflectedField(Ctx& c) {
    c.u32();
    jobject f = obj(c.u32());
    JNIEnv* e = E();
    c.ret(f ? member_id(nullptr, e->FromReflectedField(f), reflected_sig(e, f, true)) : 0);
}
void env_ToReflectedMethod(Ctx& c) {
    c.u32();
    auto cls = static_cast<jclass>(obj(c.u32()));
    const Member m = member(c.u32());
    c.ret(take_local(E()->ToReflectedMethod(cls, m.m, c.u32() != 0)));
}
void env_ToReflectedField(Ctx& c) {
    c.u32();
    auto cls = static_cast<jclass>(obj(c.u32()));
    const Member m = member(c.u32());
    c.ret(take_local(E()->ToReflectedField(cls, m.f, c.u32() != 0)));
}

// ---- strings ----
void env_NewStringUTF(Ctx& c) { c.u32(); const gaddr s = c.u32(); c.ret(s ? take_local(E()->NewStringUTF(M().ptr<char>(s))) : 0); }
void env_GetStringUTFChars(Ctx& c) {
    c.u32();
    auto s = static_cast<jstring>(obj(c.u32()));
    const gaddr is_copy = c.u32();
    if (is_copy) M().write<std::uint8_t>(is_copy, 1);
    if (!s) { c.ret(0); return; }
    const char* v = E()->GetStringUTFChars(s, nullptr);
    c.ret(heap::strdup(v));
    E()->ReleaseStringUTFChars(s, v);
}
void env_ReleaseChars(Ctx& c) { c.u32(); c.u32(); heap::free(c.u32()); }
void env_GetStringUTFLength(Ctx& c) { c.u32(); c.ret(E()->GetStringUTFLength(static_cast<jstring>(obj(c.u32())))); }
void env_GetStringLength(Ctx& c) { c.u32(); c.ret(E()->GetStringLength(static_cast<jstring>(obj(c.u32())))); }
void env_GetStringChars(Ctx& c) {
    c.u32();
    auto s = static_cast<jstring>(obj(c.u32()));
    const gaddr is_copy = c.u32();
    if (is_copy) M().write<std::uint8_t>(is_copy, 1);
    const jsize n = E()->GetStringLength(s);
    const gaddr buf = heap::malloc(2 * (n + 1));
    E()->GetStringRegion(s, 0, n, M().ptr<jchar>(buf));
    M().write<std::uint16_t>(buf + 2 * n, 0);
    c.ret(buf);
}
void env_NewString(Ctx& c) {
    c.u32();
    const gaddr chars = c.u32();
    const jsize n = c.u32();
    c.ret(take_local(E()->NewString(M().ptr<jchar>(chars), n)));
}
void env_GetStringUTFRegion(Ctx& c) {
    c.u32();
    auto s = static_cast<jstring>(obj(c.u32()));
    const jsize start = c.u32(), len = c.u32();
    E()->GetStringUTFRegion(s, start, len, M().ptr<char>(c.u32()));
}
void env_GetStringRegion(Ctx& c) {
    c.u32();
    auto s = static_cast<jstring>(obj(c.u32()));
    const jsize start = c.u32(), len = c.u32();
    E()->GetStringRegion(s, start, len, M().ptr<jchar>(c.u32()));
}

// ---- arrays ----
struct Pinned { u32 array; char elem; };
std::mutex pinned_lock;
std::map<gaddr, Pinned> pinned;

void region(JNIEnv* e, jarray a, char t, jsize start, jsize len, void* buf, bool set) {
#define R(Type, ctype, jt)                                                                                         \
    if (set) e->Set##Type##ArrayRegion(static_cast<jt>(a), start, len, static_cast<const ctype*>(buf));            \
    else e->Get##Type##ArrayRegion(static_cast<jt>(a), start, len, static_cast<ctype*>(buf));                      \
    break;
    switch (t) {
    case 'Z': R(Boolean, jboolean, jbooleanArray) case 'B': R(Byte, jbyte, jbyteArray) case 'C': R(Char, jchar, jcharArray)
    case 'S': R(Short, jshort, jshortArray) case 'I': R(Int, jint, jintArray) case 'J': R(Long, jlong, jlongArray)
    case 'F': R(Float, jfloat, jfloatArray) case 'D': R(Double, jdouble, jdoubleArray)
    }
#undef R
}
char elem_of(jarray a) {  // for GetPrimitiveArrayCritical, which doesn't say
    static const char* names[] = {"[B", "[I", "[F", "[S", "[C", "[Z", "[J", "[D"};
    JNIEnv* e = E();
    for (const char* n : names) {
        jclass k = e->FindClass(n);
        const bool is = k && e->IsInstanceOf(a, k);
        if (k) e->DeleteLocalRef(k);
        if (is) return n[1];
    }
    return 'B';
}
template<char E_>
void new_array(Ctx& c) {
    c.u32();
    const jsize n = c.u32();
    JNIEnv* e = E();
    jarray a = nullptr;
    switch (E_) {
    case 'Z': a = e->NewBooleanArray(n); break; case 'B': a = e->NewByteArray(n); break;
    case 'C': a = e->NewCharArray(n); break;    case 'S': a = e->NewShortArray(n); break;
    case 'I': a = e->NewIntArray(n); break;     case 'J': a = e->NewLongArray(n); break;
    case 'F': a = e->NewFloatArray(n); break;   case 'D': a = e->NewDoubleArray(n); break;
    }
    c.ret(take_local(a));
}
template<char E_>
void get_elements(Ctx& c) {
    c.u32();
    const u32 h = c.u32();
    const gaddr is_copy = c.u32();
    auto a = static_cast<jarray>(obj(h));
    if (!a) { c.ret(0); return; }
    const char t = E_ ? E_ : elem_of(a);
    const jsize n = E()->GetArrayLength(a);
    const gaddr buf = heap::malloc(n * detail::elem_size(t) + 1);
    region(E(), a, t, 0, n, M().ptr(buf), false);
    if (is_copy) M().write<std::uint8_t>(is_copy, 1);
    std::lock_guard g{pinned_lock};
    pinned[buf] = {h, t};
    c.ret(buf);
}
void release_elements(Ctx& c) {
    c.u32();
    c.u32();
    const gaddr buf = c.u32();
    const u32 mode = c.u32();
    Pinned p;
    {
        std::lock_guard g{pinned_lock};
        auto it = pinned.find(buf);
        if (it == pinned.end()) return;
        p = it->second;
        if (mode != JNI_COMMIT) pinned.erase(it);
    }
    auto a = static_cast<jarray>(obj(p.array));
    if (mode != JNI_ABORT && a) region(E(), a, p.elem, 0, E()->GetArrayLength(a), M().ptr(buf), true);
    if (mode != JNI_COMMIT) heap::free(buf);
}
template<char E_, bool Set>
void array_region(Ctx& c) {
    c.u32();
    auto a = static_cast<jarray>(obj(c.u32()));
    const jsize start = c.u32(), len = c.u32();
    region(E(), a, E_, start, len, M().ptr(c.u32()), Set);
}
void env_GetArrayLength(Ctx& c) { c.u32(); c.ret(E()->GetArrayLength(static_cast<jarray>(obj(c.u32())))); }
void env_NewObjectArray(Ctx& c) {
    c.u32();
    const jsize n = c.u32();
    auto cls = static_cast<jclass>(obj(c.u32()));
    c.ret(take_local(E()->NewObjectArray(n, cls, obj(c.u32()))));
}
void env_GetObjectArrayElement(Ctx& c) {
    c.u32();
    auto a = static_cast<jobjectArray>(obj(c.u32()));
    c.ret(take_local(E()->GetObjectArrayElement(a, c.u32())));
}
void env_SetObjectArrayElement(Ctx& c) {
    c.u32();
    auto a = static_cast<jobjectArray>(obj(c.u32()));
    const jsize i = c.u32();
    E()->SetObjectArrayElement(a, i, obj(c.u32()));
}

// ---- direct buffers ----
// A buffer backed by guest memory (NewDirectByteBuffer from the guest) is used in place; any other
// direct buffer gets a guest shadow, copied in now and back when the current native call returns.
struct Shadow { void* host; jlong cap; gaddr guest; };
thread_local std::vector<Shadow> t_dirty;
std::mutex shadow_lock;
std::map<void*, Shadow> shadows;
void env_NewDirectByteBuffer(Ctx& c) {
    c.u32();
    const gaddr addr = c.u32();
    const u64 cap = c.u64();
    c.ret(take_local(E()->NewDirectByteBuffer(M().ptr(addr), static_cast<jlong>(cap))));
}
void env_GetDirectBufferAddress(Ctx& c) {
    c.u32();
    jobject b = obj(c.u32());
    void* host = E()->GetDirectBufferAddress(b);
    const jlong cap = E()->GetDirectBufferCapacity(b);
    auto* base = static_cast<std::uint8_t*>(M().ptr(0));
    if (!host) { c.ret(0); return; }
    if (host >= base && static_cast<std::uint8_t*>(host) < base + (1ull << 32)) { c.ret(static_cast<u32>(static_cast<std::uint8_t*>(host) - base)); return; }
    Shadow s;
    {
        std::lock_guard g{shadow_lock};
        auto& sh = shadows[host];
        if (!sh.guest || sh.cap < cap) sh = {host, cap, heap::malloc(cap ? cap : 1)};
        s = sh;
    }
    std::memcpy(M().ptr(s.guest), host, cap);
    t_dirty.push_back(s);
    c.ret(s.guest);
}
void env_GetDirectBufferCapacity(Ctx& c) { c.u32(); c.ret64(static_cast<u64>(E()->GetDirectBufferCapacity(obj(c.u32())))); }
void sync_shadows() {
    for (const Shadow& s : t_dirty) std::memcpy(s.host, M().ptr(s.guest), s.cap);
    t_dirty.clear();
}

// ---- references, classes, exceptions ----
void env_GetVersion(Ctx& c) { c.ret(E()->GetVersion()); }
void env_FindClass(Ctx& c) { c.u32(); c.ret(take_local(E()->FindClass(S(c.u32()).c_str()))); }
void env_GetSuperclass(Ctx& c) { c.u32(); c.ret(take_local(E()->GetSuperclass(static_cast<jclass>(obj(c.u32()))))); }
void env_IsAssignableFrom(Ctx& c) { c.u32(); auto a = static_cast<jclass>(obj(c.u32())); c.ret(E()->IsAssignableFrom(a, static_cast<jclass>(obj(c.u32())))); }
void env_GetObjectClass(Ctx& c) { c.u32(); c.ret(take_local(E()->GetObjectClass(obj(c.u32())))); }
void env_AllocObject(Ctx& c) { c.u32(); c.ret(take_local(E()->AllocObject(static_cast<jclass>(obj(c.u32()))))); }
void env_IsInstanceOf(Ctx& c) { c.u32(); jobject o = obj(c.u32()); c.ret(E()->IsInstanceOf(o, static_cast<jclass>(obj(c.u32())))); }
void env_IsSameObject(Ctx& c) { c.u32(); jobject a = obj(c.u32()); c.ret(E()->IsSameObject(a, obj(c.u32()))); }
void env_NewGlobalRef(Ctx& c) { c.u32(); c.ret(handle_for(obj(c.u32()), true)); }
void env_NewLocalRef(Ctx& c) { c.u32(); c.ret(handle_for(obj(c.u32()))); }
void env_DeleteGlobalRef(Ctx& c) { c.u32(); release(c.u32()); }
void env_DeleteLocalRef(Ctx& c) { c.u32(); delete_local(c.u32()); }
void env_GetObjectRefType(Ctx& c) { c.u32(); c.ret(obj(c.u32()) ? JNIGlobalRefType : JNIInvalidRefType); }
void env_PushLocalFrame(Ctx& c) { t_frames.emplace_back(); c.ret(0); }
void env_PopLocalFrame(Ctx& c) {
    c.u32();
    const u32 keep = c.u32();
    pop_frame(keep);
    c.ret(keep);
}
void env_Zero(Ctx& c) { c.ret(0); }
void env_Throw(Ctx& c) { c.u32(); c.ret(E()->Throw(static_cast<jthrowable>(obj(c.u32())))); }
void env_ThrowNew(Ctx& c) { c.u32(); auto cls = static_cast<jclass>(obj(c.u32())); c.ret(E()->ThrowNew(cls, S(c.u32()).c_str())); }
void env_ExceptionOccurred(Ctx& c) { c.ret(take_local(E()->ExceptionOccurred())); }
void env_ExceptionDescribe(Ctx&) { E()->ExceptionDescribe(); }
void env_ExceptionClear(Ctx&) { E()->ExceptionClear(); }
void env_ExceptionCheck(Ctx& c) { c.ret(E()->ExceptionCheck()); }
void env_FatalError(Ctx& c) { c.u32(); c.t.fatal("JNI FatalError: %s", S(c.u32()).c_str()); }
void env_MonitorEnter(Ctx& c) { c.u32(); c.ret(E()->MonitorEnter(obj(c.u32()))); }
void env_MonitorExit(Ctx& c) { c.u32(); c.ret(E()->MonitorExit(obj(c.u32()))); }
void env_GetJavaVM(Ctx& c) { c.u32(); M().write<gaddr>(c.u32(), g_vm); c.ret(0); }

// ---- natives ----
gaddr slot_fn[64][8];
// Marshals one Java->guest native call (used by the generated trampolines).
class NativeCall {
public:
    NativeCall(JNIEnv* env, jobject self, int sig, int slot) : fn_(slot_fn[sig][slot]) {
        t_env = env;
        t_frames.emplace_back();
        args_ = {g_env, handle_for(self)};
    }
    ~NativeCall() {
        sync_shadows();
        pop_frame();
    }
    void i(std::int64_t v) { args_.push_back(static_cast<u32>(v)); }
    void l(jobject o) { args_.push_back(handle_for(o)); }
    void j(jlong v) { align(); args_.push_back(static_cast<u32>(v)); args_.push_back(static_cast<u32>(static_cast<u64>(v) >> 32)); }
    void f(jfloat v) { u32 u; std::memcpy(&u, &v, 4); args_.push_back(u); }
    void d(jdouble v) { u64 u; std::memcpy(&u, &v, 8); j(static_cast<jlong>(u)); }
    u64 call() { return thread_guest().call(fn_, args_); }
    template<typename T> T ret_i() { return static_cast<T>(static_cast<u32>(call())); }
    template<typename T> T ret_j() { return static_cast<T>(call()); }
    template<typename T> T ret_f() { const u32 u = static_cast<u32>(call()); T v; std::memcpy(&v, &u, 4); return v; }
    template<typename T> T ret_d() { const u64 u = call(); T v; std::memcpy(&v, &u, 8); return v; }
    template<typename T> T ret_l() { jobject o = obj(static_cast<u32>(call())); return o ? t_env->NewLocalRef(o) : nullptr; }

private:
    void align() { if (args_.size() % 2) args_.push_back(0); }  // AAPCS: 64-bit values in even register pairs
    gaddr fn_;
    std::vector<u32> args_;
};

#include "jni/natives.inc"
static_assert(std::size(kNativeSigs) <= 64);
std::mutex native_lock;
int used_slots[64];

void env_RegisterNatives(Ctx& c) {
    c.u32();
    auto cls = static_cast<jclass>(obj(c.u32()));
    const gaddr methods = c.u32();
    const u32 n = c.u32();
    jint result = 0;
    for (u32 i = 0; i < n; ++i) {
        const gaddr m = methods + 12 * i;
        const std::string name = S(M().read<gaddr>(m)), sig = S(M().read<gaddr>(m + 4));
        void* tramp = nullptr;
        {
            std::lock_guard g{native_lock};
            for (std::size_t k = 0; k < std::size(kNativeSigs); ++k)
                if (sig == kNativeSigs[k].sig && used_slots[k] < kNativeSlots) {
                    slot_fn[k][used_slots[k]] = M().read<gaddr>(m + 8);
                    tramp = kNativeSigs[k].fn[used_slots[k]++];
                    break;
                }
        }
        if (!tramp) {
            std::fprintf(stderr, "[jni] RegisterNatives: no trampoline for %s%s\n", name.c_str(), sig.c_str());
            result = -1;
            continue;
        }
        const JNINativeMethod nm{name.c_str(), sig.c_str(), tramp};
        if (E()->RegisterNatives(cls, &nm, 1) != JNI_OK) result = -1;
    }
    c.ret(static_cast<u32>(result));
}

// JavaVM
void vm_GetEnv(Ctx& c) {
    c.u32();
    const gaddr out = c.u32();
    JNIEnv* e = nullptr;
    if (host_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) { c.ret(static_cast<u32>(JNI_EDETACHED)); return; }
    t_env = e;
    M().write<gaddr>(out, g_env);
    c.ret(0);
}
void vm_Attach(Ctx& c) {
    c.u32();
    const gaddr out = c.u32();
    JNIEnv* e = nullptr;
    const jint r = host_vm->AttachCurrentThread(&e, nullptr);
    t_env = e;
    if (r == JNI_OK) M().write<gaddr>(out, g_env);
    c.ret(static_cast<u32>(r));
}
void vm_Detach(Ctx& c) {
    t_env = nullptr;
    c.ret(static_cast<u32>(host_vm->DetachCurrentThread()));
}

void def_env(const std::string& name, HleFn fn) { hle::def(("JNIEnv::" + name).c_str(), fn); }
}  // namespace

void detail::register_host_env() {
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
            def_env("Set" + T + "Field", set_field<false>);
            def_env("SetStatic" + T + "Field", set_field<true>);
        }
    }
    def_env("NewObject", call_method<Kind::Constructor, Form::Dots>);
    def_env("NewObjectV", call_method<Kind::Constructor, Form::VaList>);
    def_env("NewObjectA", call_method<Kind::Constructor, Form::Array>);
    def_env("GetMethodID", get_member_id<false, false>); def_env("GetStaticMethodID", get_member_id<true, false>);
    def_env("GetFieldID", get_member_id<false, true>); def_env("GetStaticFieldID", get_member_id<true, true>);
    def_env("FromReflectedMethod", env_FromReflectedMethod); def_env("FromReflectedField", env_FromReflectedField);
    def_env("ToReflectedMethod", env_ToReflectedMethod); def_env("ToReflectedField", env_ToReflectedField);
    def_env("GetVersion", env_GetVersion); def_env("FindClass", env_FindClass); def_env("GetSuperclass", env_GetSuperclass);
    def_env("IsAssignableFrom", env_IsAssignableFrom); def_env("GetObjectClass", env_GetObjectClass);
    def_env("AllocObject", env_AllocObject); def_env("IsInstanceOf", env_IsInstanceOf); def_env("IsSameObject", env_IsSameObject);
    def_env("NewGlobalRef", env_NewGlobalRef); def_env("NewWeakGlobalRef", env_NewGlobalRef); def_env("NewLocalRef", env_NewLocalRef);
    def_env("DeleteGlobalRef", env_DeleteGlobalRef); def_env("DeleteWeakGlobalRef", env_DeleteGlobalRef);
    def_env("DeleteLocalRef", env_DeleteLocalRef); def_env("GetObjectRefType", env_GetObjectRefType);
    def_env("PushLocalFrame", env_PushLocalFrame); def_env("PopLocalFrame", env_PopLocalFrame); def_env("EnsureLocalCapacity", env_Zero);
    def_env("Throw", env_Throw); def_env("ThrowNew", env_ThrowNew); def_env("ExceptionOccurred", env_ExceptionOccurred);
    def_env("ExceptionDescribe", env_ExceptionDescribe); def_env("ExceptionClear", env_ExceptionClear);
    def_env("ExceptionCheck", env_ExceptionCheck); def_env("FatalError", env_FatalError);
    def_env("MonitorEnter", env_MonitorEnter); def_env("MonitorExit", env_MonitorExit); def_env("GetJavaVM", env_GetJavaVM);
    def_env("NewStringUTF", env_NewStringUTF); def_env("GetStringUTFChars", env_GetStringUTFChars);
    def_env("ReleaseStringUTFChars", env_ReleaseChars); def_env("GetStringUTFLength", env_GetStringUTFLength);
    def_env("GetStringLength", env_GetStringLength); def_env("GetStringChars", env_GetStringChars);
    def_env("ReleaseStringChars", env_ReleaseChars); def_env("GetStringUTFRegion", env_GetStringUTFRegion);
    def_env("GetStringRegion", env_GetStringRegion); def_env("NewString", env_NewString);
    def_env("GetStringCritical", env_GetStringChars); def_env("ReleaseStringCritical", env_ReleaseChars);
    def_env("GetArrayLength", env_GetArrayLength); def_env("NewObjectArray", env_NewObjectArray);
    def_env("GetObjectArrayElement", env_GetObjectArrayElement); def_env("SetObjectArrayElement", env_SetObjectArrayElement);
#define ARR(T, E)                                                                   \
    def_env("New" T "Array", new_array<E>);                                         \
    def_env("Get" T "ArrayElements", get_elements<E>);                              \
    def_env("Release" T "ArrayElements", release_elements);                         \
    def_env("Get" T "ArrayRegion", array_region<E, false>);                         \
    def_env("Set" T "ArrayRegion", array_region<E, true>);
    ARR("Boolean", 'Z') ARR("Byte", 'B') ARR("Char", 'C') ARR("Short", 'S') ARR("Int", 'I') ARR("Long", 'J') ARR("Float", 'F') ARR("Double", 'D')
#undef ARR
    def_env("GetPrimitiveArrayCritical", get_elements<0>); def_env("ReleasePrimitiveArrayCritical", release_elements);
    def_env("NewDirectByteBuffer", env_NewDirectByteBuffer); def_env("GetDirectBufferAddress", env_GetDirectBufferAddress);
    def_env("GetDirectBufferCapacity", env_GetDirectBufferCapacity);
    def_env("RegisterNatives", env_RegisterNatives); def_env("UnregisterNatives", env_Zero);
    hle::def("JavaVM::GetEnv", vm_GetEnv); hle::def("JavaVM::AttachCurrentThread", vm_Attach);
    hle::def("JavaVM::AttachCurrentThreadAsDaemon", vm_Attach); hle::def("JavaVM::DetachCurrentThread", vm_Detach);
    hle::def("JavaVM::DestroyJavaVM", env_Zero);
}

void use_host_java(JavaVM* vm, gaddr* guest_vm, gaddr* guest_env) {
    host_vm = vm;
    detail::register_host_env();
    detail::build_guest_tables(g_vm, g_env);
    *guest_vm = g_vm;
    *guest_env = g_env;
}

}  // namespace skim::jni
