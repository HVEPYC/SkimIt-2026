// Android entry point. The port ships this library as lib/<abi>/libunity.so (plus an empty
// libmono.so), so the original Java player's System.loadLibrary("mono") / ("unity") lands here:
// JNI_OnLoad boots the original 32-bit libmono/libunity in the guest and runs *their* JNI_OnLoad
// against the real JVM, which registers Unity's natives through host trampolines.
#include <android/log.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "guest/cpu.h"
#include "hle/registry.h"
#include "jni/bridge.h"
#include "loader/elf.h"
#include "runtime/environment.h"
#include "runtime/zip.h"

using namespace skim;
namespace fs = std::filesystem;

namespace {
// The runner logs to stderr (as on the desktop); on Android that goes to logcat, tag "skimrunner".
void stderr_to_logcat() {
    int p[2];
    if (pipe(p) != 0) return;
    dup2(p[1], STDERR_FILENO);
    close(p[1]);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([fd = p[0]] {
        std::string line;
        char buf[1024];
        for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;)
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] != '\n') { line += buf[i]; continue; }
                __android_log_write(ANDROID_LOG_INFO, "skimrunner", line.c_str());
                line.clear();
            }
    }).detach();
}

std::string jstr(JNIEnv* env, jobject s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(static_cast<jstring>(s), nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(static_cast<jstring>(s), c);
    return r;
}

struct AppPaths { std::string apk, data; };
AppPaths app_paths(JNIEnv* env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    jobject app = env->CallStaticObjectMethod(at, env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;"));
    jclass ctx = env->GetObjectClass(app);
    AppPaths p;
    p.apk = jstr(env, env->CallObjectMethod(app, env->GetMethodID(ctx, "getPackageCodePath", "()Ljava/lang/String;")));
    jobject info = env->CallObjectMethod(app, env->GetMethodID(ctx, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;"));
    p.data = jstr(env, env->GetObjectField(info, env->GetFieldID(env->GetObjectClass(info), "dataDir", "Ljava/lang/String;")));
    return p;
}

// Extracts the guest payload once per APK build (size + mtime stamp); returns the assets dir.
std::string extract_payload(const AppPaths& p) {
    struct stat st{};
    stat(p.apk.c_str(), &st);
    const std::string stamp = std::to_string(st.st_size) + "-" + std::to_string(st.st_mtime);
    const fs::path root = fs::path(p.data) / "skimrunner/payload";
    const fs::path dir = root / stamp;
    if (!fs::exists(dir / ".complete")) {
        std::error_code ec;
        fs::remove_all(root, ec);
        const int n = runtime::unzip(p.apk, {"assets/libs/armeabi-v7a/", "assets/bin/Data/Managed/"}, dir.string());
        std::fprintf(stderr, "[boot] extracted %d payload files\n", n);
        if (n <= 0) return {};
        std::ofstream(dir / ".complete") << stamp;
    }
    return (dir / "assets").string();
}
}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    static jint version = 0;
    if (version) return version;  // loadLibrary("unity") after an earlier load
    stderr_to_logcat();
    JNIEnv* env = nullptr;
    vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    const AppPaths paths = app_paths(env);
    std::fprintf(stderr, "[boot] apk %s, data %s\n", paths.apk.c_str(), paths.data.c_str());
    const std::string assets = extract_payload(paths);
    if (assets.empty()) return JNI_ERR;

    runtime::Layout layout;
    layout.apk_guest = layout.apk_host = paths.apk;
    layout.assets_host = assets;
    layout.data_guest = layout.data_host = paths.data;
    layout.private_host = paths.data + "/skimrunner";
    runtime::setup(layout);

    hle::init();
    GuestThread& t = jni::thread_guest();
    const std::string libs = assets + "/libs/armeabi-v7a/";
    Library* mono = loader::load(libs + "libmono.so", "libmono.so");
    if (!mono) return JNI_ERR;
    loader::run_constructors(*mono, t);
    Library* unity = loader::load(libs + "libunity.so", "libunity.so");
    if (!unity) return JNI_ERR;
    loader::run_constructors(*unity, t);
    gaddr guest_vm = 0, guest_env = 0;
    jni::use_host_java(vm, &guest_vm, &guest_env);
    const auto onload = unity->exports.find("JNI_OnLoad");
    if (onload == unity->exports.end()) return JNI_ERR;
    version = static_cast<jint>(t.call(onload->second, {guest_vm, 0}));
    std::fprintf(stderr, "[boot] libunity JNI_OnLoad -> 0x%x\n", version);
    return version;
}
