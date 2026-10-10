import { useEffect, useRef, useState } from 'react';
import type { Timer } from '../protocol/messages';

/**
 * Timers arrive with `left_s` as of the snapshot. This returns a function giving the
 * seconds left *now*, re-rendering once a second while any timer is pending.
 */
export function useCountdown(timers: Timer[]): (t: Timer) => number {
  const receivedAt = useRef(Date.now());
  const [, tick] = useState(0);

  useEffect(() => {
    receivedAt.current = Date.now();
    tick((n) => n + 1);
  }, [timers]);

  useEffect(() => {
    if (timers.length === 0) return;
    const id = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(id);
  }, [timers.length]);

  return (t) => Math.max(0, t.left_s - Math.floor((Date.now() - receivedAt.current) / 1000));
}
