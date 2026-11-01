// The Bluetooth scan, as the app sees it.
//
// The native module does the scanning; this file keeps the list of what was heard and
// hands it to whoever is looking. A build without the native module (Expo Go, the web,
// Android until it is implemented) has no Bluetooth search — the app says so instead of
// pretending that nothing is around.
import { addAdvertisement, byProximity, expireDevices, type BleDevice } from '../model/bleDevices';
import { keystoneNet } from './module';
import type { NativeBleStatus } from './types';

export interface BleSnapshot {
  status: NativeBleStatus;
  devices: BleDevice[];
}

export interface BleSource {
  /** What is around right now, closest first. */
  snapshot(): BleSnapshot;
  /** Calls `cb` whenever the list or the state of the radio changes. Returns stop(). */
  subscribe(cb: (s: BleSnapshot) => void): () => void;
  /** Starts looking. Safe to call twice. */
  start(): void;
  /** Stops looking, but keeps the subscriptions. */
  stop(): void;
  /** Stops looking and lets go of the native module. */
  dispose(): void;
}

const UNKNOWN_STATUS: NativeBleStatus = { state: 'unsupported', authorization: 'notDetermined' };

/** How often the quiet devices are swept away. */
const SWEEP_MS = 5_000;

/**
 * The scan, or null when this build has no native module. The manager is created only
 * when the scan starts: iOS asks for permission the first time a central manager
 * appears, and it must not ask before the person asked for a search.
 */
export function bleSource(): BleSource | null {
  const mod = keystoneNet();
  if (!mod) return null;

  let status: NativeBleStatus = UNKNOWN_STATUS;
  let devices: BleDevice[] = [];
  let running = false;
  let disposed = false;
  let sweep: ReturnType<typeof setInterval> | null = null;
  const listeners = new Set<(s: BleSnapshot) => void>();

  const emit = () => {
    if (disposed) return;
    const snapshot = { status, devices: byProximity(devices) };
    for (const cb of listeners) cb(snapshot);
  };

  const onAdvertisement = mod.addListener('onBleAdvertisement', (adv) => {
    devices = addAdvertisement(devices, adv, Date.now());
    emit();
  });

  const onState = mod.addListener('onBleState', (s) => {
    status = s;
    emit();
  });

  const stopSweep = () => {
    if (sweep !== null) {
      clearInterval(sweep);
      sweep = null;
    }
  };

  return {
    snapshot: () => ({ status, devices: byProximity(devices) }),
    subscribe(cb) {
      listeners.add(cb);
      return () => {
        listeners.delete(cb);
      };
    },
    start() {
      if (disposed || running) return;
      running = true;
      try {
        status = mod.bleStatus();
      } catch {
        status = UNKNOWN_STATUS;
      }
      mod.startBleScan();
      sweep = setInterval(() => {
        const fresh = expireDevices(devices, Date.now());
        if (fresh.length !== devices.length) {
          devices = fresh;
          emit();
        }
      }, SWEEP_MS);
      emit();
    },
    stop() {
      if (!running) return;
      running = false;
      stopSweep();
      mod.stopBleScan();
    },
    dispose() {
      this.stop();
      disposed = true;
      onAdvertisement.remove();
      onState.remove();
      listeners.clear();
    },
  };
}
