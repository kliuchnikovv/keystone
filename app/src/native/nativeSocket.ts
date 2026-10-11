// A SocketFactory over the native module: the way a phone pins the hub's certificate.
import type { PinMismatchEvent, SocketFactory, SocketLike } from '../protocol/client';
import { normalizeFingerprint, sameFingerprint } from '../protocol/fingerprint';
import type { KeystoneNetModule, NativeSocketEvent, NativeSubscription } from './types';

class NativeSocket implements SocketLike {
  onopen: SocketLike['onopen'] = null;
  onmessage: SocketLike['onmessage'] = null;
  onclose: SocketLike['onclose'] = null;
  onerror: SocketLike['onerror'] = null;
  private done = false;

  constructor(
    private readonly mod: KeystoneNetModule,
    private readonly id: string,
    private readonly pin: string,
    private readonly release: (id: string) => void,
  ) {}

  handle(e: NativeSocketEvent): void {
    if (this.done) return;
    switch (e.kind) {
      case 'open': {
        // The platform already refused any other certificate; checking the fingerprint it
        // reports again costs nothing and means a bug in one native file cannot silently
        // turn pinning off (an `open` without a matching fingerprint is treated as a mismatch).
        const seen = e.fingerprint ? normalizeFingerprint(e.fingerprint) : null;
        if (!seen || !sameFingerprint(seen, this.pin)) {
          this.fail({ pinMismatch: true, ...(seen ? { seen } : {}) });
          return;
        }
        this.onopen?.({});
        return;
      }
      case 'message':
        this.onmessage?.({ data: e.data });
        return;
      case 'error': {
        if (e.pinMismatch) {
          const seen = e.seen ? normalizeFingerprint(e.seen) : null;
          const ev: PinMismatchEvent = { pinMismatch: true, ...(seen ? { seen } : {}) };
          this.onerror?.(ev);
        } else {
          this.onerror?.(new Error(e.message ?? 'socket error'));
        }
        return; // the native side follows an error with `close`
      }
      case 'close':
        this.finish();
        return;
    }
  }

  private fail(ev: PinMismatchEvent): void {
    this.onerror?.(ev);
    this.mod.close(this.id);
    this.finish();
  }

  private finish(): void {
    if (this.done) return;
    this.done = true;
    this.release(this.id);
    this.onclose?.({});
  }

  send(data: string): void {
    if (!this.done) this.mod.send(this.id, data);
  }

  close(): void {
    if (this.done) return;
    this.mod.close(this.id);
    this.finish();
  }
}

/**
 * HubClient socket factory backed by the native module. With a fingerprint it connects only to
 * a server presenting exactly that certificate; without one it refuses (the stock
 * WebSocket is the right tool for ws:// and for hubs with a CA-signed certificate).
 */
export function createNativeSocketFactory(mod: KeystoneNetModule): SocketFactory {
  const sockets = new Map<string, NativeSocket>();
  let sub: NativeSubscription | null = null;
  let next = 0;

  const release = (id: string) => {
    sockets.delete(id);
    if (sockets.size === 0 && sub) {
      sub.remove();
      sub = null;
    }
  };

  return (url, opts) => {
    const pin = opts.fingerprint ? normalizeFingerprint(opts.fingerprint) : null;
    if (!pin) throw new Error('the native socket needs a SHA-256 fingerprint to pin');
    if (!/^wss:\/\//i.test(url)) throw new Error('the native socket is for wss:// hubs');
    if (!sub) sub = mod.addListener('onSocketEvent', (e) => sockets.get(e.id)?.handle(e));
    const id = `s${++next}`;
    const sock = new NativeSocket(mod, id, pin, release);
    sockets.set(id, sock);
    mod.connect(id, url, pin);
    return sock;
  };
}
