import { requireNativeModule } from 'expo-modules-core';

/**
 * The native ExpoMicMonitor module, loaded via JSI (New Architecture).
 *
 * This object exposes:
 *   - start(): Promise<boolean>   — opens Oboe streams, returns success
 *   - stop(): void               — tears down streams and releases audio focus
 *   - setMuted(muted: boolean): void — atomically toggles silence in the audio callback
 */
const ExpoMicMonitor = requireNativeModule('ExpoMicMonitor');

export async function start(): Promise<boolean> {
  return await ExpoMicMonitor.start();
}

export function stop(): void {
  ExpoMicMonitor.stop();
}

export function setMuted(muted: boolean): void {
  ExpoMicMonitor.setMuted(muted);
}
