// keystone-embed.js — iframe-side of the Layer 4 postMessage bridge.
//
// Plugins that ship their own UI framework include this script and
// then use window.keystone the same way a Layer 3 Web Component would.
// The bridge waits for a "hello" from the parent frame (which knows
// exactly which plugin it launched), then installs a window.keystone
// proxy whose calls round-trip via postMessage.
//
// One tiny surface, zero dependencies, so a plugin can ship whatever
// UI stack it likes without pulling this bootstrap into its bundle.

(function () {
  if (window.keystone && window.keystone.__installed) return;

  let parentOrigin = null;
  let ready;
  const resolveReady = new Promise((r) => { ready = r; });
  const pending = new Map();
  let seq = 0;

  function call(method, params) {
    if (!parentOrigin) {
      return Promise.reject(new Error('keystone-embed: parent did not say hello yet'));
    }
    const id = 'k' + (++seq);
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject });
      window.parent.postMessage({ ks: 1, id, method, params }, parentOrigin);
    });
  }

  const subscribers = new Map();

  window.addEventListener('message', (event) => {
    const msg = event.data;
    if (!msg || msg.ks !== 1) return;
    // Accept hello only from a same-origin parent — a page nested
    // elsewhere cannot pretend to be keystone by posting a matching
    // shape. The parent posted its origin so we can pin it.
    if (msg.type === 'hello') {
      if (event.origin !== window.location.origin) return;
      parentOrigin = event.origin;
      resolveReady();
      return;
    }
    if (parentOrigin && event.origin !== parentOrigin) return;
    if (msg.type === 'response' && msg.id) {
      const p = pending.get(msg.id);
      if (!p) return;
      pending.delete(msg.id);
      if (msg.error) p.reject(new Error(msg.error));
      else p.resolve(msg.result);
      return;
    }
    if (msg.type === 'event' && msg.topic) {
      for (const [sid, sub] of subscribers) {
        if (sub.topic === msg.topic || matches(sub.topic, msg.topic)) {
          try { sub.cb(msg.payload); } catch (_) { /* subscriber crashed */ }
        }
        void sid;
      }
    }
  });

  function matches(pattern, topic) {
    const p = String(pattern).split('.');
    const t = String(topic).split('.');
    let i = 0, j = 0;
    while (i < p.length && j < t.length) {
      if (p[i] === '**') return true;
      if (p[i] !== '*' && p[i] !== t[j]) return false;
      i++; j++;
    }
    return i === p.length && j === t.length;
  }

  const api = {
    __installed: true,
    ready: resolveReady,
    devices: {
      list: () => call('devices.list'),
      get: (id) => call('devices.get', { id }),
      setState: (id, feature, key, value) =>
        call('devices.setState', { id, feature, key, value }),
      invoke: (id, feature, action, params) =>
        call('devices.invoke', { id, feature, action, params: params || {} }),
    },
    subscribe: (topic, cb) => {
      const sid = 's' + (++seq);
      subscribers.set(sid, { topic, cb });
      call('subscribe', { topic, id: sid }).catch(() => { /* parent decides */ });
      return () => {
        subscribers.delete(sid);
        call('unsubscribe', { id: sid }).catch(() => {});
      };
    },
  };
  window.keystone = api;
})();
