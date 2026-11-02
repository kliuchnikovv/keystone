// The window in which the Zigbee network is open.
//
// The hub opens its network for a couple of minutes and the devices that join show up
// in the hub's list. The hub does not report the window itself (the request is
// fire-and-forget), so the countdown is ours, and the devices that joined are the ones
// that were not in the list when the window opened.
import { useCallback, useEffect, useRef, useState } from 'react';
import { PAIR_SECONDS } from '../model/devices';
import type { Entity } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import type { Commands } from './useCommands';

export interface JoinWindowState {
  /** The network is open and the clock is running. */
  open: boolean;
  /** Seconds left; 0 when the window is over. */
  left: number;
  /** The window is over. */
  expired: boolean;
  /** The devices that joined while the window was open. */
  joined: Entity[];
  /** When each of them joined, in milliseconds. */
  joinedAt: Map<number, number>;
  /** The hub did not open the network. */
  failed: boolean;
  /** The request is on its way. */
  opening: boolean;
  /** The current time, so the caller can say "just now" without a clock of its own. */
  now: number;
  /** Open the network again. */
  reopen(): void;
}

export function useJoinWindow(active: boolean, home: HomeState, commands: Commands): JoinWindowState {
  const request = commands.request;
  const [window, setWindow] = useState<{ until: number; known: Set<number> } | null>(null);
  const [opening, setOpening] = useState(false);
  const [failed, setFailed] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const joinedAt = useRef(new Map<number, number>());
  const alive = useRef(true);
  // The devices as they are when the window opens — not as they are now.
  const entities = useRef(home.entities);
  entities.current = home.entities;

  useEffect(() => () => { alive.current = false; }, []);

  useEffect(() => {
    if (!active) return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [active]);

  const open = useCallback(() => {
    const known = new Set(entities.current.map((e) => e.id));
    setOpening(true);
    setFailed(false);
    setWindow(null);
    void (async () => {
      const ok = await request('pair', { seconds: PAIR_SECONDS });
      if (!alive.current) return;
      setOpening(false);
      if (ok === null) {
        setFailed(true);
        return;
      }
      setNow(Date.now());
      setWindow({ until: Date.now() + PAIR_SECONDS * 1000, known });
    })();
  }, [request]);

  // Opening the screen opens the network: that is what the person came for.
  const started = useRef(false);
  useEffect(() => {
    if (!active) {
      started.current = false;
      return;
    }
    if (started.current) return;
    started.current = true;
    open();
  }, [active, open]);

  const joined = window ? home.entities.filter((e) => !window.known.has(e.id)) : [];
  for (const e of joined) if (!joinedAt.current.has(e.id)) joinedAt.current.set(e.id, Date.now());
  const left = window ? Math.max(0, Math.ceil((window.until - now) / 1000)) : PAIR_SECONDS;

  return {
    open: window !== null,
    left,
    expired: window !== null && left === 0,
    joined,
    joinedAt: joinedAt.current,
    failed,
    opening,
    now,
    reopen: open,
  };
}
