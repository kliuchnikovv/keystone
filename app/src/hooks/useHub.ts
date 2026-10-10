import { useEffect, useMemo, useSyncExternalStore } from 'react';
import { HubClient, type HubClientOptions } from '../protocol/client';
import type { HomeState } from '../protocol/store';
import type { Settings } from '../settings';

/** One connection per (url, token); it lives as long as the component that asked. */
export function useHubClient(settings: Settings, options: Partial<HubClientOptions> = {}): HubClient {
  const client = useMemo(
    () =>
      new HubClient({
        url: settings.url,
        token: settings.token,
        clientName: 'keystone-app',
        ...(settings.fingerprint ? { pinnedFingerprint: settings.fingerprint } : {}),
        ...options,
      }),
    // The options object is built fresh on every render; only a new hub, token or pin matters.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [settings.url, settings.token, settings.fingerprint],
  );
  useEffect(() => {
    client.connect();
    return () => client.close();
  }, [client]);
  return client;
}

/** The client's state, re-rendering the component when it changes. */
export function useHome(client: HubClient): HomeState {
  return useSyncExternalStore(client.subscribe, client.getSnapshot, client.getSnapshot);
}
