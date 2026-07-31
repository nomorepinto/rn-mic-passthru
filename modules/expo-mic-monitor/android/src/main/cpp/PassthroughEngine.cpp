#include "PassthroughEngine.h"

#include <algorithm>
#include <cstring>

// =====================================================================
// Lifecycle
// =====================================================================

PassthroughEngine::~PassthroughEngine() {
    stop();
}

bool PassthroughEngine::start() {
    std::lock_guard<std::mutex> lock(mStreamLock);

    if (mIsRunning.load(std::memory_order_acquire)) {
        LOGW("start() called while already running — ignoring");
        return true;
    }

    oboe::Result result = openStreams();
    if (result != oboe::Result::OK) {
        LOGE("Failed to open streams: %s", oboe::convertToText(result));
        return false;
    }

    mIsRunning.store(true, std::memory_order_release);
    LOGI("Passthrough engine started");
    return true;
}

void PassthroughEngine::stop() {
    // Signal first so any in-flight restart aborts early.
    mIsRunning.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(mStreamLock);
    closeStreams();
    LOGI("Passthrough engine stopped");
}

// =====================================================================
// Stream management
// =====================================================================

oboe::Result PassthroughEngine::openStreams() {
    // ── 1. Output stream (speaker / headphones) ─────────────────────
    //
    // Opened FIRST so we can query the device's native sample rate and
    // burst size, then match the input stream to avoid resampling.
    oboe::AudioStreamBuilder outBuilder;
    outBuilder.setDirection(oboe::Direction::Output)
              ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
              ->setSharingMode(oboe::SharingMode::Exclusive)
              ->setFormat(oboe::AudioFormat::Float)
              ->setChannelCount(kChannelCount)
              ->setUsage(oboe::Usage::Media)
              // FullDuplexStream is both the data callback and the error
              // callback for the *output* stream.
              ->setDataCallback(this)
              ->setErrorCallback(this);

    oboe::Result result = outBuilder.openStream(mOutputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return result;
    }

    LOGI("Output stream opened — sampleRate=%d, framesPerBurst=%d, "
         "bufferCapacity=%d, sharingMode=%s, performanceMode=%s",
         mOutputStream->getSampleRate(),
         mOutputStream->getFramesPerBurst(),
         mOutputStream->getBufferCapacityInFrames(),
         oboe::convertToText(mOutputStream->getSharingMode()),
         oboe::convertToText(mOutputStream->getPerformanceMode()));

    // ── 2. Input stream (microphone) ────────────────────────────────
    //
    // Locked to the output stream's sample rate so the OS doesn't insert
    // a resampler in the path (which adds latency and CPU).
    //
    // InputPreset::VoicePerformance (API 29+) disables system DSP:
    //   • No echo cancellation
    //   • No noise suppression
    //   • No automatic gain control
    // On API < 29, Oboe ignores the preset — acceptable fallback.
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
             ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
             ->setSharingMode(oboe::SharingMode::Exclusive)
             ->setFormat(oboe::AudioFormat::Float)
             ->setChannelCount(kChannelCount)
             ->setSampleRate(mOutputStream->getSampleRate())
             ->setInputPreset(oboe::InputPreset::VoicePerformance)
             // Double the output buffer capacity so we never starve the
             // output callback waiting for input data.
             ->setBufferCapacityInFrames(
                 mOutputStream->getBufferCapacityInFrames() * 2);

    result = inBuilder.openStream(mInputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        mOutputStream->close();
        mOutputStream.reset();
        return result;
    }

    LOGI("Input stream opened — sampleRate=%d, framesPerBurst=%d, "
         "bufferCapacity=%d, sharingMode=%s",
         mInputStream->getSampleRate(),
         mInputStream->getFramesPerBurst(),
         mInputStream->getBufferCapacityInFrames(),
         oboe::convertToText(mInputStream->getSharingMode()));

    // ── 3. Wire up FullDuplexStream ─────────────────────────────────
    setSharedInputStream(mInputStream);
    setSharedOutputStream(mOutputStream);

    // ── 4. Start both streams ───────────────────────────────────────
    //
    // FullDuplexStream::start() starts input first, then output.
    result = oboe::FullDuplexStream::start();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start duplex stream: %s", oboe::convertToText(result));
        closeStreams();
        return result;
    }

    return oboe::Result::OK;
}

void PassthroughEngine::closeStreams() {
    // Stop the duplex controller first (stops callbacks).
    oboe::FullDuplexStream::stop();

    // Close output before input — prevents the output callback from
    // reading a dead input stream.
    if (mOutputStream) {
        mOutputStream->close();
        mOutputStream.reset();
    }
    if (mInputStream) {
        mInputStream->close();
        mInputStream.reset();
    }
}

// =====================================================================
// Audio callback — HOT PATH
//
// Rules for this function:
//   ✗ No heap allocations (new, malloc, std::vector resize)
//   ✗ No mutex locks
//   ✗ No I/O or syscalls
//   ✗ No JNI calls
//   ✓ memcpy, memset, std::atomic loads only
// =====================================================================

oboe::DataCallbackResult PassthroughEngine::onBothStreamsReady(
        const void *inputData,
        int numInputFrames,
        void *outputData,
        int numOutputFrames) {

    const size_t bytesPerFrame = kChannelCount * sizeof(float);

    if (mIsMuted.load(std::memory_order_relaxed)) {
        // ── Muted: fill output with silence ─────────────────────────
        std::memset(outputData, 0, static_cast<size_t>(numOutputFrames) * bytesPerFrame);
    } else {
        // ── Passthrough: copy input → output ────────────────────────
        const int framesToCopy = std::min(numInputFrames, numOutputFrames);
        std::memcpy(outputData, inputData,
                    static_cast<size_t>(framesToCopy) * bytesPerFrame);

        // If the output needs more frames than we got from input,
        // zero-pad the tail to prevent playback of stale/garbage data.
        if (numOutputFrames > framesToCopy) {
            auto *tail = static_cast<uint8_t *>(outputData)
                         + static_cast<size_t>(framesToCopy) * bytesPerFrame;
            std::memset(tail, 0,
                        static_cast<size_t>(numOutputFrames - framesToCopy) * bytesPerFrame);
        }
    }

    return oboe::DataCallbackResult::Continue;
}

// =====================================================================
// Error handling
//
// Called on Oboe's error-notification thread (NOT the audio thread).
// Safe to close / reopen streams here.
// =====================================================================

void PassthroughEngine::onErrorAfterClose(
        oboe::AudioStream *stream,
        oboe::Result error) {

    if (error == oboe::Result::ErrorDisconnected) {
        LOGW("Audio stream disconnected (headphone plug/unplug?) — restarting…");

        // Grab the lifecycle lock so we don't race with a user stop().
        std::lock_guard<std::mutex> lock(mStreamLock);

        // If the user already called stop(), don't restart.
        if (!mIsRunning.load(std::memory_order_acquire)) {
            LOGI("Engine was stopped during disconnect — skipping restart");
            return;
        }

        closeStreams();

        oboe::Result restartResult = openStreams();
        if (restartResult != oboe::Result::OK) {
            LOGE("Failed to restart after disconnect: %s",
                 oboe::convertToText(restartResult));
            mIsRunning.store(false, std::memory_order_release);
        } else {
            LOGI("Streams restarted successfully after disconnect");
        }
    } else {
        LOGE("Audio stream error: %s", oboe::convertToText(error));
    }
}
