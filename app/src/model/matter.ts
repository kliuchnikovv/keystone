// Matter devices in the air.
//
// The app does not commission anything — it has no Matter stack and cannot hold one.
// What it can do is read what a device says about itself over Bluetooth (or what its QR
// code says) and hand those bytes to the hub, which does have the stack.
//
// The formats, from the Matter specification 1.0 (sections 5.1.2 and 5.1.4) and the CHIP
// implementation of them:
//
//   commissioning service data, UUID 0xFFF6, 7 bytes:
//       version(1) | discriminator(u16 LE) | setup hash(4)
//
//   enhanced commissioning, manufacturer data of company 0xFFF1, 12 bytes + attestation:
//       opcode(1, 0) | version(1) | vendor id(u16 LE) | product id(u16 LE) |
//       discriminator(u16 LE) | setup hash(4) | attestation TLV
//
//   the QR code, "MT:" followed by base38 of 11 bytes (+ an optional TLV):
//       version(3 bits) | vendor id(16) | product id(16) | commissioning flow(2) |
//       rendezvous information(8) | discriminator(12) | setup PIN code(27) | padding(4)
//     — the bits are packed least significant first.
//
// What the app cannot do: commission from the code printed on the label. That code
// carries the discriminator and the setup PIN code but not the setup hash, and without
// the setup hash the hub has no way to prove to the device that it is the commissioner.

/** The Matter commissioning service UUID, as it appears in an advertisement. */
export const MATTER_SERVICE_UUID = 'fff6';

/** The Bluetooth SIG company id Matter uses for the enhanced commissioning advertisement. */
export const MATTER_ENHANCED_COMPANY_ID = 0xfff1;

/** What kind of bytes we are handing the hub — it parses each kind its own way. */
export const MATTER_PAYLOAD_SERVICE = 0;
export const MATTER_PAYLOAD_MANUFACTURER = 1;
export const MATTER_PAYLOAD_QR = 2;

export type MatterPayloadKind = typeof MATTER_PAYLOAD_SERVICE | typeof MATTER_PAYLOAD_MANUFACTURER | typeof MATTER_PAYLOAD_QR;

function hex(bytes: number[]): string {
  return bytes.map((b) => b.toString(16).padStart(2, '0')).join('');
}

function unhex(text: string): number[] | null {
  const clean = text.trim().toLowerCase();
  if (clean.length % 2 !== 0 || !/^[0-9a-f]*$/.test(clean)) return null;
  const out: number[] = [];
  for (let i = 0; i < clean.length; i += 2) out.push(parseInt(clean.slice(i, i + 2), 16));
  return out;
}

/** What the app needs to know about a device it found in the air. */
export interface MatterIdentity {
  /** Where the information came from. */
  source: 'service' | 'manufacturer' | 'qr';
  /** The 12- or 16-bit discriminator, when the source carries one. */
  discriminator: number | null;
  /** The setup hash, when the source carries one (hex). */
  setupHash: string | null;
  /** The bytes to hand to the hub, and what they are. */
  kind: MatterPayloadKind;
  payload: number[];
  /** The payload as hex, for the wire. */
  payloadHex: string;
}

export interface MatterServiceData {
  version: number;
  discriminator: number;
  setupHash: string;
}

export interface MatterManufacturerData {
  version: number;
  vendorId: number;
  productId: number;
  discriminator: number;
  setupHash: string;
  /** The attestation TLV, hex. Empty when the device sent none. */
  attestation: string;
}

export interface MatterQrPayload {
  version: number;
  vendorId: number;
  productId: number;
  commissioningFlow: number;
  rendezvousInformation: number;
  discriminator: number;
  setupPinCode: number;
  /** The raw bytes, 11 plus whatever TLV followed. */
  payload: number[];
}

/** The commissioning service data: version(1) | discriminator(u16 LE) | setup hash(4). */
export function decodeMatterServiceData(bytes: number[]): MatterServiceData | null {
  if (bytes.length !== 7) return null;
  return {
    version: bytes[0] ?? 0,
    discriminator: (bytes[1] ?? 0) | ((bytes[2] ?? 0) << 8),
    setupHash: hex(bytes.slice(3, 7)),
  };
}

/**
 * The enhanced commissioning advertisement: opcode(1) | version(1) | vendor(2) |
 * product(2) | discriminator(2) | setup hash(4) | attestation.
 *
 * The specification has the opcode in front, and a few stacks leave it out. The app
 * only needs the discriminator to show the person what it found — the bytes it hands
 * the hub are untouched, and the hub's Matter stack is the one that parses them for
 * real.
 */
export function decodeMatterManufacturerData(bytes: number[]): MatterManufacturerData | null {
  if (bytes.length < 11) return null;
  // The opcode is 0x00 and the version behind it is 0x01; anything else is read as
  // the form without the opcode.
  const hasOpcode = bytes.length >= 12 && bytes[0] === 0x00 && bytes[1] === 0x01;
  const b = hasOpcode ? bytes.slice(1) : bytes;
  if (b.length < 11) return null;
  return {
    version: b[0] ?? 0,
    vendorId: (b[1] ?? 0) | ((b[2] ?? 0) << 8),
    productId: (b[3] ?? 0) | ((b[4] ?? 0) << 8),
    discriminator: (b[5] ?? 0) | ((b[6] ?? 0) << 8),
    setupHash: hex(b.slice(7, 11)),
    attestation: hex(b.slice(11)),
  };
}

// ---- base38 ---------------------------------------------------------------------------
// The alphabet Matter chose so a QR code stays readable by a person and by a camera.

const BASE38 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.';
const BASE38_VALUES = new Map<string, number>([...BASE38].map((c, i) => [c, i]));

/**
 * Decode base38 the way CHIP does: three bytes at a time as a little-endian number,
 * five characters out, the least significant character first. Two bytes become four
 * characters, one byte becomes two.
 */
export function base38Decode(text: string): number[] | null {
  const chars = text.replace(/\s/g, '').toUpperCase();
  const out: number[] = [];
  let index = 0;
  let left = chars.length;
  while (left > 0) {
    let nChars: number;
    let nBytes: number;
    if (left >= 5) {
      nChars = 5;
      nBytes = 3;
    } else if (left === 4) {
      nChars = 4;
      nBytes = 2;
    } else if (left === 2) {
      nChars = 2;
      nBytes = 1;
    } else {
      return null; // a length base38 cannot produce
    }
    let value = 0;
    for (let k = nChars; k >= 1; k--) {
      const c = chars[index + k - 1];
      if (c === undefined) return null;
      const v = BASE38_VALUES.get(c);
      if (v === undefined) return null;
      value = value * 38 + v;
    }
    index += nChars;
    left -= nChars;
    for (let b = 0; b < nBytes; b++) {
      out.push(value & 0xff);
      value = Math.floor(value / 256);
    }
    if (value > 0) return null; // the chunk did not fit
  }
  return out;
}

/** Read `bits` bits from `bytes`, least significant bit first, as CHIP packs them. */
function readBits(bytes: number[], from: number, bits: number): number | null {
  if (from + bits > bytes.length * 8) return null;
  let value = 0;
  for (let i = 0; i < bits; i++) {
    const bit = ((bytes[(from + i) >> 3] ?? 0) >> ((from + i) & 7)) & 1;
    value |= bit << i;
  }
  return value;
}

/**
 * The payload of a Matter QR code: "MT:" plus base38. The code may carry more than one
 * chunk separated by "%"; the first one is the payload proper.
 */
export function decodeMatterQr(text: string): MatterQrPayload | null {
  const match = /MT:([0-9A-Za-z\-.]+)/.exec(text.trim().toUpperCase());
  const encoded = match?.[1];
  if (!encoded) return null;
  const bytes = base38Decode(encoded);
  if (!bytes || bytes.length < 11) return null;

  let at = 0;
  const take = (bits: number): number | null => {
    const v = readBits(bytes, at, bits);
    if (v === null) return null;
    at += bits;
    return v;
  };
  const version = take(3);
  const vendorId = take(16);
  const productId = take(16);
  const commissioningFlow = take(2);
  const rendezvousInformation = take(8);
  const discriminator = take(12);
  const setupPinCode = take(27);
  const padding = take(4);
  if (
    version === null ||
    vendorId === null ||
    productId === null ||
    commissioningFlow === null ||
    rendezvousInformation === null ||
    discriminator === null ||
    setupPinCode === null ||
    padding === null
  ) {
    return null;
  }
  if (padding !== 0) return null; // the padding bits are always zero
  return {
    version,
    vendorId,
    productId,
    commissioningFlow,
    rendezvousInformation,
    discriminator,
    setupPinCode,
    payload: bytes,
  };
}

/** The QR text of a payload, for the tests and for showing a person what they scanned. */
export function encodeMatterQr(payload: MatterQrPayload): string {
  // Pack the fields the way CHIP does, least significant bit first.
  const bits: number[] = [];
  const push = (value: number, count: number) => {
    for (let i = 0; i < count; i++) bits.push((value >> i) & 1);
  };
  push(payload.version, 3);
  push(payload.vendorId, 16);
  push(payload.productId, 16);
  push(payload.commissioningFlow, 2);
  push(payload.rendezvousInformation, 8);
  push(payload.discriminator, 12);
  push(payload.setupPinCode, 27);
  push(0, 4);
  const bytes: number[] = [];
  for (let i = 0; i < bits.length; i += 8) {
    let byte = 0;
    for (let b = 0; b < 8; b++) byte |= (bits[i + b] ?? 0) << b;
    bytes.push(byte);
  }
  return `MT:${base38Encode(bytes)}`;
}

/** The other direction of base38Decode: three bytes at a time, five characters out. */
export function base38Encode(bytes: number[]): string {
  const out: string[] = [];
  let i = 0;
  while (i < bytes.length) {
    const n = Math.min(3, bytes.length - i);
    let value = 0;
    for (let b = 0; b < n; b++) value += ((bytes[i + b] ?? 0) & 0xff) << (8 * b);
    i += n;
    const chars = n === 3 ? 5 : n === 2 ? 4 : 2;
    for (let c = 0; c < chars; c++) {
      out.push(BASE38.charAt(value % 38));
      value = Math.floor(value / 38);
    }
  }
  return out.join('');
}

// ---- what an advertisement says --------------------------------------------------------

/** The part of a BLE advertisement this file needs; the native type satisfies it. */
export interface AdvertisementLike {
  serviceData: Record<string, number[]>;
  manufacturerData: Record<string, number[]>;
}

/**
 * What a device says about itself, if it says anything Matter-ish.
 *
 * The enhanced advertisement is preferred: it carries the vendor, the product and the
 * attestation next to the setup hash, which is what the hub needs to commission the
 * device. The plain service data is the fallback — it identifies the device and gives
 * the setup hash, but says less.
 */
export function matterFromAdvertisement(a: AdvertisementLike): MatterIdentity | null {
  const enhanced = a.manufacturerData?.[String(MATTER_ENHANCED_COMPANY_ID)];
  if (enhanced) {
    const decoded = decodeMatterManufacturerData(enhanced);
    if (decoded) {
      return {
        source: 'manufacturer',
        discriminator: decoded.discriminator,
        setupHash: decoded.setupHash,
        kind: MATTER_PAYLOAD_MANUFACTURER,
        payload: enhanced,
        payloadHex: hex(enhanced),
      };
    }
  }
  const service = a.serviceData?.[MATTER_SERVICE_UUID];
  if (service) {
    const decoded = decodeMatterServiceData(service);
    if (decoded) {
      return {
        source: 'service',
        discriminator: decoded.discriminator,
        setupHash: decoded.setupHash,
        kind: MATTER_PAYLOAD_SERVICE,
        payload: service,
        payloadHex: hex(service),
      };
    }
  }
  return null;
}

/** What a scanned QR code gives us: the payload, and the discriminator in it. */
export function matterFromQr(text: string): MatterIdentity | null {
  const decoded = decodeMatterQr(text);
  if (!decoded) return null;
  return {
    source: 'qr',
    discriminator: decoded.discriminator,
    setupHash: null,
    kind: MATTER_PAYLOAD_QR,
    payload: decoded.payload,
    payloadHex: hex(decoded.payload),
  };
}

/** The bytes of a hex string, for the wire and for the tests. */
export function payloadBytes(hexText: string): number[] | null {
  return unhex(hexText);
}

// ---- what the hub says about the attempt ------------------------------------------------

/** The hub's answer about the attempt to add a Matter device, in the app's own words. */
export interface MatterAttempt {
  state: 'idle' | 'scanning' | 'commissioning' | 'adding' | 'done' | 'failed';
  code:
    | 'ok'
    | 'no_device'
    | 'bad_payload'
    | 'no_setup_hash'
    | 'busy'
    | 'unsupported'
    | 'timeout'
    | 'failed';
  message?: string;
}

/**
 * What to show about the attempt. The hub sends a short line of its own; the app has its
 * own words for the failures, because those are the ones a person has to act on.
 */
export function matterWords(attempt: MatterAttempt): string {
  switch (attempt.state) {
    case 'done':
      return 'The device is in the hub now.';
    case 'scanning':
    case 'commissioning':
    case 'adding':
      return 'The hub is adding the device…';
    case 'idle':
      return '';
    case 'failed':
      break;
  }
  switch (attempt.code) {
    case 'no_device':
      return 'The device did not answer. Wake it up and try again.';
    case 'bad_payload':
      return 'The hub did not understand what the phone read. Use the QR code on the device.';
    case 'no_setup_hash':
      return 'The device did not share its setup code. Use the QR code on the device.';
    case 'busy':
      return 'The hub is busy with another device. Try again in a moment.';
    case 'unsupported':
      return 'This hub cannot add Matter devices yet.';
    case 'timeout':
      return 'The device did not answer in time. Move it closer to the hub and try again.';
    case 'ok':
    case 'failed':
      return attempt.message && attempt.message.length > 0 ? attempt.message : 'Adding the device failed.';
  }
}

/** Is the attempt still running? */
export function matterBusy(attempt: MatterAttempt): boolean {
  return attempt.state === 'scanning' || attempt.state === 'commissioning' || attempt.state === 'adding';
}

