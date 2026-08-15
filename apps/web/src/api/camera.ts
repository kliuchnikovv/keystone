import { API_BASE } from './client';

/**
 * Живое видео с Matter-камеры.
 *
 * Через keystone идёт только сигнализация: SDP и ICE-кандидаты передаются
 * командами Matter-кластеров. Сами кадры текут напрямую с камеры в этот
 * `RTCPeerConnection` — ни keystone, ни sidecar видео не видят и не
 * перекодируют. Камера и браузер в одной сети, что для ICE самый простой
 * случай.
 */
export interface CameraSession {
  /** Останавливает сессию и освобождает peer connection. */
  close: () => void;
}

export interface CameraHandlers {
  onStream: (stream: MediaStream) => void;
  onState?: (state: RTCPeerConnectionState) => void;
  onError?: (e: Error) => void;
  onEnded?: (reason?: string) => void;
}

interface SignalFrame {
  kind: 'session' | 'answer' | 'offer' | 'ice' | 'end';
  session_id: number;
  sdp?: string;
  candidates?: string[];
  reason?: string;
}

export function openCameraStream(deviceId: string, handlers: CameraHandlers): CameraSession {
  const ctrl = new AbortController();
  const pc = new RTCPeerConnection({
    // Локальная сеть: STUN не нужен, кандидаты хоста находят друг друга сами.
    // Внешний STUN добавил бы задержку старта и утечку адреса наружу.
    iceServers: [],
  });

  let sessionId: number | undefined;
  // Кандидаты, найденные до того, как камера сообщила id сессии, — отправить
  // их некуда, поэтому копим и досылаем.
  const pending: string[] = [];
  let closed = false;

  const post = async (path: string, body: unknown) => {
    await fetch(`${API_BASE}/devices/${deviceId}/camera/${path}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
      signal: ctrl.signal,
    });
  };

  const flushCandidates = () => {
    if (sessionId === undefined || pending.length === 0) return;
    const batch = pending.splice(0, pending.length);
    void post('ice', { session_id: sessionId, candidates: batch }).catch(() => {
      /* сигнализация best-effort: часть кандидатов может не дойти */
    });
  };

  pc.ontrack = (ev) => {
    if (ev.streams[0]) handlers.onStream(ev.streams[0]);
  };
  pc.onicecandidate = (ev) => {
    if (!ev.candidate) return;
    pending.push(ev.candidate.candidate);
    flushCandidates();
  };
  pc.onconnectionstatechange = () => handlers.onState?.(pc.connectionState);

  const close = () => {
    if (closed) return;
    closed = true;
    if (sessionId !== undefined) {
      // Без AbortController: запрос должен уйти даже когда стрим уже закрыт,
      // иначе камера продолжит кодировать в никуда.
      void fetch(`${API_BASE}/devices/${deviceId}/camera/stop`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ session_id: sessionId }),
        keepalive: true,
      }).catch(() => {});
    }
    ctrl.abort();
    pc.close();
  };

  (async () => {
    try {
      // Смотрим, не отправляем: камера — единственный источник медиа.
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.addTransceiver('audio', { direction: 'recvonly' });

      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);

      const r = await fetch(`${API_BASE}/devices/${deviceId}/camera/session`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ sdp: offer.sdp }),
        signal: ctrl.signal,
      });
      if (!r.ok || !r.body) {
        throw new Error(`camera session ${r.status}`);
      }

      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (!closed) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let nl: number;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).trim();
          buf = buf.slice(nl + 1);
          if (!line) continue;
          let frame: SignalFrame;
          try {
            frame = JSON.parse(line) as SignalFrame;
          } catch {
            continue;
          }
          await handleSignal(frame);
        }
      }
    } catch (e) {
      if ((e as { name?: string }).name !== 'AbortError') {
        handlers.onError?.(e as Error);
      }
    }
  })();

  async function handleSignal(frame: SignalFrame) {
    switch (frame.kind) {
      case 'session':
        sessionId = frame.session_id;
        flushCandidates();
        break;
      case 'answer':
        if (frame.sdp) {
          await pc.setRemoteDescription({ type: 'answer', sdp: frame.sdp });
        }
        break;
      case 'ice':
        for (const candidate of frame.candidates ?? []) {
          try {
            await pc.addIceCandidate({ candidate, sdpMLineIndex: 0 });
          } catch {
            /* кандидат может относиться к уже закрытой сессии */
          }
        }
        break;
      case 'end':
        handlers.onEnded?.(frame.reason);
        close();
        break;
    }
  }

  return { close };
}
