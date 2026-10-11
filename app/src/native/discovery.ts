// Hubs found by the native mDNS browser, as a small observable list.
import type { KeystoneNetModule, NativeHub } from './types';

/** Starts browsing; `onChange` gets the whole list (stable order) on every change. Returns stop(). */
export function startDiscovery(mod: KeystoneNetModule, onChange: (hubs: NativeHub[]) => void): () => void {
  const hubs = new Map<string, NativeHub>();
  const emit = () => onChange([...hubs.values()].sort((a, b) => a.instance.localeCompare(b.instance)));
  const found = mod.addListener('onHubFound', (h) => {
    hubs.set(h.instance, h);
    emit();
  });
  const lost = mod.addListener('onHubLost', (h) => {
    if (hubs.delete(h.instance)) emit();
  });
  mod.startDiscovery();
  return () => {
    found.remove();
    lost.remove();
    mod.stopDiscovery();
  };
}

/** The URL to connect to for a discovered hub (the address, as not every OS resolves `.local`). */
export function hubUrl(h: NativeHub): string {
  return `${h.tls ? 'wss' : 'ws'}://${h.address ?? h.host}:${h.port}/ws`;
}
