// Which socket this platform can use for the hub.
import { defaultSocketFactory, PinUnsupportedError, type SocketFactory } from '../protocol/client';
import { createNativeSocketFactory } from './nativeSocket';
import type { KeystoneNetModule } from './types';

/**
 * With a pin: the native pinned socket if the build has the module, otherwise refuse
 * (PinUnsupportedError). Without a pin: the platform's own WebSocket.
 */
export function chooseSocketFactory(native: KeystoneNetModule | null): SocketFactory {
  const pinned = native ? createNativeSocketFactory(native) : null;
  return (url, opts) => {
    if (opts.fingerprint) {
      if (!pinned) throw new PinUnsupportedError();
      return pinned(url, opts);
    }
    return defaultSocketFactory(url, opts);
  };
}
