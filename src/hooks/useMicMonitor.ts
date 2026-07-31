import { useState, useCallback, useEffect } from 'react';
import { Platform, PermissionsAndroid } from 'react-native';
import { start, stop, setMuted } from 'expo-mic-monitor';

/**
 * useMicMonitor — drop-in replacement for the WebRTC loopback hook.
 *
 * Differences from the old WebRTC version:
 *   • No `stream` in the return value — audio flows through the hardware
 *     directly (Oboe input → Oboe output), so there's no MediaStream
 *     to render via <RTCView> or <audio>.
 *   • Latency drops from ~100-150ms (WebRTC loopback) to <20-30ms
 *     (Oboe exclusive-mode, matched sample rate, mono float32).
 *   • Android-only for now; iOS will use AVAudioEngine separately.
 *
 * Usage:
 * ```tsx
 * const { isMonitoring, isMuted, error, startMonitoring, stopMonitoring, toggleMute } = useMicMonitor();
 * ```
 */
export function useMicMonitor() {
  const [isMonitoring, setIsMonitoring] = useState<boolean>(false);
  const [isMuted, setIsMuted] = useState<boolean>(false);
  const [error, setError] = useState<string | null>(null);

  // ── Stop ─────────────────────────────────────────────────────────
  const stopMonitoring = useCallback(() => {
    try {
      stop();
    } catch {
      // Best-effort cleanup; swallow errors.
    }
    setIsMonitoring(false);
    setIsMuted(false);
  }, []);

  // ── Start ────────────────────────────────────────────────────────
  const startMonitoring = useCallback(async () => {
    try {
      setError(null);

      // Ensure we're on Android (this module is Android-only).
      if (Platform.OS !== 'android') {
        throw new Error(
          'expo-mic-monitor is Android-only. Use AVAudioEngine on iOS.'
        );
      }

      // Request RECORD_AUDIO permission before touching native code.
      const granted = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.RECORD_AUDIO,
        {
          title: 'Microphone Permission',
          message:
            'This app needs microphone access for real-time vocal monitoring.',
          buttonPositive: 'Allow',
          buttonNegative: 'Deny',
        }
      );

      if (granted !== PermissionsAndroid.RESULTS.GRANTED) {
        throw new Error(
          'Microphone permission denied. Enable it in Settings to use mic monitoring.'
        );
      }

      // Stop any existing session first (idempotent).
      stopMonitoring();

      // Fire up the Oboe engine.
      const success = await start();
      if (success) {
        setIsMonitoring(true);
      } else {
        throw new Error(
          'Failed to start the audio engine. Check logcat for details.'
        );
      }
    } catch (err: any) {
      console.error('[useMicMonitor] startMonitoring failed:', err);
      setError(err.message || 'Failed to initialise mic monitor.');
      stopMonitoring();
    }
  }, [stopMonitoring]);

  // ── Toggle mute ──────────────────────────────────────────────────
  const toggleMute = useCallback(() => {
    const newMuted = !isMuted;
    try {
      setMuted(newMuted);
      setIsMuted(newMuted);
    } catch (err: any) {
      console.error('[useMicMonitor] toggleMute failed:', err);
    }
  }, [isMuted]);

  // ── Cleanup on unmount ───────────────────────────────────────────
  useEffect(() => {
    return () => {
      stopMonitoring();
    };
  }, [stopMonitoring]);

  return {
    isMonitoring,
    isMuted,
    error,
    startMonitoring,
    stopMonitoring,
    toggleMute,
  } as const;
}
