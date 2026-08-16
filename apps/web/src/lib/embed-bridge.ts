// Parent-side of the Layer 4 postMessage bridge. Given a live
// iframe, this exchanges "hello" with the embed script inside it,
// proxies window.keystone.* calls back into the parent's real API,
// and forwards /stream events for subscribed topics.
//
// Origin is same-origin by construction — keystone serves both the
// parent app and the iframe out of one host — so postMessage's
// origin field is checked strictly and any cross-origin sender is
// dropped.

import { installKeystoneAPI } from './plugin-runtime';

interface Subscription {
  id: string;
  topic: string;
  unsubscribe: () => void;
}

export function attachEmbedBridge(iframe: HTMLIFrameElement): () => void {
  installKeystoneAPI();
  const api = window.keystone!;
  const origin = window.location.origin;
  const subs = new Map<string, Subscription>();
  let disposed = false;

  const handshake = () => {
    if (disposed) return;
    try {
      iframe.contentWindow?.postMessage({ ks: 1, type: 'hello' }, origin);
    } catch {
      // Iframe not ready yet; the load event fires the retry.
    }
  };

  const onLoad = () => handshake();
  iframe.addEventListener('load', onLoad);
  handshake();

  const onMessage = (event: MessageEvent) => {
    if (disposed) return;
    if (event.origin !== origin) return;
    if (event.source !== iframe.contentWindow) return;
    const msg = event.data as { ks?: number; id?: string; method?: string; params?: any };
    if (!msg || msg.ks !== 1 || !msg.id || !msg.method) return;

    const respond = (result?: unknown, error?: string) => {
      try {
        iframe.contentWindow?.postMessage({ ks: 1, type: 'response', id: msg.id, result, error }, origin);
      } catch { /* iframe closed */ }
    };

    (async () => {
      try {
        switch (msg.method) {
          case 'devices.list':
            respond(await api.devices.list());
            break;
          case 'devices.get':
            respond(await api.devices.get(String(msg.params?.id ?? '')));
            break;
          case 'devices.setState':
            respond(await api.devices.setState(
              String(msg.params?.id ?? ''),
              String(msg.params?.feature ?? ''),
              String(msg.params?.key ?? ''),
              msg.params?.value,
            ));
            break;
          case 'devices.invoke':
            respond(await api.devices.invoke(
              String(msg.params?.id ?? ''),
              String(msg.params?.feature ?? ''),
              String(msg.params?.action ?? ''),
              msg.params?.params,
            ));
            break;
          case 'subscribe': {
            const sid = String(msg.params?.id ?? '');
            const topic = String(msg.params?.topic ?? '');
            if (!sid || !topic) { respond(null, 'subscribe: id and topic required'); return; }
            // Cap the number of subscriptions per iframe so a hostile
            // plugin cannot spin the bridge into oblivion.
            if (subs.size >= 64) { respond(null, 'too many subscriptions'); return; }
            const unsub = api.subscribe(topic, (payload) => {
              if (disposed) return;
              try {
                iframe.contentWindow?.postMessage({ ks: 1, type: 'event', topic, payload }, origin);
              } catch { /* iframe closed */ }
            });
            subs.set(sid, { id: sid, topic, unsubscribe: unsub });
            respond({ ok: true });
            break;
          }
          case 'unsubscribe': {
            const sid = String(msg.params?.id ?? '');
            const sub = subs.get(sid);
            if (sub) { sub.unsubscribe(); subs.delete(sid); }
            respond({ ok: true });
            break;
          }
          default:
            respond(null, `unknown method: ${msg.method}`);
        }
      } catch (e) {
        respond(null, String(e));
      }
    })();
  };
  window.addEventListener('message', onMessage);

  return () => {
    disposed = true;
    iframe.removeEventListener('load', onLoad);
    window.removeEventListener('message', onMessage);
    for (const sub of subs.values()) sub.unsubscribe();
    subs.clear();
  };
}
