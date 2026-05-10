#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdio>
#include <vector>

#include "glyph_extractor.h"
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
Java_com_example_baqarah_vk_NativeRenderer_nUploadOutlineFromTtf(
    JNIEnv* env, jobject, jlong handle,
    jbyteArray ttfBytes, jint codepoint,
    jfloat dstX, jfloat dstY, jfloat dstW, jfloat dstH) {
    auto* r = asRenderer(handle);
    if (!r || !ttfBytes) return JNI_FALSE;

    const jsize ttfSize = env->GetArrayLength(ttfBytes);
    if (ttfSize <= 0) return JNI_FALSE;
    std::vector<uint8_t> ttf((size_t)ttfSize);
    env->GetByteArrayRegion(ttfBytes, 0, ttfSize, reinterpret_cast<jbyte*>(ttf.data()));

    baqarah::GlyphOutline g;
    if (!baqarah::extractGlyphOutline(ttf.data(), ttfSize, codepoint, g)) {
        return JNI_FALSE;
    }

    const float dx = (float)(g.bboxMaxX - g.bboxMinX);
    const float dy = (float)(g.bboxMaxY - g.bboxMinY);
    if (dx <= 0.0f || dy <= 0.0f) return JNI_FALSE;

    // Normalize outline to UV space [0, 1]^2 with Y flipped (TTF font units
    // are Y-up; our quads have Y-down UV). Even-odd fill is winding-agnostic
    // so the flip doesn't change which side is inside.
    std::vector<float> norm(g.curves.size());
    for (size_t i = 0; i < g.curves.size(); i += 2) {
        norm[i + 0] = (g.curves[i + 0] - (float)g.bboxMinX) / dx;
        norm[i + 1] = ((float)g.bboxMaxY - g.curves[i + 1]) / dy;
    }

    return r->setOutlineGlyph(norm.data(), g.curveCount, dstX, dstY, dstW, dstH)
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadOutlineGlyph(
    JNIEnv* env, jobject, jlong handle,
    jfloatArray curves, jint curveCount,
    jfloat dstX, jfloat dstY, jfloat dstW, jfloat dstH) {
    auto* r = asRenderer(handle);
    if (!r || !curves || curveCount < 0) return JNI_FALSE;
    const jsize len = env->GetArrayLength(curves);
    if (len != curveCount * 6) {
        LOGI("nUploadOutlineGlyph: size mismatch %d != %d*6", (int)len, curveCount);
        return JNI_FALSE;
    }
    std::vector<float> curveBuf((size_t)len);
    env->GetFloatArrayRegion(curves, 0, len, curveBuf.data());
    return r->setOutlineGlyph(curveBuf.data(), curveCount, dstX, dstY, dstW, dstH)
               ? JNI_TRUE
               : JNI_FALSE;
}
