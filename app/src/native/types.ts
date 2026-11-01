// The contract between the app and the native module in modules/keystone-net (Kotlin on
// Android, Swift on iOS). The module exists because the stock React Native WebSocket can
// neither pin a certificate nor accept a self-signed one, and a browser-style API has no
// mDNS. Keep this in step with both native files.

/** One message from a native socket. `open` is only ever sent AFTER the platform verified
 *  the pinned certificate (or, for `connectLearning`, after the handshake), and carries the
 *  fingerprint it saw (checked again in JS). */
export type NativeSocketEvent =
  | { id: string; kind: 'open'; fingerprint?: string }
  | { id: string; kind: 'message'; data: string }
  | { id: string; kind: 'close' }
  | { id: string; kind: 'error'; message?: string; pinMismatch?: boolean; seen?: string | null };

export interface NativeHub {
  /** Service instance name, unique per hub on the LAN. */
  instance: string;
  /** The hub's own name (TXT `name`). */
  name: string | null;
  host: string;
  address: string | null;
  port: number;
  tls: boolean;
}

export interface NativeSubscription {
  remove(): void;
}

/** What the Bluetooth stack says about itself. */
export type BleState =
  | 'unknown'
  | 'resetting'
  | 'unsupported'
  | 'unauthorized'
  | 'poweredOff'
  | 'ready';

export type BleAuthorization = 'notDetermined' | 'allowed' | 'denied' | 'restricted';

/** One device seen in the air. */
export interface NativeBleAdvertisement {
  /** The system's id for this peripheral — stable while it is around. */
  id: string;
  /** The signal strength. iOS reports 127 when it has no reading. */
  rssi: number;
  /** The name the device advertises, when it advertises one. */
  name: string | null;
  /** The 16-bit service UUIDs it advertises, lower case, without dashes. */
  serviceUuids: string[];
  /** The service data, keyed by the UUID in lower case. */
  serviceData: Record<string, number[]>;
  /** The manufacturer data, keyed by the company id as a decimal string. */
  manufacturerData: Record<string, number[]>;
}

export interface NativeBleStatus {
  state: BleState;
  authorization: BleAuthorization;
}

export interface KeystoneNetModule {
  /** Open `url` (wss://) trusting only the certificate whose SHA-256 is `fingerprint`. */
  connect(id: string, url: string, fingerprint: string): void;
  /**
   * Open `url` (wss://) accepting whatever certificate the server presents, and report its SHA-256 in `open`.
   * Only for the first-time setup over the hub's own password-protected access point (src/protocol/setup.ts).
   */
  connectLearning(id: string, url: string): void;
  send(id: string, text: string): void;
  close(id: string): void;
  startDiscovery(): void;
  stopDiscovery(): void;
  /** Start looking for Bluetooth devices around the phone. */
  startBleScan(): void;
  /** Stop looking. */
  stopBleScan(): void;
  /** What the Bluetooth stack says about itself, without starting a scan. */
  bleStatus(): NativeBleStatus;
  addListener(event: 'onSocketEvent', listener: (e: NativeSocketEvent) => void): NativeSubscription;
  addListener(event: 'onHubFound', listener: (h: NativeHub) => void): NativeSubscription;
  addListener(event: 'onHubLost', listener: (h: { instance: string }) => void): NativeSubscription;
  addListener(event: 'onBleAdvertisement', listener: (a: NativeBleAdvertisement) => void): NativeSubscription;
  addListener(event: 'onBleState', listener: (s: NativeBleStatus) => void): NativeSubscription;
}
