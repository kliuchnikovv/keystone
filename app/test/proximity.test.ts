// How close a device is: bands, not metres.
import { describe, expect, it } from 'vitest';
import {
  RSSI_UNKNOWN,
  proximityFromRssi,
  proximityLabel,
  rankByProximity,
  tooFar,
} from '../src/model/proximity';

describe('the proximity of a Bluetooth device', () => {
  it('reads the bands at their edges', () => {
    expect(proximityFromRssi(-40)).toBe('veryClose');
    expect(proximityFromRssi(-55)).toBe('veryClose');
    expect(proximityFromRssi(-56)).toBe('close');
    expect(proximityFromRssi(-67)).toBe('close');
    expect(proximityFromRssi(-68)).toBe('near');
    expect(proximityFromRssi(-80)).toBe('near');
    expect(proximityFromRssi(-81)).toBe('far');
  });

  it('does not judge a device it could not hear', () => {
    expect(proximityFromRssi(null)).toBe('unknown');
    expect(proximityFromRssi(undefined)).toBe('unknown');
    // iOS reports 127 when it has no reading.
    expect(proximityFromRssi(RSSI_UNKNOWN)).toBe('unknown');
  });

  it('says something a person can read', () => {
    expect(proximityLabel('veryClose')).toBe('very close');
    expect(proximityLabel('far')).toBe('far');
    expect(proximityLabel('unknown')).toBe('signal unknown');
  });

  it('only calls a device too far when it measured it', () => {
    expect(tooFar('far')).toBe(true);
    expect(tooFar('near')).toBe(false);
    expect(tooFar('unknown')).toBe(false);
  });
});

describe('ranking the nearby devices', () => {
  it('puts the strongest signal first', () => {
    const devices = [
      { id: 'a', rssi: -85 },
      { id: 'b', rssi: -45 },
      { id: 'c', rssi: -70 },
    ];
    const order = rankByProximity(devices, (d) => d.rssi).map((d) => d.id);
    expect(order).toEqual(['b', 'c', 'a']);
  });

  it('puts the devices it could not place last', () => {
    const devices = [
      { id: 'unknown', rssi: null },
      { id: 'weak', rssi: -90 },
      { id: 'strong', rssi: -50 },
    ];
    const order = rankByProximity(devices, (d) => d.rssi).map((d) => d.id);
    expect(order).toEqual(['strong', 'weak', 'unknown']);
  });

  it('leaves the list it was given alone', () => {
    const devices = [{ id: 'a', rssi: -80 }, { id: 'b', rssi: -40 }];
    rankByProximity(devices, (d) => d.rssi);
    expect(devices.map((d) => d.id)).toEqual(['a', 'b']);
  });
});
