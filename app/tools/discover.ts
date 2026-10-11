// Finds hubs on the LAN by mDNS (service _keystone._tcp):
//   npm run discover [-- --wait 3]
// Prints one line per hub with its URL. Says nothing about trust: pin the certificate
// fingerprint printed by `webtoken` on the hub console.
import dgram from 'node:dgram';
import { buildQuery, hubsFromRecords, parsePacket, SERVICE, type DnsRecord } from '../src/protocol/mdns';

const i = process.argv.indexOf('--wait');
const waitMs = Math.max(1, Number(i >= 0 ? process.argv[i + 1] : 3)) * 1000;

const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
const records: DnsRecord[] = [];
sock.on('message', (m) => {
  try { records.push(...parsePacket(m)); } catch { /* not ours / malformed: ignore */ }
});
sock.on('error', (e) => { console.error(`mDNS socket: ${e.message}`); process.exit(1); });
sock.bind(5353, () => {
  try { sock.addMembership('224.0.0.251'); } catch (e) { console.error(`cannot join the mDNS group: ${(e as Error).message}`); process.exit(1); }
  const q = buildQuery(SERVICE, 'PTR');
  sock.send(q, 5353, '224.0.0.251');
  setTimeout(() => sock.send(q, 5353, '224.0.0.251'), 800); // one retry: packets get lost
  setTimeout(() => {
    sock.close();
    const hubs = hubsFromRecords(records);
    if (hubs.length === 0) { console.log('no hubs found'); process.exit(1); }
    for (const h of hubs) console.log(`${h.url}  name=${h.name ?? '?'}  host=${h.host}  ${h.tls ? 'TLS' : 'plain (dev)'}`);
    process.exit(0);
  }, waitMs);
});
