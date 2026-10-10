// Native key-value store: the OS keychain/keystore via expo-secure-store.
// (The browser build uses storage.web.ts.)
import * as SecureStore from 'expo-secure-store';
import type { KeyValueStore } from './settings';

export const defaultStore: KeyValueStore = {
  get: (k) => SecureStore.getItemAsync(k),
  set: (k, v) => SecureStore.setItemAsync(k, v),
  delete: (k) => SecureStore.deleteItemAsync(k),
};
