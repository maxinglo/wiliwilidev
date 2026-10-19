#include <jni.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <dlfcn.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "utils/android_video.hpp"

namespace {
jclass activityClass = nullptr;
std::atomic<uint64_t> surfaceGeneration{0};
}

extern "C" {
int av_jni_set_java_vm(void* vm, void* logContext);
int av_jni_set_android_app_ctx(void* context, void* logContext);
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xfangfang_wiliwili_WiliwiliActivity_nativeInitialize(JNIEnv* env, jclass clazz, jobject context) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) return;
    av_jni_set_java_vm(vm, nullptr);
    // FFmpeg retains the context for MediaCodec for the lifetime of the process.
    static jobject appContext = env->NewGlobalRef(context);
    av_jni_set_android_app_ctx(appContext, nullptr);
    activityClass = static_cast<jclass>(env->NewGlobalRef(clazz));
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xfangfang_wiliwili_VideoSurface_nativeSurfaceChanged(JNIEnv*, jclass) { ++surfaceGeneration; }

void* androidAcquireVideoSurface() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !activityClass) return nullptr;
    auto method = env->GetStaticMethodID(activityClass, "getVideoSurface", "()Landroid/view/Surface;");
    auto surface = env->CallStaticObjectMethod(activityClass, method);
    if (!surface) return nullptr;
    auto result = env->NewGlobalRef(surface);
    env->DeleteLocalRef(surface);
    return result;
}

void androidReleaseVideoSurface(void* surface) {
    if (surface) static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv())->DeleteGlobalRef(static_cast<jobject>(surface));
}

bool androidSetVideoRotation(void* surface, int degrees) {
    // Resolve the API 26 function dynamically so the APK still loads on API 23.
    using SetTransform = int32_t (*)(ANativeWindow*, int32_t);
    static void* nativeWindowLibrary = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    static auto setTransform = nativeWindowLibrary
        ? reinterpret_cast<SetTransform>(dlsym(nativeWindowLibrary, "ANativeWindow_setBuffersTransform")) : nullptr;
    if (!setTransform || !surface) return degrees == 0;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto* window = ANativeWindow_fromSurface(env, static_cast<jobject>(surface));
    if (!window) return false;
    const int transform = degrees == 90 ? 4 : degrees == 180 ? 3 : degrees == 270 ? 7 : 0;
    int result = setTransform(window, transform);
    ANativeWindow_release(window);
    if (result != 0) __android_log_print(ANDROID_LOG_WARN, "wiliwili-hdr", "Cannot rotate video Surface: %d", result);
    return result == 0;
}

bool androidSupportsHdr10() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    return env && activityClass && env->CallStaticBooleanMethod(activityClass,
        env->GetStaticMethodID(activityClass, "supportsHdr10", "()Z"));
}

uint64_t androidVideoSurfaceGeneration() { return surfaceGeneration.load(); }

void androidSetVideoBounds(int x, int y, int w, int h, float aspect, bool crop, bool stretch) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !activityClass) return;
    env->CallStaticVoidMethod(activityClass,
        env->GetStaticMethodID(activityClass, "setVideoBounds", "(IIIIFZZ)V"),
        x, y, w, h, static_cast<jfloat>(aspect), static_cast<jboolean>(crop), static_cast<jboolean>(stretch));
}
