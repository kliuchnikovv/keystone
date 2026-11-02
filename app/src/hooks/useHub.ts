import { useEffect, useMemo, useSyncExternalStore } from 'react';
import { keystoneNet } from '../native/module';
import { chooseSocketFactory } from '../native/platform';
import { HubClient, type HubClientOptions } from '../protocol/client';
import { CloudClient } from '../protocol/cloud-client';
import type { HomeState } from '../protocol/store';
import { isCloudUrl, pinnedFingerprintFor, type Settings } from '../settings';
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
  /** Show what the app already knows (the cached state) until the hub says otherwise. */
  hydrate(state: HomeState): void;
  /** The connection is not ours to close any more: it was handed to someone else. */
  handOver(): void;
  /** True after `handOver()`. */
  readonly isHandedOver: boolean;
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
      // A relay presents its own certificate, not the hub's: pinning the hub's
      // fingerprint there would refuse the connection (docs/relay-security.md).
      const pin = pinnedFingerprintFor(settings.url, settings.fingerprint);
      return new HubClient({
        url: settings.url,
        token: settings.token,
        clientName: 'keystone-app',
        socketFactory: chooseSocketFactory(keystoneNet()),
        ...(pin ? { pinnedFingerprint: pin } : {}),
        ...options,
      });
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [settings.url, settings.token, settings.fingerprint],
  );
  // The last known state is kept by the screen that shows the home (see
  // useStateCache): the connection may outlive the component that made it.
  useEffect(() => {
    if (!autoConnect) return;
    client.connect();
    return () => {
      // A handed-over connection lives on in someone else's hands.
      if (!client.isHandedOver) client.close();
    };
  }, [client, autoConnect]);
  return client;
}

/** The client's state, re-rendering the component when it changes. */
export function useHome(client: HubConnection): HomeState {
  return useSyncExternalStore(client.subscribe, client.getSnapshot, client.getSnapshot);
}
