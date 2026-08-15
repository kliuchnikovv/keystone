import { create } from 'zustand';

export interface EventLogEntry {
  id: string;
  at: number;
  kind: 'state' | 'event';
  deviceId: string;
  feature: string;
  /** Для state: `${key} · ${prev} → ${next}`, для event: name */
  label: string;
  detail?: string;
}

/** Последнее событие устройства — то, что подсвечивается в UI. */
export interface RecentEvent {
  deviceId: string;
  feature: string;
  name: string;
  data?: Record<string, unknown>;
  at: number;
}

interface EventsState {
  events: EventLogEntry[];
  /**
   * Последнее событие по каждому устройству. Держим отдельно от журнала:
   * кнопке нечего показывать кроме этого, а искать по журналу на каждый
   * рендер плитки — лишняя работа при живом потоке.
   */
  recent: Record<string, RecentEvent>;
  push: (e: Omit<EventLogEntry, 'id' | 'at'>) => void;
  noteEvent: (e: Omit<RecentEvent, 'at'>) => void;
  clear: () => void;
}

const MAX = 60;

export const useEventsStore = create<EventsState>((set) => ({
  events: [],
  recent: {},
  push: (e) =>
    set((s) => {
      const entry: EventLogEntry = {
        ...e,
        id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        at: Date.now(),
      };
      const next = [entry, ...s.events];
      if (next.length > MAX) next.length = MAX;
      return { events: next };
    }),
  noteEvent: (e) =>
    set((s) => ({ recent: { ...s.recent, [e.deviceId]: { ...e, at: Date.now() } } })),
  clear: () => set({ events: [], recent: {} }),
}));
