package expo.modules.micmonitor

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import androidx.core.content.ContextCompat
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

// ─────────────────────────────────────────────────────────────────────
// MicMonitorBridge — JNI binding target
//
// Kotlin `object` compiles to a JVM class whose methods are bound
// via RegisterNatives in jni_bridge.cpp at System.loadLibrary() time.
// ─────────────────────────────────────────────────────────────────────
object MicMonitorBridge {
    init {
        System.loadLibrary("micmonitor")
    }

    external fun nativeStart(): Boolean
    external fun nativeStop()
    external fun nativeSetMuted(muted: Boolean)
}

// ─────────────────────────────────────────────────────────────────────
// ExpoMicMonitorModule — Expo Modules API entry point
//
// Exposes start/stop/setMuted to JavaScript. Uses the Kotlin DSL
// from expo-modules-core, which builds on JSI (New Architecture) and
// also supports the legacy bridge as a fallback.
// ─────────────────────────────────────────────────────────────────────
class ExpoMicMonitorModule : Module() {

    private var audioFocusRequest: AudioFocusRequest? = null

    // Convenience accessor for the Android context.
    private val ctx: Context
        get() = appContext.reactContext
            ?: throw CodedException("ERR_NO_CONTEXT", "React context is not available", null)

    override fun definition() = ModuleDefinition {

        Name("ExpoMicMonitor")

        // ── start() → Promise<boolean> ──────────────────────────────
        //
        // 1. Checks RECORD_AUDIO permission (must be requested by JS
        //    before calling this — we don't prompt from native).
        // 2. Requests transient audio focus.
        // 3. Opens Oboe streams via JNI.
        AsyncFunction("start") {
            // Permission gate
            val permission = ContextCompat.checkSelfPermission(
                ctx, Manifest.permission.RECORD_AUDIO
            )
            if (permission != PackageManager.PERMISSION_GRANTED) {
                throw CodedException(
                    "ERR_PERMISSION",
                    "RECORD_AUDIO permission is not granted. " +
                    "Call PermissionsAndroid.request() from JS before starting.",
                    null
                )
            }

            // Audio focus — transient gain so other audio ducks.
            requestAudioFocus()

            // Fire up the native engine.
            val success = MicMonitorBridge.nativeStart()
            if (!success) {
                abandonAudioFocus()
                throw CodedException(
                    "ERR_ENGINE",
                    "Failed to start the Oboe audio engine. " +
                    "Check logcat tag 'MicMonitor' for details.",
                    null
                )
            }

            return@AsyncFunction success
        }

        // ── stop() → void ───────────────────────────────────────────
        Function("stop") {
            MicMonitorBridge.nativeStop()
            abandonAudioFocus()
        }

        // ── setMuted(boolean) → void ────────────────────────────────
        Function("setMuted") { muted: Boolean ->
            MicMonitorBridge.nativeSetMuted(muted)
        }

        // ── Cleanup on module destruction ───────────────────────────
        OnDestroy {
            MicMonitorBridge.nativeStop()
            abandonAudioFocus()
        }
    }

    // ── Audio Focus helpers ─────────────────────────────────────────

    private fun requestAudioFocus() {
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()

            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(attrs)
                .setWillPauseWhenDucked(false)
                .build()

            audioManager.requestAudioFocus(request)
            audioFocusRequest = request
        } else {
            // Pre-Oreo fallback (API 24-25).
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            )
        }
    }

    private fun abandonAudioFocus() {
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
            audioFocusRequest = null
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(null)
        }
    }
}
