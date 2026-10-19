#include <jni.h>

extern "C" {
int av_jni_set_java_vm(void* vm, void* logContext);
int av_jni_set_android_app_ctx(void* context, void* logContext);
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xfangfang_wiliwili_WiliwiliActivity_nativeInitialize(JNIEnv* env, jclass, jobject context) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) return;
    av_jni_set_java_vm(vm, nullptr);
    // FFmpeg retains the context for MediaCodec for the lifetime of the process.
    static jobject appContext = env->NewGlobalRef(context);
    av_jni_set_android_app_ctx(appContext, nullptr);
}
