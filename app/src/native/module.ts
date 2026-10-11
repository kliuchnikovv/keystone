// The native module, when this build has it (a dev/production build with modules/keystone-net;
// not Expo Go, not the web).
import { requireOptionalNativeModule } from 'expo-modules-core';
import type { KeystoneNetModule } from './types';

let cached: KeystoneNetModule | null | undefined;

export function keystoneNet(): KeystoneNetModule | null {
  if (cached === undefined) {
    try {
      cached = requireOptionalNativeModule<KeystoneNetModule>('KeystoneNet');
    } catch {
      cached = null;
    }
  }
  return cached;
}
