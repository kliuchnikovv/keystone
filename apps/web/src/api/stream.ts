import { API_BASE } from './client';
import type { StreamMessage } from './types';

export interface StreamHandle {
  close: () => void;
}

/**
 * Подписка на /stream (NDJSON поверх long-lived HTTP).
 * Reconnect с экспоненциальным backoff [1s, 3s, 6s, 12s, 30s].
 */
export function openStream(
  onMessage: (msg: StreamMessage) => void,
  onStatus: (s: 'connecting' | 'live' | 'reconnecting' | 'error') => void,
): StreamHandle {
  const backoff = [1000, 3000, 6000, 12000, 30000];
  let attempt = 0;
  let stopped = false;
  let ctrl: AbortController | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  async function connect() {
    if (stopped) return;
    onStatus(attempt === 0 ? 'connecting' : 'reconnecting');
    ctrl = new AbortController();
    try {
      const res = await fetch(`${API_BASE}/stream`, { signal: ctrl.signal });
      if (!res.ok || !res.body) throw new Error(`stream ${res.status}`);
      onStatus('live');
      attempt = 0;
      const reader = res.body.getReader();
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
            onMessage(JSON.parse(line) as StreamMessage);
          } catch {
            /* skip malformed line */
          }
        }
      }
      throw new Error('stream closed');
    } catch (e) {
      if (stopped) return;
      if ((e as { name?: string }).name === 'AbortError') return;
      onStatus('error');
      const delay = backoff[Math.min(attempt, backoff.length - 1)]!;
      attempt += 1;
      reconnectTimer = setTimeout(connect, delay);
    }
  }

  void connect();

  return {
    close() {
      stopped = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      ctrl?.abort();
    },
  };
}
