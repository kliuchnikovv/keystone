import { describe, expect, it } from 'vitest';
import { buildQuery, hubsFromRecords, parsePacket, SERVICE, type DnsRecord } from '../src/protocol/mdns';

// A tiny response builder (the reverse of parsePacket) so the tests exercise real wire bytes,
// including name compression like the hub's responder uses.
function name(n: string): number[] {
  return [...n.split('.').flatMap((l) => [l.length, ...new TextEncoder().encode(l)]), 0];
}
function rr(n: number[], type: number, ttl: number, rdata: number[]): number[] {
  return [...n, 0, type, 0x80, 1, (ttl >>> 24) & 255, (ttl >>> 16) & 255, (ttl >>> 8) & 255, ttl & 255, rdata.length >> 8, rdata.length & 255, ...rdata];
}
function txt(items: string[]): number[] {
  return items.flatMap((s) => [s.length, ...new TextEncoder().encode(s)]);
}
function packet(rrs: number[][], qd = 0): Uint8Array {
  return Uint8Array.from([0, 0, 0x84, 0, 0, qd, 0, rrs.length, 0, 0, 0, 0, ...rrs.flat()]);
}
const inst = 'Keystone hub._keystone._tcp.local';
function hubPacket(opts: { ttl?: number; tls?: string; v?: string; port?: number } = {}): Uint8Array {
  const ttl = opts.ttl ?? 120;
  return packet([
    rr(name(SERVICE), 12, ttl, name(inst)),
    rr(name(inst), 33, ttl, [0, 0, 0, 0, (opts.port ?? 8443) >> 8, (opts.port ?? 8443) & 255, ...name('keystone.local')]),
    rr(name(inst), 16, ttl, txt([`v=${opts.v ?? '1'}`, 'name=home', `tls=${opts.tls ?? '1'}`])),
    rr(name('keystone.local'), 1, ttl, [192, 168, 0, 202]),
  ]);
}

describe('mdns codec', () => {
  it('builds a PTR query for the service', () => {
    const q = buildQuery(SERVICE);
    expect([...q.subarray(0, 12)]).toEqual([0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
    expect([...q.subarray(12)]).toEqual([...name(SERVICE), 0, 12, 0, 1]);
    expect(() => buildQuery('a'.repeat(64) + '.local')).toThrow();
  });

  it('parses PTR, SRV, TXT and A records', () => {
    const recs = parsePacket(hubPacket());
    expect(recs.map((r) => r.type)).toEqual(['PTR', 'SRV', 'TXT', 'A']);
    expect(recs[0]).toMatchObject({ name: SERVICE, ptr: inst });
    expect(recs[1]!.srv).toEqual({ port: 8443, target: 'keystone.local' });
    expect(recs[2]!.txt).toEqual(['v=1', 'name=home', 'tls=1']);
    expect(recs[3]!.a).toBe('192.168.0.202');
  });

  it('follows name compression pointers', () => {
    // question "x.local", then an A record whose name is a pointer to it (offset 12).
    const d = Uint8Array.from([0, 0, 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 0, ...name('x.local'), 0, 1, 0, 1, 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 10, 0, 0, 1]);
    expect(parsePacket(d)).toMatchObject([{ name: 'x.local', type: 'A', a: '10.0.0.1', ttl: 10 }]);
  });

  it('rejects malformed packets instead of hanging or reading out of bounds', () => {
    const good = hubPacket();
    for (let n = 0; n < good.length - 1; n++) {
      // Every truncation either parses to fewer records or throws; it never loops.
      try { parsePacket(good.subarray(0, n)); } catch { /* expected */ }
    }
    // A pointer that points at itself.
    const loop = Uint8Array.from([0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0]);
    expect(() => parsePacket(loop)).toThrow(/loop/);
    expect(() => parsePacket(Uint8Array.from([1, 2, 3]))).toThrow();
  });
});

describe('hubsFromRecords', () => {
  it('describes a TLS hub, preferring the address over the .local name', () => {
    expect(hubsFromRecords(parsePacket(hubPacket()))).toEqual([
      { instance: 'Keystone hub', name: 'home', host: 'keystone.local', address: '192.168.0.202', port: 8443, tls: true, url: 'wss://192.168.0.202:8443/ws' },
    ]);
  });

  it('a plain hub gives ws://, and falls back to the host name without an A record', () => {
    const recs = parsePacket(hubPacket({ tls: '0', port: 8080 })).filter((r) => r.type !== 'A');
    expect(hubsFromRecords(recs)[0]).toMatchObject({ tls: false, address: null, url: 'ws://keystone.local:8080/ws' });
  });

  it('ignores goodbye packets, other protocol versions and unrelated services', () => {
    expect(hubsFromRecords(parsePacket(hubPacket({ ttl: 0 })))).toEqual([]);
    expect(hubsFromRecords(parsePacket(hubPacket({ v: '2' })))).toEqual([]);
    const other: DnsRecord[] = [{ name: 'Printer._ipp._tcp.local', type: 'SRV', ttl: 120, srv: { port: 631, target: 'p.local' } }];
    expect(hubsFromRecords(other)).toEqual([]);
  });

  it('merges records from several packets and deduplicates', () => {
    const a = parsePacket(hubPacket());
    expect(hubsFromRecords([...a, ...a])).toHaveLength(1);
  });
});
