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

interface EventsState {
  events: EventLogEntry[];
  push: (e: Omit<EventLogEntry, 'id' | 'at'>) => void;
  clear: () => void;
}

const MAX = 60;

export const useEventsStore = create<EventsState>((set) => ({
  events: [],
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
  clear: () => set({ events: [] }),
}));
