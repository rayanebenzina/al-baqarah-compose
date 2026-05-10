#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <vector>

#include "sdf_gen.h"
#include "vk_renderer.h"

#define LOG_TAG "BaqarahVkJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using baqarah::VkRenderer;

static inline VkRenderer* asRenderer(jlong h) { return reinterpret_cast<VkRenderer*>(h); }

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nCreate(JNIEnv*, jobject) {
    auto* r = new VkRenderer();
    LOGI("nCreate -> %p", r);
    return reinterpret_cast<jlong>(r);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDestroy(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (r) delete r;
    LOGI("nDestroy %p", r);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nAttachSurface(
    JNIEnv* env, jobject, jlong handle, jobject surface) {
    auto* r = asRenderer(handle);
    if (!r || !surface) return JNI_FALSE;
    ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
    if (!w) return JNI_FALSE;
    bool ok = r->attachWindow(w);
    ANativeWindow_release(w);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDetachSurface(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (r) r->detachWindow();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDrawFrame(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (!r) return JNI_FALSE;
    return r->drawFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nSetScrollY(JNIEnv*, jobject, jlong handle, jfloat y) {
    auto* r = asRenderer(handle);
    if (r) r->setScrollY(y);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadAyahAtlas(
    JNIEnv* env, jobject, jlong handle, jbyteArray alpha, jint w, jint h, jint spread,
    jfloatArray quads, jint quadCount) {
    auto* r = asRenderer(handle);
    if (!r || !alpha || !quads || w <= 0 || h <= 0 || spread <= 0 || quadCount < 0) {
        return JNI_FALSE;
    }
    const jsize aLen = env->GetArrayLength(alpha);
    if (aLen != w * h) {
        LOGI("nUploadAyahAtlas: alpha size %d != %dx%d", (int)aLen, w, h);
        return JNI_FALSE;
    }
    const jsize qLen = env->GetArrayLength(quads);
    if (qLen != quadCount * 8) {
        LOGI("nUploadAyahAtlas: quads size %d != %d*8", (int)qLen, quadCount);
        return JNI_FALSE;
    }

    std::vector<uint8_t> alphaBuf((size_t)aLen);
    env->GetByteArrayRegion(alpha, 0, aLen, reinterpret_cast<jbyte*>(alphaBuf.data()));

    std::vector<uint8_t> sdfBuf((size_t)aLen);
    baqarah::computeSdf(alphaBuf.data(), w, h, spread, /*threshold=*/128, sdfBuf.data());

    std::vector<float> quadBuf((size_t)qLen);
    env->GetFloatArrayRegion(quads, 0, qLen, quadBuf.data());

    return r->setAyahAtlas(sdfBuf.data(), w, h, quadBuf.data(), quadCount) ? JNI_TRUE
                                                                            : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadGlyphAlpha(
    JNIEnv* env, jobject, jlong handle, jbyteArray alpha, jint w, jint h, jint spread) {
    auto* r = asRenderer(handle);
    if (!r || !alpha || w <= 0 || h <= 0 || spread <= 0) return JNI_FALSE;

    const jsize len = env->GetArrayLength(alpha);
    if (len != w * h) {
        LOGI("nUploadGlyphAlpha: size mismatch %d vs %dx%d=%d", (int)len, w, h, w * h);
        return JNI_FALSE;
    }

    std::vector<uint8_t> alphaBuf((size_t)len);
    env->GetByteArrayRegion(alpha, 0, len, reinterpret_cast<jbyte*>(alphaBuf.data()));

    std::vector<uint8_t> sdfBuf((size_t)len);
    baqarah::computeSdf(alphaBuf.data(), w, h, spread, /*threshold=*/128, sdfBuf.data());

    return r->setGlyphSdf(sdfBuf.data(), w, h) ? JNI_TRUE : JNI_FALSE;
}
