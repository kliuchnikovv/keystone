import { describe, expect, it } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import { canUpdateFirmware, otaInputProblem, otaPhaseLabel, otaStatusLabel } from '../src/model/devices';
import { HubClient, type SocketLike } from '../src/protocol/client';
import type { ClientMessage, ServerMessage } from '../src/protocol/messages';
import { cleared, deviceOtaState, initialState, reduce } from '../src/protocol/store';
import { MockHub } from '../tools/mock-hub/hub';

const SHA = 'a'.repeat(64);
const URL = 'https://example.com/lamp.ota';

async function until(fn: () => boolean, what: string, ms = 3000): Promise<void> {
  const t0 = Date.now();
  while (!fn()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for: ${what}`);
    await new Promise((r) => setTimeout(r, 10));
  }
}

/** A socket that keeps what the client sent and answers the requests itself. */
class FakeSocket implements SocketLike {
  onopen: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: ((ev: unknown) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  readonly sent: ClientMessage[] = [];
  private readonly reply: (msg: ClientMessage) => ServerMessage | null;

  constructor(reply: (msg: ClientMessage) => ServerMessage | null = () => null) {
    this.reply = reply;
  }

  /** The client says hello once the socket is open. */
  open(): void {
    this.onopen?.({});
  }

  deliver(m: ServerMessage): void {
    this.onmessage?.({ data: JSON.stringify(m) });
  }

  send(data: string): void {
    const m = JSON.parse(data) as ClientMessage;
    this.sent.push(m);
    const r = this.reply(m);
    if (r) this.deliver(r);
  }

  close(): void {
    this.onclose?.({});
  }
}

/** A client that is connected and ready to be asked things. */
async function readyClient(sock: FakeSocket): Promise<HubClient> {
  const client = new HubClient({
    url: 'ws://hub.local/ws',
    token: 'token-token',
    socketFactory: () => sock,
    reconnect: false,
    requestTimeoutMs: 2000,
  });
  client.connect();
  sock.open();
  sock.deliver({ type: 'welcome', v: 1, hub: { name: 'h', fw: 'x', time: 0, tz: 'UTC', synced: false } });
  sock.deliver({ type: 'snapshot', entities: [], chips: [], rules: [], schedules: [], timers: [] });
  await new Promise((r) => setTimeout(r, 0));
  expect(client.getSnapshot().status).toBe('ready');
  return client;
}

function theRequest(sock: FakeSocket): Extract<ClientMessage, { type: 'req' }> {
  const r = sock.sent.find((m) => m.type === 'req');
  if (!r) throw new Error('the client sent no request');
  return r;
}

describe('the firmware session in the state', () => {
  it('a device_ota frame lands in the state', () => {
    const s = reduce(initialState, { type: 'device_ota', entity: 0x0100, phase: 'transferring', status: 'ok', percent: 42 });
    expect(s.deviceOta[0x0100]).toEqual({ phase: 'transferring', status: 'ok', percent: 42 });
    expect(deviceOtaState(s, 0x0100)?.percent).toBe(42);
    expect(deviceOtaState(s, 0x0101)).toBeUndefined();
  });

  it('a later frame replaces the session of that device only', () => {
    let s = reduce(initialState, { type: 'device_ota', entity: 0x0100, phase: 'receiving', status: 'ok', percent: 5 });
    s = reduce(s, { type: 'device_ota', entity: 0x0101, phase: 'receiving', status: 'ok', percent: 1 });
    s = reduce(s, { type: 'device_ota', entity: 0x0100, phase: 'done', status: 'ok', percent: 100 });
    expect(s.deviceOta[0x0100]).toEqual({ phase: 'done', status: 'ok', percent: 100 });
    expect(s.deviceOta[0x0101]).toEqual({ phase: 'receiving', status: 'ok', percent: 1 });
  });

  it('the same frame again changes nothing', () => {
    const s = reduce(initialState, { type: 'device_ota', entity: 0x0100, phase: 'done', status: 'ok', percent: 100 });
    expect(reduce(s, { type: 'device_ota', entity: 0x0100, phase: 'done', status: 'ok', percent: 100 })).toBe(s);
  });

  it('cleared() drops the firmware session', () => {
    const s = reduce(initialState, { type: 'device_ota', entity: 0x0100, phase: 'transferring', status: 'ok', percent: 42 });
    expect(cleared(s).deviceOta).toEqual({});
    expect(deviceOtaState(cleared(s), 0x0100)).toBeUndefined();
  });
});

describe('asking the hub for a firmware update', () => {
  it('sends the device.ota request with the entity, the url and the checksum', async () => {
    const sock = new FakeSocket((m) => (m.type === 'req' ? { type: 'res', id: m.id, ok: true } : null));
    const client = await readyClient(sock);
    await expect(client.deviceOta(0x0100, URL, SHA)).resolves.toEqual({});
    const req = theRequest(sock);
    expect(req.op).toBe('device.ota');
    expect(req.args).toEqual({ entity: 0x0100, url: URL, sha256: SHA });
    client.close();
  });

  it('the hub refusing the update rejects with its code', async () => {
    const sock = new FakeSocket((m) =>
      m.type === 'req' ? { type: 'res', id: m.id, ok: false, code: 'busy', message: 'an update is already running' } : null,
    );
    const client = await readyClient(sock);
    await expect(client.deviceOta(0x0100, URL, SHA)).rejects.toMatchObject({ code: 'busy' });
    client.close();
  });

  it('the request needs a connection', async () => {
    const client = new HubClient({ url: 'ws://hub.local/ws', token: 'token-token', socketFactory: () => new FakeSocket(), reconnect: false });
    await expect(client.deviceOta(0x0100, URL, SHA)).rejects.toMatchObject({ code: 'disconnected' });
  });

  it('against a hub that speaks the op, the progress lands in the state', async () => {
    const hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 });
    const client = new HubClient({
      url: hub.url,
      token: hub.token,
      socketFactory: (u) => new NodeWebSocket(u) as unknown as SocketLike,
      reconnect: false,
      requestTimeoutMs: 2000,
    });
    try {
      client.connect();
      await until(() => client.getSnapshot().status === 'ready', 'the hub is ready');
      await client.deviceOta(0x0101, URL, SHA); // the lamp
      await until(() => client.getSnapshot().deviceOta[0x0101]?.phase === 'done', 'the update to finish');
      expect(client.getSnapshot().deviceOta[0x0101]).toEqual({ phase: 'done', status: 'ok', percent: 100 });
    } finally {
      client.close();
      await hub.close();
    }
  });
});

describe('what the app says about a firmware update', () => {
  it('only a device on the Zigbee chip can be updated', () => {
    const zigbee = { id: 0x0100, name: null, chip: 1, device: 'light', attrs: [] };
    const matter = { id: 0x0200, name: null, chip: 2, device: 'light', attrs: [] };
    expect(canUpdateFirmware(zigbee)).toBe(true);
    expect(canUpdateFirmware(matter)).toBe(false);
  });

  it('the address and the checksum are checked before anything is sent', () => {
    expect(otaInputProblem(URL, SHA)).toBeNull();
    expect(otaInputProblem('', SHA)).toMatch(/address/i);
    expect(otaInputProblem('ftp://example.com/lamp.ota', SHA)).toMatch(/http/);
    expect(otaInputProblem('example.com/lamp.ota', SHA)).toMatch(/http/);
    expect(otaInputProblem(URL, '')).toMatch(/SHA-256/);
    expect(otaInputProblem(URL, 'abc')).toMatch(/64 hex digits/);
    expect(otaInputProblem(URL, 'z'.repeat(64))).toMatch(/64 hex digits/);
    expect(otaInputProblem(URL, SHA.toUpperCase())).toBeNull();
  });

  it('every phase and status reads like a sentence', () => {
    expect(otaPhaseLabel('receiving')).toMatch(/sending/i);
    expect(otaPhaseLabel('verifying')).toMatch(/verifying/i);
    expect(otaPhaseLabel('transferring')).toMatch(/installing/i);
    expect(otaPhaseLabel('done')).toBe('Done');
    expect(otaPhaseLabel('failed')).toBe('Failed');
    expect(otaStatusLabel('bad_checksum')).toMatch(/SHA-256/);
    expect(otaStatusLabel('too_large')).toMatch(/too big/i);
    expect(otaStatusLabel('ok')).toBeTruthy();
  });
});
