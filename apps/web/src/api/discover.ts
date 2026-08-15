import { API_BASE } from './client';
import type { CommissioningEvent, DiscoveredDevice } from './types';

const USE_MOCK = import.meta.env.VITE_KEYSTONE_MOCK_DISCOVER === 'true';

/** Замедлитель mock-таймингов для отладки screenshot'ов и UX-полировки. Активируется `?slow=1` или `?fail=1`. */
function mockMultiplier(): number {
  if (typeof window === 'undefined') return 1;
  const p = new URLSearchParams(window.location.search);
  return p.has('slow') || p.has('fail') ? 3 : 1;
}
function mockShouldFail(): boolean {
  if (typeof window === 'undefined') return false;
  return new URLSearchParams(window.location.search).has('fail');
}

export interface DiscoverHandle {
  close: () => void;
}

/** Длительность одного скана. Совпадает с таймаутом, который UI показывает пользователю. */
export const SCAN_SECONDS = 12;

/**
 * Подписка на `GET /discover?transport=matter` (NDJSON).
 * Пока бэк-эндпоинт не готов — включён mock, который эмулирует 3 находки.
 * Отключить: `VITE_KEYSTONE_MOCK_DISCOVER=false yarn dev`.
 */
/** Сырая строка NDJSON от `GET /discover`. */
interface RawFound {
  ref: string;
  name?: string;
  type?: string;
  transport?: string;
  discriminator?: number;
  vendor_id?: number;
  product_id?: number;
}

function toDiscovered(raw: RawFound): DiscoveredDevice {
  return {
    ref: raw.ref,
    // Имя есть не всегда: производитель может ничего не анонсировать.
    name: raw.name && raw.name !== '' ? raw.name : 'Matter-устройство',
    type: raw.type as DiscoveredDevice['type'],
    transport: (raw.transport ?? 'matter') as DiscoveredDevice['transport'],
    discriminator: raw.discriminator,
    vendorId: raw.vendor_id,
    productId: raw.product_id,
  };
}

export function openDiscover(
  onFound: (d: DiscoveredDevice) => void,
  onError?: (e: Error) => void,
): DiscoverHandle {
  if (USE_MOCK) return mockDiscover(onFound);

  const ctrl = new AbortController();
  (async () => {
    try {
      const r = await fetch(`${API_BASE}/discover?transport=matter&timeout=${SCAN_SECONDS}s`, {
        signal: ctrl.signal,
      });
      if (!r.ok || !r.body) throw new Error(`discover ${r.status}`);
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let nl: number;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).trim();
          buf = buf.slice(nl + 1);
          if (!line) continue;
          try {
            onFound(toDiscovered(JSON.parse(line) as RawFound));
          } catch {
            /* skip */
          }
        }
      }
    } catch (e) {
      if ((e as { name?: string }).name !== 'AbortError') onError?.(e as Error);
    }
  })();
  return { close: () => ctrl.abort() };
}

function mockDiscover(onFound: (d: DiscoveredDevice) => void): DiscoverHandle {
  const timers: number[] = [];
  const mult = mockMultiplier();
  const schedule = (ms: number, d: DiscoveredDevice) => {
    timers.push(window.setTimeout(() => onFound(d), ms * mult));
  };
  schedule(1200, {
    ref: 'matter-node-42',
    name: 'WARMBLIXT',
    type: 'light',
    transport: 'matter',
    manufacturer: 'IKEA',
    features: [
      { key: 'onoff', states: ['value'], actions: ['turn_on', 'turn_off', 'toggle'] },
      { key: 'brightness', states: ['level'], actions: ['set'] },
      { key: 'color_temp', states: ['kelvin'], actions: ['set'] },
    ],
  });
  schedule(2400, {
    ref: 'zigbee-plug-7',
    name: 'Умная розетка',
    type: 'plug',
    transport: 'zigbee',
    manufacturer: 'DIRIGERA',
    features: [
      { key: 'onoff', states: ['value'], actions: ['turn_on', 'turn_off'] },
      { key: 'power_meter', states: ['watts'], actions: [] },
    ],
  });
  schedule(3600, {
    ref: 'zigbee-sens-3',
    name: 'Датчик · спальня',
    type: 'sensor',
    transport: 'zigbee',
    manufacturer: 'DIRIGERA',
    features: [{ key: 'temperature', states: ['celsius'], actions: [] }],
  });
  return { close: () => timers.forEach((t) => window.clearTimeout(t)) };
}

/**
 * `POST /devices/commission` (NDJSON стрим).
 * Мок эмулирует 4 стадии за ~3s и в конце возвращает device либо error.
 */
export interface CommissionHandle {
  close: () => void;
}

export function openCommission(
  candidate: DiscoveredDevice,
  onEvent: (ev: CommissioningEvent) => void,
  opts: { forceFail?: boolean } = {},
): CommissionHandle {
  if (USE_MOCK) return mockCommission(candidate, onEvent, opts);

  const ctrl = new AbortController();
  (async () => {
    try {
      const r = await fetch(`${API_BASE}/devices/commission`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          transport: candidate.transport,
          payload: candidate.ref,
          name: candidate.name,
          type: candidate.type,
        }),
        signal: ctrl.signal,
      });
      if (!r.ok || !r.body) throw new Error(`commission ${r.status}`);
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let nl: number;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).trim();
          buf = buf.slice(nl + 1);
          if (!line) continue;
          try {
            onEvent(JSON.parse(line) as CommissioningEvent);
          } catch {
            /* skip */
          }
        }
      }
    } catch (e) {
      if ((e as { name?: string }).name !== 'AbortError') {
        onEvent({ stage: 'error', message: (e as Error).message });
      }
    }
  })();
  return { close: () => ctrl.abort() };
}

/**
 * `POST /devices/commission` с setup-code от внешней экосистемы (multi-admin).
 * Мок эмулирует те же 4 стадии.
 */
export function openCommissionWithCode(
  setupCode: string,
  ecosystemHint: string,
  onEvent: (ev: CommissioningEvent) => void,
  opts: { forceFail?: boolean; target?: string; name?: string } = {},
): CommissionHandle {
  if (USE_MOCK) {
    return mockCommission(
      {
        ref: `setup:${setupCode}`,
        name: 'WARMBLIXT',
        type: 'light',
        transport: 'matter',
        manufacturer: 'IKEA',
        features: [
          { key: 'onoff', states: ['value'], actions: ['turn_on', 'turn_off', 'toggle'] },
          { key: 'brightness', states: ['level'], actions: ['set'] },
          { key: 'color_temp', states: ['kelvin'], actions: ['set'] },
        ],
      },
      onEvent,
      opts,
    );
  }

  const ctrl = new AbortController();
  (async () => {
    try {
      const r = await fetch(`${API_BASE}/devices/commission`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          transport: 'matter',
          payload: setupCode,
          name: opts.name ?? 'Новое устройство',
          ecosystem_hint: ecosystemHint,
          // Устройство, выбранное в списке найденных. Код всё равно обязателен:
          // passcode не анонсируется, без него PASE не стартует.
          ...(opts.target ? { extra: { 'matter.target': opts.target } } : {}),
        }),
        signal: ctrl.signal,
      });
      if (!r.ok || !r.body) throw new Error(`commission ${r.status}`);
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let nl: number;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).trim();
          buf = buf.slice(nl + 1);
          if (!line) continue;
          try {
            onEvent(JSON.parse(line) as CommissioningEvent);
          } catch {
            /* skip */
          }
        }
      }
    } catch (e) {
      if ((e as { name?: string }).name !== 'AbortError') {
        onEvent({ stage: 'error', message: (e as Error).message });
      }
    }
  })();
  return { close: () => ctrl.abort() };
}

function mockCommission(
  candidate: DiscoveredDevice,
  onEvent: (ev: CommissioningEvent) => void,
  opts: { forceFail?: boolean },
): CommissionHandle {
  const timers: number[] = [];
  const mult = mockMultiplier();
  const failNow = opts.forceFail || mockShouldFail();
  const step = (ms: number, ev: CommissioningEvent) =>
    timers.push(window.setTimeout(() => onEvent(ev), ms * mult));

  step(400, { stage: 'discovering', message: 'Устройство найдено' });
  step(1200, { stage: 'pairing', message: 'Обмен ключами · безопасное соединение' });
  step(2100, { stage: 'verifying', message: 'Проверяем связь · мигаем лампой' });
  if (failNow) {
    step(3200, { stage: 'error', message: candidate.type === 'light' ? 'лампа не отвечает' : 'устройство не отвечает' });
  } else {
    step(3200, {
      stage: 'done',
      message: 'Готово',
      device: {
        ID: `commissioned-${candidate.ref}`,
        Type: candidate.type ?? 'light',
        Name: candidate.name,
        Manufacturer: candidate.manufacturer,
        Transport: candidate.transport,
        TransportRef: candidate.ref,
        Features: (candidate.features ?? []).map((f) => ({
          Key: f.key,
          States: f.states,
          Actions: f.actions,
        })),
      },
    });
  }
  return { close: () => timers.forEach((t) => window.clearTimeout(t)) };
}
