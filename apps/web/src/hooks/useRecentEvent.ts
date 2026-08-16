import { useEffect, useState } from 'react';
import { useEventsStore } from '../state/eventsStore';
import { describeEvent, describeUnconfirmed, type EventLabel } from '../lib/device-events';

/** Сколько событие остаётся подсвеченным. */
export const EVENT_HIGHLIGHT_MS = 4000;

export interface RecentEventView extends EventLabel {
  name: string;
  at: number;
}

/**
 * Возвращает последнее событие устройства, пока оно «свежее».
 *
 * Подписка на стор сама по себе не разбудит компонент, когда событие устареет —
 * стор в этот момент не меняется. Поэтому здесь свой таймер: без него подсветка
 * висела бы до следующего рендера по любой другой причине.
 */
export function useRecentEvent(deviceId: string): RecentEventView | undefined {
  const recent = useEventsStore((s) => s.recent[deviceId]);
  const [, force] = useState(0);

  useEffect(() => {
    if (!recent) return;
    const left = recent.at + EVENT_HIGHLIGHT_MS - Date.now();
    if (left <= 0) return;
    const timer = window.setTimeout(() => force((n) => n + 1), left);
    return () => window.clearTimeout(timer);
  }, [recent]);

  if (!recent) return undefined;
  if (Date.now() - recent.at > EVENT_HIGHLIGHT_MS) return undefined;
  return { ...describeEvent(recent.name, recent.data), name: recent.name, at: recent.at };
}

/** Последнее событие без ограничения по свежести — для экрана устройства. */
export function useLastEvent(deviceId: string): RecentEventView | undefined {
  const recent = useEventsStore((s) => s.recent[deviceId]);
  if (!recent) return undefined;
  return { ...describeEvent(recent.name, recent.data), name: recent.name, at: recent.at };
}

/** Сколько держится сообщение о неподтверждённой команде. */
const UNCONFIRMED_MS = 12000;

/**
 * useUnconfirmed возвращает текст о команде, которую устройство не подтвердило.
 *
 * Живёт дольше обычной подсветки: это не индикация, а сообщение об ошибке —
 * его должны успеть прочитать. Само по себе гаснет, потому что следующая
 * удачная команда его не «перебьёт»: событий об успехе не бывает.
 */
export function useUnconfirmed(deviceId: string): string | undefined {
  const recent = useEventsStore((s) => s.recent[deviceId]);
  const [, force] = useState(0);
  const fresh = recent?.name === 'not_confirmed' && Date.now() - recent.at <= UNCONFIRMED_MS;

  useEffect(() => {
    if (!recent || recent.name !== 'not_confirmed') return;
    const left = recent.at + UNCONFIRMED_MS - Date.now();
    if (left <= 0) return;
    const timer = window.setTimeout(() => force((n) => n + 1), left);
    return () => window.clearTimeout(timer);
  }, [recent]);

  if (!fresh || !recent) return undefined;
  return describeUnconfirmed(recent.data);
}
