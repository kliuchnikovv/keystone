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
  addListener(event: 'onSocketEvent', listener: (e: NativeSocketEvent) => void): NativeSubscription;
  addListener(event: 'onHubFound', listener: (h: NativeHub) => void): NativeSubscription;
  addListener(event: 'onHubLost', listener: (h: { instance: string }) => void): NativeSubscription;
}
