// How close a Bluetooth device is: bands, not metres.
// BLE RSSI is noisy — the same device reads ±50 % apart depending on how the phone is
// held and what is in the room — so the app says "very close" instead of pretending to
// know "2.3 m". What a person needs is "is it close enough to work".

export type Proximity = 'veryClose' | 'close' | 'near' | 'far' | 'unknown';

/** iOS reports 127 when it has no reading for a peripheral. */
export const RSSI_UNKNOWN = 127;

export function proximityFromRssi(rssi: number | null | undefined): Proximity {
  if (rssi === null || rssi === undefined) return 'unknown';
  if (rssi >= RSSI_UNKNOWN) return 'unknown';
  if (rssi >= -55) return 'veryClose';
  if (rssi >= -67) return 'close';
  if (rssi >= -80) return 'near';
  return 'far';
}

export function proximityLabel(p: Proximity): string {
  switch (p) {
    case 'veryClose':
      return 'very close';
    case 'close':
      return 'close';
    case 'near':
      return 'near';
    case 'far':
      return 'far';
    case 'unknown':
      return 'signal unknown';
  }
}

/** Strongest signal first; devices we cannot place come last. */
export function rankByProximity<T>(items: T[], rssiOf: (item: T) => number | null): T[] {
  const placeable = (rssi: number | null): rssi is number => rssi !== null && rssi < RSSI_UNKNOWN;
  return [...items].sort((a, b) => {
    const ra = rssiOf(a);
    const rb = rssiOf(b);
    if (!placeable(ra) && !placeable(rb)) return 0;
    if (!placeable(ra)) return 1;
    if (!placeable(rb)) return -1;
    return rb - ra;
  });
}

/**
 * Too far to work with. Only a device we actually measured can be too far: a device
 * that never showed up over Bluetooth is not judged at all, because the app has no
 * idea where it is.
 */
export function tooFar(p: Proximity): boolean {
  return p === 'far';
}
