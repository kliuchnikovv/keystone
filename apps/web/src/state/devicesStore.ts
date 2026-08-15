import { create } from 'zustand';
import type { Device } from '../api/types';

export type PendingKey = string;

interface DevicesState {
  devices: Map<string, Device>;
  liveState: Map<string, unknown>;
  pending: Map<PendingKey, number>;

  setDevices: (list: Device[]) => void;
  upsertDevice: (d: Device) => void;
  removeDevice: (id: string) => void;

  setLive: (deviceId: string, feature: string, key: string, value: unknown) => void;
  markPending: (deviceId: string, feature: string, key: string) => void;
  clearPending: (deviceId: string, feature: string, key: string) => void;
}

export const liveKey = (id: string, feature: string, key: string): string =>
  `${id}|${feature}|${key}`;

export const useDevicesStore = create<DevicesState>((set) => ({
  devices: new Map(),
  liveState: new Map(),
  pending: new Map(),

  setDevices: (list) =>
    set(() => {
      const m = new Map<string, Device>();
      for (const d of list) m.set(d.id, d);
      return { devices: m };
    }),

  upsertDevice: (d) =>
    set((s) => {
      const m = new Map(s.devices);
      m.set(d.id, d);
      return { devices: m };
    }),

  removeDevice: (id) =>
    set((s) => {
      const m = new Map(s.devices);
      m.delete(id);
      return { devices: m };
    }),

  setLive: (id, feature, key, value) =>
    set((s) => {
      const m = new Map(s.liveState);
      m.set(liveKey(id, feature, key), value);
      const p = new Map(s.pending);
      p.delete(liveKey(id, feature, key));
      return { liveState: m, pending: p };
    }),

  markPending: (id, feature, key) =>
    set((s) => {
      const p = new Map(s.pending);
      p.set(liveKey(id, feature, key), Date.now());
      return { pending: p };
    }),

  clearPending: (id, feature, key) =>
    set((s) => {
      const p = new Map(s.pending);
      p.delete(liveKey(id, feature, key));
      return { pending: p };
    }),
}));
