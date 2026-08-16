// Layer 3 runtime: a small window.keystone bootstrap plus a loader
// that resolves plugin-declared custom elements on demand.
//
// The API is intentionally narrow — devices.get/list/setState/
// invoke, plus subscribe over the same NDJSON /stream the app
// already consumes. When a plugin's Web Component asks for
// window.keystone, it gets exactly this surface; anything else is
// opt-in via a future permissions block.

import { apiFetch, API_BASE } from '../api/client';
import { listPlugins, type PluginStatus } from '../api/plugins';

// -------- window.keystone typings --------

export interface KeystoneAPI {
  devices: DevicesAPI;
  subscribe: (topic: string, cb: (event: unknown) => void) => () => void;
}

interface DevicesAPI {
  list: () => Promise<unknown>;
  get: (id: string) => Promise<unknown>;
  setState: (id: string, feature: string, key: string, value: unknown) => Promise<unknown>;
  invoke: (id: string, feature: string, action: string, params?: Record<string, unknown>) => Promise<unknown>;
}

declare global {
  interface Window {
    keystone?: KeystoneAPI;
  }
}

// -------- window.keystone bootstrap --------

const streamSubscribers = new Set<{ topic: string; cb: (e: unknown) => void }>();
let streamStarted = false;

function ensureStream() {
  if (streamStarted) return;
  streamStarted = true;
  // The app already opens a WebSocket via useLiveStream — this
  // subscribe hook is a lightweight fan-out from a fresh EventSource
  // so plugin components stay decoupled from the app's Zustand
  // store. Topic matching is prefix-only for MVP.
  try {
    const es = new EventSource(`${API_BASE}/stream`);
    es.onmessage = (evt) => {
      let payload: unknown;
      try { payload = JSON.parse(evt.data); } catch { return; }
      for (const sub of streamSubscribers) {
        const t = typeof payload === 'object' && payload && 'topic' in (payload as any)
          ? String((payload as any).topic ?? '')
          : '';
        if (matches(sub.topic, t)) sub.cb(payload);
      }
    };
  } catch {
    // No SSE — subscribe becomes a no-op. Web Components that only
    // read state via devices.get still work.
  }
}

// matches supports single-segment "*" and multi-segment "**"
// wildcards at any position — the same shape sidecar-protocol
// topics use.
function matches(pattern: string, topic: string): boolean {
  const p = pattern.split('.');
  const t = topic.split('.');
  let i = 0;
  let j = 0;
  while (i < p.length && j < t.length) {
    if (p[i] === '**') return true;
    if (p[i] !== '*' && p[i] !== t[j]) return false;
    i++; j++;
  }
  return i === p.length && j === t.length;
}

export function installKeystoneAPI() {
  if (window.keystone) return;
  window.keystone = {
    devices: {
      list: () => apiFetch('/devices'),
      get: (id) => apiFetch(`/devices/${encodeURIComponent(id)}`),
      setState: (id, feature, key, value) =>
        apiFetch(`/devices/${encodeURIComponent(id)}/state`, {
          method: 'POST',
          body: JSON.stringify({ feature, key, value }),
        }),
      invoke: (id, feature, action, params) =>
        apiFetch(`/devices/${encodeURIComponent(id)}/actions`, {
          method: 'POST',
          body: JSON.stringify({ feature, action, params: params ?? {} }),
        }),
    },
    subscribe: (topic, cb) => {
      ensureStream();
      const sub = { topic, cb };
      streamSubscribers.add(sub);
      return () => { streamSubscribers.delete(sub); };
    },
  };
}

// -------- element loader --------

// A single plugin's UI declaration. The manifest ships a bare
// map[string]any; we validate at read time so a malformed entry
// simply gets skipped rather than crashing the app.
export interface DeviceDetailBinding {
  matchDeviceType?: string;
  matchFeature?: string;
  element: string;
  source: string;
}

// modulesLoaded tracks which /plugins/{name}/ui/<src> URLs have
// already been imported so a device detail switching between two
// matter thermostats does not re-import the same JS. import() has
// its own cache but we cache the promise to serialize concurrent
// callers.
const modulesLoaded = new Map<string, Promise<unknown>>();

async function loadModule(url: string) {
  const cached = modulesLoaded.get(url);
  if (cached) return cached;
  const p = import(/* @vite-ignore */ url);
  modulesLoaded.set(url, p);
  return p;
}

// pickDetailBinding walks every plugin's ui.deviceDetail list and
// returns the first entry whose match criterion applies. Order is
// stable per plugin list.
export function pickDetailBinding(
  plugins: PluginStatus[],
  deviceType: string,
  features: string[],
): { plugin: string; binding: DeviceDetailBinding } | null {
  for (const p of plugins) {
    if (p.state !== 'running') continue;
    const raw = (p.manifest as any)?.UI?.deviceDetail ?? (p.manifest as any)?.ui?.deviceDetail;
    if (!Array.isArray(raw)) continue;
    for (const entry of raw as DeviceDetailBinding[]) {
      if (!entry.element || !entry.source) continue;
      if (entry.matchDeviceType && entry.matchDeviceType !== deviceType) continue;
      if (entry.matchFeature && !features.includes(entry.matchFeature)) continue;
      return { plugin: p.name, binding: entry };
    }
  }
  return null;
}

// mountPluginElement resolves a binding: loads its module (which is
// expected to call customElements.define on the declared element),
// then creates the element and passes device-id + a few useful
// attributes so the component can start rendering immediately.
export async function mountPluginElement(
  host: HTMLElement,
  plugin: string,
  binding: DeviceDetailBinding,
  deviceID: string,
): Promise<HTMLElement | null> {
  // Custom elements per WHATWG must be lowercase, start with a
  // letter, and contain a hyphen — that excludes built-in tags
  // (script, iframe, img) so a malicious manifest cannot smuggle
  // one in under the "element" field.
  if (!/^[a-z][a-z0-9]*-[a-z0-9-]+$/.test(binding.element)) {
    console.error('plugin ui: invalid custom element name', binding.element);
    return null;
  }
  installKeystoneAPI();
  const src = `${API_BASE}/plugins/${encodeURIComponent(plugin)}/ui/${binding.source}`;
  try {
    await loadModule(src);
  } catch (e) {
    console.error('plugin ui load failed', src, e);
    return null;
  }
  if (!customElements.get(binding.element)) {
    console.error('plugin did not register custom element', binding.element);
    return null;
  }
  const el = document.createElement(binding.element) as HTMLElement;
  el.setAttribute('device-id', deviceID);
  // replaceChildren is the safe idiom for "clear the host, then mount
  // this element" — no HTML parsing, no serialization round-trip,
  // so no room for XSS even if a caller mistakenly passes markup
  // through binding.element in the future.
  host.replaceChildren(el);
  return el;
}

// -------- helper used by DeviceDetailScreen --------

/**
 * usePluginDetail resolves a plugin-declared custom element for the
 * given device type + feature list and mounts it into a host element.
 * Returns whether a plugin element rendered so the caller can hide
 * the built-in detail markup when it did.
 */
export async function usePluginDetail(host: HTMLElement, deviceID: string, deviceType: string, features: string[]) {
  const plugins = await listPlugins();
  const match = pickDetailBinding(plugins, deviceType, features);
  if (!match) return false;
  const mounted = await mountPluginElement(host, match.plugin, match.binding, deviceID);
  return !!mounted;
}
