import { useEffect, useMemo, useSyncExternalStore } from 'react';
import { keystoneNet } from '../native/module';
import { chooseSocketFactory } from '../native/platform';
import { HubClient, type HubClientOptions } from '../protocol/client';
import { CloudClient } from '../protocol/cloud-client';
import type { HomeState } from '../protocol/store';
import { isCloudUrl, type Settings } from '../settings';
import type { AckStatus, HistoryPush, Value } from '../protocol/messages';

/** The public surface both HubClient and CloudClient expose. */
export interface HubConnection {
  subscribe(fn: () => void): () => void;
  getSnapshot(): HomeState;
  subscribeHistory(fn: (push: HistoryPush) => void): () => void;
  cmd(entity: number, attr: number, value: Value): Promise<AckStatus>;
  req(op: string, args?: Record<string, unknown>): Promise<Record<string, unknown>>;
  connect(): void;
  close(): void;
}

/** One connection per (url, token); it lives as long as the component that asked.
 * `autoConnect` is false when the caller already has a live connection (the Connect
 * screen hands its client over) — then this one must not open a second socket. */
export function useHubClient(settings: Settings, options: Partial<HubClientOptions> = {}, autoConnect = true): HubConnection {
  const client = useMemo(
    () => {
      if (isCloudUrl(settings.url)) {
        return new CloudClient({ url: settings.url, token: settings.token });
      }
      return new HubClient({
        url: settings.url,
        token: settings.token,
        clientName: 'keystone-app',
        socketFactory: chooseSocketFactory(keystoneNet()),
        ...(settings.fingerprint ? { pinnedFingerprint: settings.fingerprint } : {}),
        ...options,
      });
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [settings.url, settings.token, settings.fingerprint],
  );
  useEffect(() => {
    if (!autoConnect) return;
    client.connect();
    return () => client.close();
  }, [client, autoConnect]);
  return client;
}

/** The client's state, re-rendering the component when it changes. */
export function useHome(client: HubConnection): HomeState {
  return useSyncExternalStore(client.subscribe, client.getSnapshot, client.getSnapshot);
}
