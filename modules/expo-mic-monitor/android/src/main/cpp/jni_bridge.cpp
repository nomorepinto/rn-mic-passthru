#include <jni.h>
#include "PassthroughEngine.h"

// =====================================================================
// JNI Bridge
//
// Uses RegisterNatives (in JNI_OnLoad) instead of the
// Java_com_package_Class_method naming convention — cleaner, faster
// lookup, and immune to package-name refactors.
//
// The engine lives as a file-scoped singleton. This is safe because
// JS calls are serialised on the JS thread (via the Expo module).
// =====================================================================

static PassthroughEngine *gEngine = nullptr;

// ── Native method implementations ───────────────────────────────────

static jboolean nativeStart(JNIEnv * /*env*/, jobject /*thiz*/) {
    if (gEngine != nullptr) {
        // Idempotent: if already running, treat as success.
        if (gEngine->isRunning()) {
            return JNI_TRUE;
        }
        // Stopped but not cleaned up — clean up first.
        delete gEngine;
        gEngine = nullptr;
    }

    gEngine = new PassthroughEngine();
    if (gEngine->start()) {
        return JNI_TRUE;
    }

    // Start failed — clean up immediately.
    delete gEngine;
    gEngine = nullptr;
    return JNI_FALSE;
}

static void nativeStop(JNIEnv * /*env*/, jobject /*thiz*/) {
    if (gEngine != nullptr) {
        gEngine->stop();
        delete gEngine;
        gEngine = nullptr;
    }
}

static void nativeSetMuted(JNIEnv * /*env*/, jobject /*thiz*/, jboolean muted) {
    if (gEngine != nullptr) {
        gEngine->setMuted(muted == JNI_TRUE);
    }
}

// ── Registration table ──────────────────────────────────────────────

static const JNINativeMethod gMethods[] = {
    {"nativeStart",    "()Z",  reinterpret_cast<void *>(nativeStart)},
    {"nativeStop",     "()V",  reinterpret_cast<void *>(nativeStop)},
    {"nativeSetMuted", "(Z)V", reinterpret_cast<void *>(nativeSetMuted)},
};

// ── JNI_OnLoad: called once when System.loadLibrary("micmonitor") ───

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: GetEnv failed");
        return JNI_ERR;
    }

    // The Kotlin object `MicMonitorBridge` compiles to this class.
    jclass clazz = env->FindClass("expo/modules/micmonitor/MicMonitorBridge");
    if (clazz == nullptr) {
        LOGE("JNI_OnLoad: FindClass(MicMonitorBridge) failed");
        return JNI_ERR;
    }

    int rc = env->RegisterNatives(
        clazz, gMethods,
        static_cast<jint>(sizeof(gMethods) / sizeof(gMethods[0])));
    if (rc != JNI_OK) {
        LOGE("JNI_OnLoad: RegisterNatives failed (%d)", rc);
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: MicMonitor native methods registered");
    return JNI_VERSION_1_6;
}
