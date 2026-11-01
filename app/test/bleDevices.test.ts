// What the app makes of the devices it hears around it.
import { describe, expect, it } from 'vitest';
import {
  BLE_EXPIRY_MS,
  addAdvertisement,
  byProximity,
  expireDevices,
  medianRssi,
  type BleAdvertisement,
} from '../src/model/bleDevices';

function advertisement(over: Partial<BleAdvertisement> = {}): BleAdvertisement {
  return {
    id: 'device-1',
    rssi: -60,
    name: 'Thing',
    serviceData: {},
    manufacturerData: {},
    ...over,
  };
}

describe('the strength of a signal', () => {
  it('takes the middle of the last few readings', () => {
    expect(medianRssi([-40, -80, -60, -70, -50])).toBe(-60);
    expect(medianRssi([-40, -80, -60])).toBe(-60);
    expect(medianRssi([-40])).toBe(-40);
  });

  it('keeps only the last few', () => {
    // The old, strong readings must not keep the device in the "very close" band.
    expect(medianRssi([-40, -40, -90, -90, -90, -90, -90])).toBe(-90);
  });

  it('does not take a middle of readings it does not have', () => {
    expect(medianRssi([127, 127])).toBeNull();
    expect(medianRssi([-50, 127])).toBe(-50);
  });
});

describe('the devices around the phone', () => {
  it('remembers a device and its last readings', () => {
    let devices = addAdvertisement([], advertisement({ rssi: -50 }), 1000);
    devices = addAdvertisement(devices, advertisement({ rssi: -70 }), 2000);
    expect(devices).toHaveLength(1);
    expect(devices[0]?.samples).toEqual([-50, -70]);
    expect(devices[0]?.median).toBe(-60);
    expect(devices[0]?.proximity).toBe('close');
    expect(devices[0]?.lastSeen).toBe(2000);
  });

  it('keeps the name it learned', () => {
    let devices = addAdvertisement([], advertisement({ name: 'Plug' }), 1000);
    devices = addAdvertisement(devices, advertisement({ name: null }), 2000);
    expect(devices[0]?.name).toBe('Plug');
  });

  it('remembers what a Matter device said about itself', () => {
    const devices = addAdvertisement(
      [],
      advertisement({
        serviceData: { fff6: [0x00, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04] },
      }),
      1000,
    );
    expect(devices[0]?.matter?.source).toBe('service');
    expect(devices[0]?.matter?.discriminator).toBe(3840);
  });

  it('forgets a device that has gone quiet', () => {
    const devices = addAdvertisement([], advertisement(), 1000);
    expect(expireDevices(devices, 1000 + BLE_EXPIRY_MS)).toHaveLength(1);
    expect(expireDevices(devices, 1000 + BLE_EXPIRY_MS + 1)).toHaveLength(0);
  });

  it('shows the closest device first', () => {
    let devices = addAdvertisement([], advertisement({ id: 'far', rssi: -85 }), 1000);
    devices = addAdvertisement(devices, advertisement({ id: 'near', rssi: -45 }), 2000);
    expect(byProximity(devices).map((d) => d.id)).toEqual(['near', 'far']);
  });

  it('does not judge a device it has no reading for', () => {
    const devices = addAdvertisement([], advertisement({ rssi: 127 }), 1000);
    expect(devices[0]?.median).toBeNull();
    expect(devices[0]?.proximity).toBe('unknown');
  });
});
