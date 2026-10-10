import { useCallback, useEffect, useRef, useState } from 'react';
import type { HubClient } from '../protocol/client';
import type { Value } from '../protocol/messages';
import { describeAck, describeError } from '../model/format';

export interface Commands {
  /** Send a command; resolves when the chip answered. Failures show up in `message`. */
  send(entity: number, attr: number, value: Value): Promise<void>;
  isPending(entity: number, attr: number): boolean;
  /** A management request (`rule.add`, `name.set`, …). Resolves with the result, or null after showing why it failed. */
  request(op: string, args?: Record<string, unknown>): Promise<Record<string, unknown> | null>;
  /** True while any management request is in flight. */
  busy: boolean;
  /** The latest failure, shown briefly. */
  message: string | null;
  dismiss(): void;
}

const MESSAGE_MS = 4500;

export function useCommands(client: HubClient): Commands {
  const [pending, setPending] = useState<ReadonlySet<string>>(new Set());
  const [message, setMessage] = useState<string | null>(null);
  const [inFlight, setInFlight] = useState(0);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      if (timer.current) clearTimeout(timer.current);
    };
  }, []);

  const show = useCallback((text: string) => {
    if (!alive.current) return;
    setMessage(text);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => alive.current && setMessage(null), MESSAGE_MS);
  }, []);

  const send = useCallback(
    async (entity: number, attr: number, value: Value) => {
      const key = `${entity}:${attr}`;
      setPending((p) => new Set(p).add(key));
      try {
        const bad = describeAck(await client.cmd(entity, attr, value));
        if (bad) show(bad);
      } catch (e) {
        show(describeError(e));
      } finally {
        if (alive.current) {
          setPending((p) => {
            const n = new Set(p);
            n.delete(key);
            return n;
          });
        }
      }
    },
    [client, show],
  );

  const request = useCallback(
    async (op: string, args?: Record<string, unknown>) => {
      setInFlight((n) => n + 1);
      try {
        return await client.req(op, args);
      } catch (e) {
        show(describeError(e));
        return null;
      } finally {
        if (alive.current) setInFlight((n) => n - 1);
      }
    },
    [client, show],
  );

  return {
    send,
    request,
    busy: inFlight > 0,
    isPending: (entity, attr) => pending.has(`${entity}:${attr}`),
    message,
    dismiss: () => setMessage(null),
  };
}
