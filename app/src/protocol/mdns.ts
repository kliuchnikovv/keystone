// Minimal mDNS (RFC 6762) codec, just enough to find the hub: build a PTR query for
// `_keystone._tcp.local` and read PTR/SRV/TXT/A records out of the answers. Pure (no
// sockets), so it is shared by the Node tools and unit-tested; the socket lives in
// tools/discover.ts. mDNS only says *where* a hub is — it is unauthenticated, so trust
// still comes from the pinned certificate fingerprint (docs/app-protocol.md §10).

export const SERVICE = '_keystone._tcp.local';

export type DnsType = 'PTR' | 'SRV' | 'TXT' | 'A' | 'other';

export interface DnsRecord {
  name: string;
  type: DnsType;
  ttl: number;
  // PTR: target name; SRV: port + target; TXT: key=value strings; A: dotted quad.
  ptr?: string;
  srv?: { port: number; target: string };
  txt?: string[];
  a?: string;
}

const TYPE_CODE: Record<Exclude<DnsType, 'other'>, number> = { A: 1, PTR: 12, TXT: 16, SRV: 33 };

export function buildQuery(name: string, type: Exclude<DnsType, 'other'> = 'PTR'): Uint8Array {
  const labels = name.split('.').filter((l) => l.length > 0);
  const enc = new TextEncoder();
  const parts: number[] = [0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]; // id 0, flags 0, qdcount 1
  for (const l of labels) {
    const b = enc.encode(l);
    if (b.length === 0 || b.length > 63) throw new Error(`bad DNS label: ${l}`);
    parts.push(b.length, ...b);
  }
  parts.push(0, 0, TYPE_CODE[type], 0, 1); // QTYPE, QCLASS IN
  return Uint8Array.from(parts);
}

// Reads a (possibly compressed) name at `o`; returns it and the offset after it in the
// original position. Bounded: a pointer loop or an over-long name is an error, not a hang.
function readName(d: Uint8Array, o: number): { name: string; next: number } {
  const labels: string[] = [];
  let next = -1;
  let hops = 0;
  const dec = new TextDecoder();
  for (;;) {
    if (o >= d.length) throw new Error('truncated name');
    const len = d[o]!;
    if (len === 0) {
      o += 1;
      break;
    }
    if ((len & 0xc0) === 0xc0) {
      if (o + 1 >= d.length) throw new Error('truncated pointer');
      if (next < 0) next = o + 2;
      if (++hops > 16) throw new Error('name pointer loop');
      o = ((len & 0x3f) << 8) | d[o + 1]!;
      continue;
    }
    if ((len & 0xc0) !== 0 || o + 1 + len > d.length) throw new Error('bad label');
    labels.push(dec.decode(d.subarray(o + 1, o + 1 + len)));
    if (labels.length > 64) throw new Error('name too long');
    o += 1 + len;
  }
  return { name: labels.join('.'), next: next >= 0 ? next : o };
}

/** Parses the answer/authority/additional records of a packet. Malformed → throws. */
export function parsePacket(d: Uint8Array): DnsRecord[] {
  if (d.length < 12) throw new Error('short packet');
  const u16 = (o: number) => (d[o]! << 8) | d[o + 1]!;
  const qd = u16(4);
  const rrs = u16(6) + u16(8) + u16(10);
  let o = 12;
  for (let i = 0; i < qd; i++) o = readName(d, o).next + 4;
  const out: DnsRecord[] = [];
  const dec = new TextDecoder();
  for (let i = 0; i < rrs; i++) {
    const n = readName(d, o);
    o = n.next;
    if (o + 10 > d.length) throw new Error('truncated record');
    const t = u16(o);
    const ttl = ((d[o + 4]! << 24) | (d[o + 5]! << 16) | (d[o + 6]! << 8) | d[o + 7]!) >>> 0;
    const len = u16(o + 8);
    o += 10;
    if (o + len > d.length) throw new Error('truncated rdata');
    const rec: DnsRecord = { name: n.name, type: 'other', ttl };
    if (t === TYPE_CODE.PTR) {
      rec.type = 'PTR';
      rec.ptr = readName(d, o).name;
    } else if (t === TYPE_CODE.SRV && len >= 7) {
      rec.type = 'SRV';
      rec.srv = { port: u16(o + 4), target: readName(d, o + 6).name };
    } else if (t === TYPE_CODE.TXT) {
      rec.type = 'TXT';
      rec.txt = [];
      for (let p = o; p < o + len; ) {
        const l = d[p]!;
        if (p + 1 + l > o + len) throw new Error('bad TXT');
        if (l > 0) rec.txt.push(dec.decode(d.subarray(p + 1, p + 1 + l)));
        p += 1 + l;
      }
    } else if (t === TYPE_CODE.A && len === 4) {
      rec.type = 'A';
      rec.a = `${d[o]}.${d[o + 1]}.${d[o + 2]}.${d[o + 3]}`;
    }
    out.push(rec);
    o += len;
  }
  return out;
}

export interface DiscoveredHub {
  /** Service instance, e.g. "Keystone hub". */
  instance: string;
  /** The hub's own name from the TXT record (`name=`), or null. */
  name: string | null;
  host: string; // mDNS host name, e.g. keystone.local
  address: string | null; // IPv4 if an A record came along
  port: number;
  tls: boolean;
  /** ws:// or wss:// URL (address preferred over the .local name, which not every OS resolves). */
  url: string;
}

/** Folds records (from any number of packets) into the hubs they describe. */
export function hubsFromRecords(records: readonly DnsRecord[]): DiscoveredHub[] {
  const suffix = `.${SERVICE}`;
  const addr = new Map<string, string>();
  for (const r of records) if (r.type === 'A' && r.a) addr.set(r.name.toLowerCase(), r.a);
  const hubs = new Map<string, DiscoveredHub>();
  for (const r of records) {
    if (r.type !== 'SRV' || !r.srv || !r.name.toLowerCase().endsWith(suffix)) continue;
    if (r.ttl === 0) continue; // goodbye packet
    const instance = r.name.slice(0, r.name.length - suffix.length);
    const txt = records.find((x) => x.type === 'TXT' && x.name === r.name)?.txt ?? [];
    const kv = new Map(txt.map((s) => { const i = s.indexOf('='); return i < 0 ? [s, ''] as const : [s.slice(0, i), s.slice(i + 1)] as const; }));
    if (kv.get('v') !== '1') continue; // a protocol version we do not speak
    const tls = kv.get('tls') === '1';
    const address = addr.get(r.srv.target.toLowerCase()) ?? null;
    hubs.set(r.name, {
      instance, name: kv.get('name') ?? null, host: r.srv.target, address, port: r.srv.port, tls,
      url: `${tls ? 'wss' : 'ws'}://${address ?? r.srv.target}:${r.srv.port}/ws`,
    });
  }
  return [...hubs.values()];
}
