// When did each device last report something?
// The hub does not put timestamps on the wire, so the app remembers: a value that
// changed was reported just now. The button's `event` pulse is a press.
import { useEffect, useRef, useState } from 'react';
import type { Entity, Value } from '../protocol/messages';

/** The attribute id of a button press (`event`). */
const EVENT_ATTR = 0x60;

export interface Activity {
  /** When the device's values last changed (client clock, ms). */
  updatedAt: Map<number, number>;
  /** When the button was last pressed (client clock, ms). */
  pressedAt: Map<number, number>;
}

function sameValue(a: Value | undefined, b: Value | undefined): boolean {
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => x === b[i]);
  return a === b;
}

function valuesOf(e: Entity): Map<number, Value> {
  const m = new Map<number, Value>();
  for (const a of e.attrs) m.set(a.id, a.value);
  return m;
}

export function useEntityActivity(entities: Entity[]): Activity {
  const prev = useRef(new Map<number, Map<number, Value>>());
  const updatedAt = useRef(new Map<number, number>());
  const pressedAt = useRef(new Map<number, number>());
  const [, bump] = useState(0);

  useEffect(() => {
    const now = Date.now();
    let changed = false;
    for (const e of entities) {
      const before = prev.current.get(e.id);
      const current = valuesOf(e);
      if (before === undefined) {
        // First sight: the app has just learned what the device is — that is the
        // moment the card counts from.
        prev.current.set(e.id, current);
        updatedAt.current.set(e.id, now);
        changed = true;
        continue;
      }
      for (const [id, v] of current) {
        if (!sameValue(before.get(id), v)) {
          changed = true;
          updatedAt.current.set(e.id, now);
          if (id === EVENT_ATTR) pressedAt.current.set(e.id, now);
          break;
        }
      }
      prev.current.set(e.id, current);
    }
    if (changed) bump((n) => n + 1);
  }, [entities]);

  return { updatedAt: updatedAt.current, pressedAt: pressedAt.current };
}
