// The privacy keys the app builds with live in app.json and are copied into the
// generated Info.plist by `expo prebuild`. iOS knows exactly two Bluetooth keys, and
// only one of them is current: a phone that touches CoreBluetooth without
// `NSBluetoothAlwaysUsageDescription` is killed by the system — not refused, killed —
// so this test guards the name of the key, not the wording of the text.
//
// There is no "while in use" Bluetooth key: `CBManagerAuthorization` has only
// NotDetermined / Restricted / Denied / AllowedAlways (CoreBluetooth's own enum), while
// CoreLocation, which really does know "when in use", has a separate case for it.

import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const appJson = JSON.parse(
  readFileSync(fileURLToPath(new URL('../app.json', import.meta.url)), 'utf8'),
) as { expo: { ios: { infoPlist: Record<string, unknown> }; plugins: unknown[] } };

const infoPlist = appJson.expo.ios.infoPlist;

/**
 * The privacy keys iOS knows. A key outside this list grants nothing: iOS reads
 * the one it knows and ignores the rest, so the app is killed the first time it
 * touches the sensor the invented key was meant to unlock.
 */
const APPLE_PRIVACY_KEYS = [
  'NSAppleMusicUsageDescription',
  'NSBluetoothAlwaysUsageDescription',
  'NSBluetoothPeripheralUsageDescription',
  'NSCalendarsFullAccessUsageDescription',
  'NSCalendarsWriteOnlyAccessUsageDescription',
  'NSCameraUsageDescription',
  'NSContactsUsageDescription',
  'NSFaceIDUsageDescription',
  'NSHealthShareUsageDescription',
  'NSHealthUpdateUsageDescription',
  'NSLocalNetworkUsageDescription',
  'NSLocationAlwaysAndWhenInUseUsageDescription',
  'NSLocationAlwaysUsageDescription',
  'NSLocationWhenInUseUsageDescription',
  'NSMicrophoneUsageDescription',
  'NSMotionUsageDescription',
  'NSPhotoLibraryAddUsageDescription',
  'NSPhotoLibraryUsageDescription',
  'NSRemindersUsageDescription',
  'NSSpeechRecognitionUsageDescription',
  'NSUserTrackingUsageDescription',
];

describe('the iOS Info.plist the app builds with', () => {
  it('asks for Bluetooth with the key iOS actually reads', () => {
    const text = infoPlist.NSBluetoothAlwaysUsageDescription;
    expect(typeof text).toBe('string');
    expect((text as string).trim().length).toBeGreaterThan(0);
  });

  it('has no Bluetooth key that iOS does not know', () => {
    // A key nobody reads grants nothing: the app still dies on the Find Devices
    // screen the moment the radio is touched.
    expect(infoPlist.NSBluetoothWhileInUseUsageDescription).toBeUndefined();
  });

  it('does not carry the pre-iOS 13 peripheral key', () => {
    // The deployment target is iOS 16.4, so the deprecated key (iOS 12 and older)
    // buys nothing and only muddies the water.
    expect(infoPlist.NSBluetoothPeripheralUsageDescription).toBeUndefined();
  });

  it('still explains the local network, which iOS also asks about', () => {
    // The hub is reached over the local network, and the same "no key, no access"
    // rule applies there.
    expect(typeof infoPlist.NSLocalNetworkUsageDescription).toBe('string');
  });

  it('asks for the camera, which the QR code needs', () => {
    // The code on the device is read with the camera; without this key iOS kills
    // the app the moment the lens is opened.
    const text = infoPlist.NSCameraUsageDescription;
    expect(typeof text).toBe('string');
    expect((text as string).trim().length).toBeGreaterThan(0);
  });

  it('uses no privacy key that iOS does not know', () => {
    const keys = Object.keys(infoPlist).filter((k) => k.endsWith('UsageDescription'));
    expect(keys.length).toBeGreaterThan(0);
    const unknown = keys.filter((k) => !APPLE_PRIVACY_KEYS.includes(k));
    expect(unknown).toEqual([]);
  });

  it('never asks for the microphone', () => {
    // The app reads codes, it does not record anything: the camera plugin must be
    // told to leave the microphone key out.
    expect(infoPlist.NSMicrophoneUsageDescription).toBeUndefined();
    const camera = appJson.expo.plugins.find(
      (p) => Array.isArray(p) && p[0] === 'expo-camera',
    ) as [string, Record<string, unknown>] | undefined;
    expect(camera).toBeDefined();
    expect(camera?.[1]?.microphonePermission).toBe(false);
  });
});
