#include <jni.h>
#include <SDL2/SDL.h>
#include <atomic>
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
    // MediaCodec owns the buffer transform and overwrites native-window changes.
    // Keep rotated sources upright using libmpv's renderer and SDR tone mapping.
    return degrees == 0;
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
