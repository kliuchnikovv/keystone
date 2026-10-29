// The state cache: what the hub said last time, kept on the phone.
//
// Two jobs: show the last known state right away (while we wait for the hub, and
// while it is out of reach), and remember what the hub says so it is there after
// the app is closed. The cache belongs to one hub: another address gets another
// home, not this one's devices.
import { useEffect, useRef } from 'react';
import { cachedState, decodeCache, encodeCache, sameHub } from '../protocol/cache';
import { readStateCache, writeStateCache } from '../statecache';
import type { HubConnection } from './useHub';

/** How long to wait before writing: the hub talks often, and every word is not worth a write. */
const SAVE_MS = 800;

export function useStateCache(client: HubConnection, url: string): void {
  // Show what we know from last time while the hub is still silent.
  useEffect(() => {
    let alive = true;
    void readStateCache().then((raw) => {
      if (!alive) return;
      const cached = decodeCache(raw);
      if (!cached || !sameHub(cached.url, url)) return;
      client.hydrate(cachedState(cached));
    });
    return () => {
      alive = false;
    };
  }, [client, url]);

  // Remember what the hub says, so it is there after a restart.
  const written = useRef('');
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | null = null;
    const save = () => {
      timer = null;
      const state = client.getSnapshot();
      if (state.entities.length === 0 && state.hub === null) return; // nothing worth keeping
      const text = encodeCache(state, url, Date.now());
      if (text === written.current) return; // the same thing we already have
      written.current = text;
      void writeStateCache(text);
    };
    const schedule = () => {
      if (timer) clearTimeout(timer);
      timer = setTimeout(save, SAVE_MS);
    };
    // What we know right now is worth keeping too (the hub may be quiet for a while).
    schedule();
    const unsubscribe = client.subscribe(schedule);
    return () => {
      if (timer) clearTimeout(timer);
      unsubscribe();
    };
  }, [client, url]);
}
