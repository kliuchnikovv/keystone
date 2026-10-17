import { useCallback, useEffect, useRef, useState } from 'react';
import { describeError } from '../model/format';
import { emptyHistory, mergeNewest, mergeOlder, mergePush, parseHistoryPage, type HistoryList } from '../model/history';
import type { ConnectionStatus } from '../protocol/store';
import type { HubConnection } from './useHub';

export interface History extends HistoryList {
  /** The first page has arrived (an empty history is then really empty). */
  loaded: boolean;
  /** An older page is being fetched. */
  loadingOlder: boolean;
  error: string | null;
  loadOlder(): Promise<void>;
  refresh(): Promise<void>;
}

const POLL_MS = 4000;
/** Once the hub has pushed an event the poll is only a safety net. */
const SAFETY_POLL_MS = 30_000;

/**
 * The hub's event history, newest first, optionally about one device. Fetches the newest page when the
 * connection (re)opens, takes new events as the hub pushes them (docs/app-protocol.md §5.1) and polls every
 * few seconds until it has seen a push (an older hub never pushes), then only as a safety net. Older pages
 * come on demand. The hub restarts its sequence numbers when it reboots, so the list is dropped on every
 * reconnect.
 */
export function useHistory(client: HubConnection, status: ConnectionStatus, entity: number | null, pollMs: number = POLL_MS): History {
  const [list, setList] = useState<HistoryList>(emptyHistory);
  const [loaded, setLoaded] = useState(false);
  const [loadingOlder, setLoadingOlder] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const listRef = useRef(list);
  listRef.current = list;
  const pushed = useRef(false); // this connection has delivered a push: the hub does it
  const fetchedAt = useRef(0);
  const epoch = useRef(0); // bumped when what we are listing changes: a late answer for the old list is dropped
  const ready = status === 'ready';

  const args = useCallback((before?: number) => ({ ...(entity !== null ? { entity } : {}), ...(before ? { before } : {}) }), [entity]);

  const refresh = useCallback(async () => {
    if (!ready) return;
    const mine = epoch.current;
    fetchedAt.current = Date.now();
    try {
      const page = parseHistoryPage(await client.req('history', args()));
      if (mine !== epoch.current) return;
      if (page === null) return setError('The hub sent a history this app does not understand');
      setList((l) => mergeNewest(l, page, Date.now()));
      setLoaded(true);
      setError(null);
    } catch (e) {
      if (mine === epoch.current) setError(describeError(e));
    }
  }, [client, ready, args]);

  const loadOlder = useCallback(async () => {
    const last = listRef.current.events[listRef.current.events.length - 1];
    if (!ready || !last) return;
    const mine = epoch.current;
    setLoadingOlder(true);
    try {
      const page = parseHistoryPage(await client.req('history', args(last.seq)));
      if (mine !== epoch.current) return;
      if (page !== null) setList((l) => mergeOlder(l, page, Date.now()));
    } catch (e) {
      if (mine === epoch.current) setError(describeError(e));
    } finally {
      if (mine === epoch.current) setLoadingOlder(false);
    }
  }, [client, ready, args]);

  // A different device, or a new connection: start over.
  useEffect(() => {
    epoch.current += 1;
    pushed.current = false;
    setList(emptyHistory);
    setLoaded(false);
    setLoadingOlder(false);
    setError(null);
  }, [entity, ready]);

  useEffect(() => {
    if (!ready) return;
    void refresh();
    if (pollMs <= 0) return;
    const t = setInterval(() => {
      if (pushed.current && Date.now() - fetchedAt.current < SAFETY_POLL_MS) return;
      void refresh();
    }, pollMs);
    return () => clearInterval(t);
  }, [ready, refresh, pollMs]);

  // Live events. A device filter is the hub's (which kinds of event mention a device), so a filtered list asks
  // again instead of guessing; an unfiltered one folds the push in, and asks again if it does not join.
  useEffect(() => {
    if (!ready) return;
    return client.subscribeHistory((push) => {
      pushed.current = true;
      if (entity !== null) return void refresh();
      const page = parseHistoryPage({ now: push.now, latest: push.latest, oldest: 0, events: push.events });
      if (page === null) return void refresh();
      const merged = mergePush(listRef.current, page, Date.now());
      if (merged === null) return void refresh();
      listRef.current = merged; // a second push in the same tick joins this one, not the list it replaced
      setList(merged);
    });
  }, [client, ready, entity, refresh]);

  return { ...list, loaded, loadingOlder, error, loadOlder, refresh };
}
