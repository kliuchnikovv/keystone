import { create } from 'zustand';

export type ConnectionStatus = 'idle' | 'connecting' | 'live' | 'reconnecting' | 'error';

interface ConnectionState {
  status: ConnectionStatus;
  host: string;
  setStatus: (s: ConnectionStatus) => void;
  setHost: (h: string) => void;
}

export const useConnectionStore = create<ConnectionState>((set) => ({
  status: 'idle',
  host: import.meta.env.VITE_KEYSTONE_HOST ?? window.location.origin,
  setStatus: (status) => set({ status }),
  setHost: (host) => set({ host }),
}));
