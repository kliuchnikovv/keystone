// A Node socket factory for HubClient that pins the hub's certificate by SHA-256
// fingerprint. A self-signed hub certificate fails normal verification, so verification
// is replaced by the pin: the TLS handshake is allowed, but `open` is withheld from the
// client until the peer certificate's fingerprint matches, so the token is never sent to
// a server that does not hold the pinned certificate.
import { connect } from 'node:tls';
import { WebSocket } from 'ws';
import { sameFingerprint, normalizeFingerprint } from '../src/protocol/fingerprint';
import type { PinMismatchEvent, SocketFactory, SocketLike } from '../src/protocol/client';

/** The fingerprint a server presents (for trust-on-first-use: show it, let a person compare). */
export function fetchFingerprint(host: string, port: number, timeoutMs = 5000): Promise<string> {
  return new Promise((resolve, reject) => {
    const s = connect({ host, port, rejectUnauthorized: false, servername: host }, () => {
      const fp = s.getPeerCertificate().fingerprint256;
      s.end();
      const n = normalizeFingerprint(fp ?? '');
      n ? resolve(n) : reject(new Error('no certificate presented'));
    });
    s.setTimeout(timeoutMs, () => {
      s.destroy();
      reject(new Error('timed out'));
    });
    s.on('error', reject);
  });
}

export interface TlsInfo {
  fingerprint: string;
  protocol: string | null;
  cipher: string | null;
  validFrom: Date;
  validTo: Date;
}

/** What a TLS server presents and negotiates (for checks and for showing a person). */
export function tlsInfo(host: string, port: number, timeoutMs = 5000): Promise<TlsInfo> {
  return new Promise((resolve, reject) => {
    const s = connect({ host, port, rejectUnauthorized: false, servername: host }, () => {
      const c = s.getPeerCertificate();
      const info: TlsInfo = {
        fingerprint: normalizeFingerprint(c.fingerprint256 ?? '') ?? '',
        protocol: s.getProtocol(),
        cipher: s.getCipher()?.name ?? null,
        validFrom: new Date(c.valid_from),
        validTo: new Date(c.valid_to),
      };
      s.end();
      resolve(info);
    });
    s.setTimeout(timeoutMs, () => {
      s.destroy();
      reject(new Error('timed out'));
    });
    s.on('error', reject);
  });
}

class PinnedSocket implements SocketLike {
  onopen: SocketLike['onopen'] = null;
  onmessage: SocketLike['onmessage'] = null;
  onclose: SocketLike['onclose'] = null;
  onerror: SocketLike['onerror'] = null;
  private ws: WebSocket;

  constructor(url: string, fingerprint?: string, learn = false) {
    const secure = /^wss:/i.test(url);
    // Verification is by pin, not by CA, so the chain error of a self-signed certificate is expected.
    this.ws = new WebSocket(url, secure ? { rejectUnauthorized: false } : {});
    this.ws.on('open', () => {
      if (secure) {
        const seen = (this.ws as unknown as { _socket?: { getPeerCertificate(): { fingerprint256?: string } } })._socket?.getPeerCertificate().fingerprint256;
        if (learn) {
          // First-time setup: take what the hub presents and say what it was.
          this.onopen?.({ fingerprint: seen ? (normalizeFingerprint(seen) ?? undefined) : undefined });
          return;
        }
        if (!fingerprint || !seen || !sameFingerprint(seen, fingerprint)) {
          const ev: PinMismatchEvent = { pinMismatch: true, seen: seen ? (normalizeFingerprint(seen) ?? undefined) : undefined };
          this.onerror?.(ev);
          this.ws.terminate();
          return;
        }
      }
      this.onopen?.({});
    });
    this.ws.on('message', (d) => this.onmessage?.({ data: d.toString() }));
    this.ws.on('close', () => this.onclose?.({}));
    this.ws.on('error', (e) => this.onerror?.(e));
  }
  send(data: string): void {
    this.ws.send(data);
  }
  close(): void {
    this.ws.close();
  }
}

/** HubClient socket factory: ws:// as is, wss:// only with a matching `fingerprint` (or `learn`: take any and report it). */
export const pinnedSocketFactory: SocketFactory = (url, opts) => new PinnedSocket(url, opts.fingerprint, opts.learn === true);
