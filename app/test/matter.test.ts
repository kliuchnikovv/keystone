// What a Matter device says about itself, and how the app reads it.
import { describe, expect, it } from 'vitest';
import {
  MATTER_ENHANCED_COMPANY_ID,
  MATTER_PAYLOAD_MANUFACTURER,
  MATTER_PAYLOAD_QR,
  MATTER_PAYLOAD_SERVICE,
  base38Decode,
  base38Encode,
  decodeMatterManufacturerData,
  decodeMatterQr,
  decodeMatterServiceData,
  matterFromAdvertisement,
  matterFromQr,
  matterBusy,
  matterWords,
  payloadBytes,
} from '../src/model/matter';

// The example QR code from the Matter specification: a device with vendor id 0xFFF1,
// product id 0x8000, discriminator 3840 and setup PIN code 20202021 — the same
// numbers as the esp-matter test DAC the hub uses.
const SPEC_QR = 'MT:Y.K9042C00KA0648G00';
const SPEC_BYTES = [0x88, 0xff, 0x07, 0x00, 0x44, 0x00, 0xe0, 0x4b, 0x84, 0x68, 0x02];

describe('base38, the alphabet of a Matter QR code', () => {
  it('reads the example payload back', () => {
    expect(base38Decode('Y.K9042C00KA0648G00')).toEqual(SPEC_BYTES);
  });

  it('writes it again', () => {
    expect(base38Encode(SPEC_BYTES)).toBe('Y.K9042C00KA0648G00');
  });

  it('reads one byte as two characters', () => {
    expect(base38Encode([0x00])).toBe('00');
    expect(base38Encode([0x01])).toBe('10');
    expect(base38Decode('10')).toEqual([0x01]);
  });

  it('refuses what it cannot read', () => {
    expect(base38Decode('abc')).toBeNull(); // three characters: no such chunk
    expect(base38Decode('abcde!')).toBeNull(); // '!' is not in the alphabet
    expect(base38Decode('ZZZZZ')).toBeNull(); // the number does not fit in three bytes
  });
});

describe('the QR code of a Matter device', () => {
  it('reads the fields out of the example', () => {
    const payload = decodeMatterQr(SPEC_QR);
    expect(payload).not.toBeNull();
    expect(payload?.version).toBe(0);
    expect(payload?.vendorId).toBe(0xfff1);
    expect(payload?.productId).toBe(0x8000);
    expect(payload?.commissioningFlow).toBe(0);
    expect(payload?.rendezvousInformation).toBe(2); // Bluetooth LE
    expect(payload?.discriminator).toBe(3840);
    expect(payload?.setupPinCode).toBe(20202021);
    expect(payload?.payload).toEqual(SPEC_BYTES);
  });

  it('writes a QR code a reader can read back', () => {
    const text = `MT:${base38Encode(SPEC_BYTES)}`;
    expect(decodeMatterQr(text)?.setupPinCode).toBe(20202021);
  });

  it('finds the payload in a longer text', () => {
    expect(decodeMatterQr(`scan me: ${SPEC_QR} thanks`)?.discriminator).toBe(3840);
  });

  it('refuses what is not a QR code', () => {
    expect(decodeMatterQr('')).toBeNull();
    expect(decodeMatterQr('hello')).toBeNull();
    expect(decodeMatterQr('MT:!!!')).toBeNull();
    // Too short: five characters carry three bytes, not the eleven we need.
    expect(decodeMatterQr('MT:00000')).toBeNull();
  });
});

describe('the commissioning service data', () => {
  it('reads version, discriminator and setup hash', () => {
    const data = decodeMatterServiceData([0x00, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04]);
    expect(data?.version).toBe(0);
    expect(data?.discriminator).toBe(3840);
    expect(data?.setupHash).toBe('01020304');
  });

  it('refuses a length it does not know', () => {
    expect(decodeMatterServiceData([0x00, 0x00, 0x0f, 0x01, 0x02, 0x03])).toBeNull();
    expect(decodeMatterServiceData([0x00, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04, 0x05])).toBeNull();
  });
});

describe('the enhanced commissioning advertisement', () => {
  // opcode(1) | version(1) | vendor(2) | product(2) | discriminator(2) | hash(4) | attestation
  const full = [0x00, 0x01, 0xf1, 0xff, 0x00, 0x80, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04, 0xaa, 0xbb];

  it('reads the fields out of the long form', () => {
    const data = decodeMatterManufacturerData(full);
    expect(data?.version).toBe(1);
    expect(data?.vendorId).toBe(0xfff1);
    expect(data?.productId).toBe(0x8000);
    expect(data?.discriminator).toBe(3840);
    expect(data?.setupHash).toBe('01020304');
    expect(data?.attestation).toBe('aabb');
  });

  it('reads the fields out of the form without the opcode', () => {
    const data = decodeMatterManufacturerData(full.slice(1, 12));
    expect(data?.version).toBe(1);
    expect(data?.vendorId).toBe(0xfff1);
    expect(data?.productId).toBe(0x8000);
    expect(data?.discriminator).toBe(3840);
    expect(data?.setupHash).toBe('01020304');
    expect(data?.attestation).toBe('');
  });

  it('refuses a blob too short to be one', () => {
    expect(decodeMatterManufacturerData([0x00, 0xf1, 0xff])).toBeNull();
  });
});

describe('what the app hands the hub', () => {
  it('takes the rich advertisement when the device sends one', () => {
    const identity = matterFromAdvertisement({
      serviceData: { fff6: [0x00, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04] },
      manufacturerData: {
        [String(MATTER_ENHANCED_COMPANY_ID)]: [
          0x00, 0x01, 0xf1, 0xff, 0x00, 0x80, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04,
        ],
      },
    });
    expect(identity?.source).toBe('manufacturer');
    expect(identity?.kind).toBe(MATTER_PAYLOAD_MANUFACTURER);
    expect(identity?.discriminator).toBe(3840);
    expect(identity?.setupHash).toBe('01020304');
  });

  it('falls back to the plain service data', () => {
    const identity = matterFromAdvertisement({
      serviceData: { fff6: [0x00, 0x00, 0x0f, 0x01, 0x02, 0x03, 0x04] },
      manufacturerData: {},
    });
    expect(identity?.source).toBe('service');
    expect(identity?.kind).toBe(MATTER_PAYLOAD_SERVICE);
    expect(identity?.discriminator).toBe(3840);
    expect(identity?.payloadHex).toBe('00000f01020304');
  });

  it('says nothing about a device that is not Matter', () => {
    expect(
      matterFromAdvertisement({
        serviceData: { '180f': [0x2c, 0x12] },
        manufacturerData: { '004c': [0x01, 0x02] },
      }),
    ).toBeNull();
  });

  it('takes the payload of a scanned QR code', () => {
    const identity = matterFromQr(SPEC_QR);
    expect(identity?.source).toBe('qr');
    expect(identity?.kind).toBe(MATTER_PAYLOAD_QR);
    expect(identity?.discriminator).toBe(3840);
    expect(identity?.setupHash).toBeNull();
    expect(payloadBytes(identity?.payloadHex ?? '')).toEqual(SPEC_BYTES);
  });

  it('reads a hex string back into bytes', () => {
    expect(payloadBytes('00ff10')).toEqual([0x00, 0xff, 0x10]);
    expect(payloadBytes('0')).toBeNull();
    expect(payloadBytes('zz')).toBeNull();
  });
});

describe('what the hub says about the attempt', () => {
  it('says the attempt is running', () => {
    expect(matterWords({ state: 'scanning', code: 'ok' })).toBe('The hub is adding the device…');
    expect(matterWords({ state: 'commissioning', code: 'ok' })).toBe('The hub is adding the device…');
    expect(matterWords({ state: 'adding', code: 'ok' })).toBe('The hub is adding the device…');
    expect(matterBusy({ state: 'adding', code: 'ok' })).toBe(true);
  });

  it('says the device is in the hub', () => {
    expect(matterWords({ state: 'done', code: 'ok' })).toBe('The device is in the hub now.');
    expect(matterBusy({ state: 'done', code: 'ok' })).toBe(false);
  });

  it('has its own words for every failure, because those are the ones to act on', () => {
    expect(matterWords({ state: 'failed', code: 'no_device' })).toMatch(/did not answer/);
    expect(matterWords({ state: 'failed', code: 'bad_payload' })).toMatch(/QR code/);
    expect(matterWords({ state: 'failed', code: 'no_setup_hash' })).toMatch(/QR code/);
    expect(matterWords({ state: 'failed', code: 'busy' })).toMatch(/busy/);
    expect(matterWords({ state: 'failed', code: 'unsupported' })).toMatch(/cannot add Matter devices/);
    expect(matterWords({ state: 'failed', code: 'timeout' })).toMatch(/closer to the hub/);
  });

  it('falls back to the hub’s own line when there is nothing better to say', () => {
    expect(matterWords({ state: 'failed', code: 'failed', message: 'the chip fell over' })).toBe(
      'the chip fell over',
    );
    expect(matterWords({ state: 'failed', code: 'failed' })).toBe('Adding the device failed.');
    expect(matterWords({ state: 'idle', code: 'ok' })).toBe('');
  });
});
