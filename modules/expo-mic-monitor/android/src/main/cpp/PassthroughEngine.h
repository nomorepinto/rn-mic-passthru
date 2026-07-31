#pragma once

#include <oboe/Oboe.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <android/log.h>

// ─────────────────────────────────────────────────────────────────────
// PassthroughEngine
//
// Zero-copy mic → speaker passthrough using Oboe's FullDuplexStream.
//
// Architecture:
//   • Output stream (speaker) is opened first to discover the device's
//     native sample rate and burst size.
//   • Input stream (mic) is opened second, locked to the same sample
//     rate to avoid resampling.
//   • FullDuplexStream drives the pipeline: its onAudioReady() fires
//     on the output callback, reads from the input stream, and calls
//     our onBothStreamsReady() where we simply memcpy (or silence).
//   • Mute is an atomic bool — no locks on the hot path.
//   • Stream disconnects (headphone plug/unplug) trigger automatic
//     restart via onErrorAfterClose().
// ─────────────────────────────────────────────────────────────────────

#define LOG_TAG "MicMonitor"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

class PassthroughEngine : public oboe::FullDuplexStream {
public:
    PassthroughEngine() = default;
    ~PassthroughEngine();

    // ── Lifecycle (called from JNI thread) ──────────────────────────
    bool start();
    void stop();

    // ── Controls (called from JNI thread, read on audio thread) ─────
    void setMuted(bool muted) {
        mIsMuted.store(muted, std::memory_order_relaxed);
    }

    bool isMuted() const {
        return mIsMuted.load(std::memory_order_relaxed);
    }

    bool isRunning() const {
        return mIsRunning.load(std::memory_order_acquire);
    }

    // ── FullDuplexStream callback (runs on high-priority audio thread) ─
    oboe::DataCallbackResult onBothStreamsReady(
        const void *inputData,
        int numInputFrames,
        void *outputData,
        int numOutputFrames
    ) override;

    // ── Error callback (runs on Oboe's error-notification thread) ───
    void onErrorAfterClose(
        oboe::AudioStream *stream,
        oboe::Result error
    ) override;

private:
    oboe::Result openStreams();
    void         closeStreams();

    // Shared pointers keep the streams alive while FullDuplexStream
    // references them internally via raw getInputStream()/getOutputStream().
    std::shared_ptr<oboe::AudioStream> mInputStream;
    std::shared_ptr<oboe::AudioStream> mOutputStream;

    std::atomic<bool> mIsMuted{false};
    std::atomic<bool> mIsRunning{false};

    // Serialises open/close/restart so that a disconnect-triggered
    // restart cannot race with a user-initiated stop().
    std::mutex mStreamLock;

    // ── Audio format constants ──────────────────────────────────────
    // Mono is sufficient for vocal monitoring and halves the data rate.
    static constexpr int32_t kChannelCount = 1;
};
