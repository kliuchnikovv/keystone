import { useCallback, useEffect, useMemo, useState } from 'react';
import { bleSource } from '../native/ble';
import type { BleDevice } from '../model/bleDevices';
import type { NativeBleStatus } from '../native/types';

export interface BleScan {
  /** False where the build cannot listen to Bluetooth (web, Expo Go, Android for now). */
  available: boolean;
  /** What the radio says about itself. */
  status: NativeBleStatus;
  /** The devices around the phone, closest first. */
  devices: BleDevice[];
  /** Look again — the radio stops and starts. */
  rescan(): void;
}

const NO_STATUS: NativeBleStatus = { state: 'unsupported', authorization: 'notDetermined' };

/**
 * The devices around the phone. The radio is only switched on while `active` is true —
 * iOS asks the person for permission the first time it starts, and it has no business
 * asking before they opened the search.
 */
export function useBleScan(active: boolean): BleScan {
  const source = useMemo(() => bleSource(), []);
  const [status, setStatus] = useState<NativeBleStatus>(source?.snapshot().status ?? NO_STATUS);
  const [devices, setDevices] = useState<BleDevice[]>(source?.snapshot().devices ?? []);

  useEffect(() => {
    if (!source) return;
    const unsubscribe = source.subscribe((s) => {
      setStatus(s.status);
      setDevices(s.devices);
    });
    if (active) source.start();
    return () => {
      unsubscribe();
      source.stop();
    };
  }, [source, active]);

  useEffect(() => () => source?.dispose(), [source]);

  const rescan = useCallback(() => {
    source?.stop();
    source?.start();
  }, [source]);

  return { available: source !== null, status, devices, rescan };
}
