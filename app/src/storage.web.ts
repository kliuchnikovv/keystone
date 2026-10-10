// Browser key-value store. localStorage is readable by any script on this origin, so
// on the web the token is only as safe as the page; prefer the native app for daily use.
import type { KeyValueStore } from './settings';

export const defaultStore: KeyValueStore = {
  async get(k) {
    try {
      return globalThis.localStorage?.getItem(k) ?? null;
    } catch {
      return null;
    }
  },
  async set(k, v) {
    globalThis.localStorage?.setItem(k, v);
  },
  async delete(k) {
    globalThis.localStorage?.removeItem(k);
  },
};
