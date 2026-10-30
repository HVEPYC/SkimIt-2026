// skimrun: desktop driver for the runner core.
//   skimrun <extracted-apk-dir> [--mono-smoke] [--unity [--jni | --start [--frames=N]]]
//   skimrun --list-hle [apk-dir]  (names of implemented HLE functions and data objects, plus libmono exports)
// Loads the original libmono.so (and libunity.so with --unity) from the extracted APK, runs their
// constructors and, with --mono-smoke, boots Mono and invokes System.Math.Max(int, int) in JIT code.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "guest/cpu.h"
#include "hle/registry.h"
#include "hle/vfs.h"
#include "jni/bridge.h"
#include "loader/elf.h"
#include "runtime/environment.h"

using namespace skim;
namespace fs = std::filesystem;

namespace {
std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

gaddr sym(Library& lib, const char* name) {
    auto it = lib.exports.find(name);
    if (it == lib.exports.end()) { std::fprintf(stderr, "missing export %s\n", name); std::exit(2); }
    return it->second;
}

// The Java side Unity queries, answered with values of a generic 2013 Android 4.4 tablet (desktop
// runs only; on Android the real Java answers). Anything not listed is logged by the bridge.
void install_fakes() {
    jni::FakeJava& j = jni::java();
    auto str = [&j](std::string v) { return [&j, v](std::uint32_t, const std::vector<jni::JValue>&) -> jni::JValue { return j.new_string(v); }; };
    auto num = [](jni::JValue v) { return [v](std::uint32_t, const std::vector<jni::JValue>&) { return v; }; };
    j.define("android/os/Build$VERSION.RELEASE:Ljava/lang/String;", str("4.4.4"));
    j.define("android/os/Build$VERSION.SDK_INT:I", num(19));
    j.define("android/os/Build$VERSION.SDK:Ljava/lang/String;", str("19"));
    j.define("android/os/Build$VERSION.INCREMENTAL:Ljava/lang/String;", str("eng.skimport"));
    j.define("android/os/Build.ID:Ljava/lang/String;", str("KTU84P"));
    j.define("android/os/Build.MODEL:Ljava/lang/String;", str("ARMv7 tablet"));
    j.define("android/os/Build.MANUFACTURER:Ljava/lang/String;", str("generic"));
    j.define("android/os/Build.FINGERPRINT:Ljava/lang/String;", str("generic/armv7/armv7:4.4.4/KTU84P/eng.skimport:user/release-keys"));
    j.define("com/unity3d/player/UnityPlayer.getTotalMemory()I", num(1024));  // MB
    j.define("com/unity3d/player/UnityPlayer.getCPUType()Ljava/lang/String;", str("ARMv7 VFPv3 NEON"));
    j.define("android/os/Build.DEVICE:Ljava/lang/String;", str("armv7"));
    // Portrait activity on an 800x1280 panel; no joysticks, no telephony.
    j.define("com/unity3d/player/UnityPlayer.getDisplaySize()[I", [&j](std::uint32_t, const std::vector<jni::JValue>&) -> jni::JValue {
        const std::uint32_t a = j.new_array('I', 2);
        Memory::get().write<std::uint32_t>(j.array_data(a), 800);
        Memory::get().write<std::uint32_t>(j.array_data(a) + 4, 1280);
        return a;
    });
    j.define("com/unity3d/player/UnityPlayer.getConnectedJoysticks()[I", [&j](std::uint32_t, const std::vector<jni::JValue>&) -> jni::JValue { return j.new_array('I', 0); });
    j.define("com/unity3d/player/UnityPlayer.getDeviceUniqueIdentifier()Ljava/lang/String;", str("0000000000000000"));  // desktop placeholder
    for (const char* m : {"setOrientation(I)V", "setWakeLock(Z)V", "startActivityIndicator()V", "stopActivityIndicator()V", "hideSoftInput()V"})
        j.define(std::string("com/unity3d/player/UnityPlayer.") + m, num(0));
    // AndroidJavaClass resolves classes with Class.forName("a.b.C").
    j.define("java/lang/Class.forName(Ljava/lang/String;)Ljava/lang/Class;", [&j](std::uint32_t, const std::vector<jni::JValue>& a) -> jni::JValue {
        std::string n = *j.string_value(static_cast<std::uint32_t>(a.at(0)));
        std::replace(n.begin(), n.end(), '.', '/');
        return j.find_class(n);
    });
    j.define("java/util/HashMap.<init>(I)V", num(0));
    j.define("java/util/HashMap.put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", num(0));
    // PlayerPrefs (com/unity3d/player/PlayerPrefs over SharedPreferences): in memory on the desktop.
    static std::map<std::string, jni::JValue> prefs;  // strings stored as handles
    static std::mutex prefs_lock;
    auto key = [&j](const std::vector<jni::JValue>& a) { return *j.string_value(static_cast<std::uint32_t>(a.at(0))); };
    auto get = [key](std::uint32_t, const std::vector<jni::JValue>& a) -> jni::JValue {
        std::lock_guard g{prefs_lock};
        auto it = prefs.find(key(a));
        return it == prefs.end() ? a.at(1) : it->second;
    };
    auto set = [key](std::uint32_t, const std::vector<jni::JValue>& a) -> jni::JValue {
        std::lock_guard g{prefs_lock};
        prefs[key(a)] = a.at(1);
        return 1;
    };
    const std::string PP = "com/unity3d/player/PlayerPrefs.";
    for (const char* s : {"GetInt(Ljava/lang/String;I)I", "GetFloat(Ljava/lang/String;F)F", "GetString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"})
        j.define(PP + s, get);
    for (const char* s : {"SetInt(Ljava/lang/String;I)Z", "SetFloat(Ljava/lang/String;F)Z", "SetString(Ljava/lang/String;Ljava/lang/String;)Z"})
        j.define(PP + s, set);
    j.define(PP + "HasKey(Ljava/lang/String;)Z", [key](std::uint32_t, const std::vector<jni::JValue>& a) -> jni::JValue {
        std::lock_guard g{prefs_lock};
        return prefs.count(key(a));
    });
    j.define(PP + "DeleteKey(Ljava/lang/String;)V", [key](std::uint32_t, const std::vector<jni::JValue>& a) -> jni::JValue {
        std::lock_guard g{prefs_lock};
        prefs.erase(key(a));
        return 0;
    });
    j.define(PP + "DeleteAll()V", [](std::uint32_t, const std::vector<jni::JValue>&) -> jni::JValue {
        std::lock_guard g{prefs_lock};
        prefs.clear();
        return 0;
    });
    j.define(PP + "Sync()V", num(0));
}

// Unity's startup, in the order the original Java player (UnityPlayer.smali) calls its natives.
void start_unity(int frames) {
    install_fakes();
    jni::FakeJava& j = jni::java();
    const std::uint32_t player = j.new_object("com/unity3d/player/UnityPlayer");
    const char* P = "com/unity3d/player/UnityPlayer";
    auto step = [&](const char* name, std::initializer_list<std::uint32_t> args) {
        std::fprintf(stderr, "[start] %s\n", name);
        return jni::call_native(P, name, player, args);
    };
    // UI thread: UnityPlayer constructor (nativeFile), then init(gles_mode, false) -> a(IZ).
    step("nativeFile", {j.new_string("/data/app/flippinpixels.skim-1.apk")});
    step("initJni", {});
    // new PlayerPrefs(sharedPreferences): its constructor calls the InitPlayerPrefs native.
    const std::uint32_t prefs = j.new_object("com/unity3d/player/PlayerPrefs");
    std::fprintf(stderr, "[start] InitPlayerPrefs\n");
    jni::call_native("com/unity3d/player/PlayerPrefs", "InitPlayerPrefs", prefs, {});
    step("nativeInitWWW", {j.find_class("com/unity3d/player/WWW")});
    step("nativeSetExtras", {j.new_object("android/os/Bundle")});
    // GL thread (GLSurfaceView): the queued nativeInit(gles_mode, splash_mode) runs first, then
    // onSurfaceCreated, onSurfaceChanged and onDrawFrame. The arguments come from
    // assets/bin/Data/settings.xml (gles_mode 2, splash_mode 0). They matter beyond their meaning:
    // libunity derives a splash integrity value from them, and when it doesn't match,
    // unityAndroidPrepareGameLoop removes the GL thread from Boehm's thread table (GC abort later).
    std::thread gl([&] {
        GuestThread gl_thread;
        step("nativeInit", {2, 0});
        step("nativeRecreateGfxState", {});                   // onSurfaceCreated
        step("nativeResize", {800, 1280, 800, 1280});         // onSurfaceChanged
        // First onDrawFrame: nativeRender (splash) returns true, then Unity's init runs.
        step("nativeRender", {});
        step("unityAndroidInit", {j.new_string("assets/bin/"), j.new_string("/data/data/flippinpixels.skim/lib")});
        step("unityAndroidPrepareGameLoop", {});
        step("nativeResize", {800, 1280, 800, 1280});
        step("nativeResume", {});
        step("nativeFocusChanged", {1});                      // windowFocusChanged(true), queued (UnityPlayer$28)
        for (int f = 0; f < frames; ++f) {
            const auto r = static_cast<std::uint32_t>(step("nativeRender", {}));
            std::fprintf(stderr, "[start] frame %d -> %u\n", f, r);
        }
    });
    gl.join();
    std::fprintf(stderr, "[start] Java members requested: %zu, without a fake: %zu\n", j.requested.size(), j.missing.size());
}

int mono_smoke(GuestThread& t, Library& mono) {
    Memory& m = Memory::get();
    auto str = [&](const char* s) { return m.static_str(s); };
    auto call = [&](const char* fn, std::initializer_list<std::uint32_t> args) {
        std::fprintf(stderr, "[smoke] %s\n", fn);
        return static_cast<std::uint32_t>(t.call(sym(mono, fn), args));
    };
    // Unity's layout: corlib in <root>/mono/2.0 (mounted onto assets/bin/Data/Managed).
    call("mono_set_dirs", {str("/mono/lib"), str("/mono/etc")});
    const gaddr domain = call("mono_jit_init_version", {str("Unity Root Domain"), str("v2.0.50727")});
    std::fprintf(stderr, "[smoke] domain = 0x%08x\n", domain);
    if (!domain) return 1;
    const gaddr corlib = call("mono_get_corlib", {});
    const gaddr klass = call("mono_class_from_name", {corlib, str("System"), str("Math")});
    // Max has overloads for every numeric type; pick Max(int,int) by signature.
    const gaddr desc = call("mono_method_desc_new", {str(":Max(int,int)"), 0});
    const gaddr method = call("mono_method_desc_search_in_class", {desc, klass});
    std::fprintf(stderr, "[smoke] corlib=0x%08x Math=0x%08x Max=0x%08x\n", corlib, klass, method);
    if (!method) return 1;
    const gaddr a = heap::malloc(16);
    m.write<std::int32_t>(a, 1234);
    m.write<std::int32_t>(a + 4, 5678);
    const gaddr params = heap::malloc(8);
    m.write<gaddr>(params, a);
    m.write<gaddr>(params + 4, a + 4);
    const gaddr exc = heap::calloc(1, 4);
    const gaddr boxed = call("mono_runtime_invoke", {method, 0, params, exc});
    if (!boxed || m.read<gaddr>(exc)) { std::fprintf(stderr, "[smoke] invoke failed (exception 0x%08x)\n", m.read<gaddr>(exc)); return 1; }
    const gaddr value = call("mono_object_unbox", {boxed});
    const std::int32_t result = m.read<std::int32_t>(value);
    std::printf("System.Math.Max(1234, 5678) = %d (run by Mono's JIT in dynarmic)\n", result);
    // A full collection: Boehm stops every other registered thread (the finalizer) with signals.
    call("mono_gc_collect", {call("mono_gc_max_generation", {})});
    std::printf("GC collection completed (heap %u bytes)\n", call("mono_gc_get_heap_size", {}));
    return result == 5678 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <extracted-apk-dir> [--mono-smoke] [--unity]\n", argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--list-hle") {  // used by scripts/hle_status.py
        hle::init();
        for (const auto& n : hle::function_names()) std::printf("%s\n", n.c_str());
        if (argc > 2) {  // also the symbols libmono itself provides (libunity imports many)
            Library* mono = loader::load((fs::path(argv[2]) / "assets/libs/armeabi-v7a/libmono.so").string(), "libmono.so");
            if (mono)
                for (const auto& [n, a] : mono->exports) std::printf("%s\n", n.c_str());
        }
        return 0;
    }
    const fs::path apk = fs::absolute(argv[1]);
    bool smoke = false, unity = false, jni_mode = false, start = false;
    int frames = 5;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        smoke |= a == "--mono-smoke";
        unity |= a == "--unity";
        jni_mode |= a == "--jni";
        if (a == "--start") { start = true; jni_mode = true; }
        if (a.rfind("--frames=", 0) == 0) frames = std::stoi(a.substr(9));
    }
    const fs::path repo = fs::path(argv[0]).parent_path();  // unused when run from elsewhere
    (void)repo;

    // Guest file system: the game's assets, the app data directory and the device profile, laid
    // out as on the reference device (Android 4.x paths).
    const fs::path data = fs::absolute("work/runner-data");
    runtime::Layout layout;
    layout.apk_guest = "/data/app/flippinpixels.skim-1.apk";
    for (const auto& e : fs::directory_iterator(fs::current_path()))
        if (e.path().extension() == ".apk") { layout.apk_host = e.path().string(); break; }
    layout.assets_host = (apk / "assets").string();
    layout.data_guest = "/data/data/flippinpixels.skim";
    layout.data_host = layout.private_host = data.string();
    // Optional: a real device's /proc/cpuinfo to present to the guest (default: a generic ARMv7 one).
    if (const char* cpuinfo = std::getenv("SKIM_CPUINFO"); cpuinfo && fs::exists(cpuinfo)) layout.cpuinfo = slurp(cpuinfo);
    runtime::setup(layout);
    fs::create_directories(data / "files");
    fs::create_directories(data / "root/app");
    vfs::mount("/data", (data / "root").string());

    hle::init();
    GuestThread& t = jni::thread_guest();

    const fs::path libs = apk / "assets/libs/armeabi-v7a";
    Library* mono = loader::load((libs / "libmono.so").string(), "libmono.so");
    if (!mono) return 1;
    loader::run_constructors(*mono, t);
    std::fprintf(stderr, "[run] libmono constructors done\n");

    if (unity) {
        Library* lib = loader::load((libs / "libunity.so").string(), "libunity.so");
        if (!lib) return 1;
        for (auto& n : lib->unresolved) std::fprintf(stderr, "[run] libunity unimplemented: %s\n", n.c_str());
        loader::run_constructors(*lib, t);
        std::fprintf(stderr, "[run] libunity constructors done\n");
        if (jni_mode) {
            const std::uint32_t version = static_cast<std::uint32_t>(t.call(sym(*lib, "JNI_OnLoad"), {jni::vm(), 0}));
            std::fprintf(stderr, "[run] JNI_OnLoad -> 0x%x, %zu natives registered\n", version, jni::java().natives.size());
            if (!start)
                for (const auto& n : jni::java().natives) std::printf("native %s.%s%s @ %s\n", n.cls.c_str(), n.name.c_str(), n.sig.c_str(), hle::describe(n.fn).c_str());
        }
        if (start) start_unity(frames);
    }
    return smoke ? mono_smoke(t, *mono) : 0;
}
