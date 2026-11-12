// The devices around the phone, and what we make of their signal.
//
// A single RSSI reading jumps around: the same device reads -55 in one advertisement
// and -75 in the next, depending on how the phone is held. Judging a device by one
// reading makes the list flicker between "close" and "far". So we keep the last few
// readings and judge by the middle one.
//
// The model is deliberately free of React and of the native module: it is plain data
// in, plain data out, so it can be tested on a computer that has no Bluetooth.

import { matterFromAdvertisement, type AdvertisementLike, type MatterIdentity } from './matter';
import { RSSI_UNKNOWN, proximityFromRssi, rankByProximity, type Proximity } from './proximity';

/** How many readings we keep per device. */
export const BLE_SAMPLE_WINDOW = 5;

/** A device we have not heard of for this long is gone. */
export const BLE_EXPIRY_MS = 30_000;

export interface BleDevice {
  /** The system's id for the device. */
  id: string;
  /** The name it advertises, when it advertises one. */
  name: string | null;
  /** The last reading, for the small number next to the name. */
  rssi: number;
  /** The middle of the recent readings — what the band is judged by. */
  median: number | null;
  /** The band the device is in. */
  proximity: Proximity;
  /** When we last heard of it, in milliseconds. */
  lastSeen: number;
  /** The last few readings we heard from it. */
  samples: number[];
  /** What it says about itself, if it says anything Matter-ish. */
  matter: MatterIdentity | null;
}

/** One advertisement, as the app sees it. */
export interface BleAdvertisement extends AdvertisementLike {
  id: string;
  rssi: number;
  name: string | null;
}

/**
 * The middle reading of the last few. Unknown readings (iOS says 127) do not count —
 * there is no signal to take a middle of. With an even number of readings the two middle
 * ones are averaged, the way a median usually is.
 */
export function medianRssi(samples: number[]): number | null {
  const real = samples.filter((r) => r !== RSSI_UNKNOWN).slice(-BLE_SAMPLE_WINDOW);
  if (real.length === 0) return null;
  const sorted = [...real].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  if (sorted.length % 2 === 1) return sorted[middle] ?? null;
  return ((sorted[middle - 1] ?? 0) + (sorted[middle] ?? 0)) / 2;
}

/** Remembers an advertisement, keeping the device's last few readings. */
export function addAdvertisement(
  devices: BleDevice[],
  adv: BleAdvertisement,
  now: number,
): BleDevice[] {
  const matter = matterFromAdvertisement(adv);
  const index = devices.findIndex((d) => d.id === adv.id);
  const previous = index >= 0 ? devices[index] : null;
  const samples = [...(previous?.samples ?? []), adv.rssi].slice(-BLE_SAMPLE_WINDOW);
  const median = medianRssi(samples);
  const device: BleDevice = {
    id: adv.id,
    name: adv.name ?? previous?.name ?? null,
    rssi: adv.rssi,
    median,
    proximity: proximityFromRssi(median),
    lastSeen: now,
    matter: matter ?? previous?.matter ?? null,
    samples,
  };
  const out = [...devices];
  if (index >= 0) out[index] = device;
  else out.push(device);
  return out;
}

/** Forgets the devices that have gone quiet. */
export function expireDevices(devices: BleDevice[], now: number): BleDevice[] {
  return devices.filter((d) => now - d.lastSeen <= BLE_EXPIRY_MS);
}

/** A device around the phone that says something Matter-ish — so the hub can add it. */
export type MatterDevice = BleDevice & { matter: MatterIdentity };

/** The devices to show: the closest first, the ones we cannot place last. */
export function byProximity(devices: BleDevice[]): BleDevice[] {
  return rankByProximity(devices, (d) => d.median);
}

/**
 * The devices the app can do something with: the ones that speak Matter.
 *
 * A device that says nothing Matter-ish cannot be added by the hub, so putting it in
 * front of the person only makes them read a row they cannot act on. The radio still
 * hears everything — this only decides what is worth showing.
 */
export function matterDevices(devices: BleDevice[]): MatterDevice[] {
  return devices.filter((d): d is MatterDevice => d.matter !== null);
}

export type { MatterIdentity };
