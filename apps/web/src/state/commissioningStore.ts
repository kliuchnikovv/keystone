import { create } from 'zustand';
import type { CommissioningEvent, DiscoveredDevice, RawDevice } from '../api/types';

export type Stage =
  | 'pick-ecosystem'
  | 'enter-code'
  | 'scanning'
  | 'commissioning'
  | 'success'
  | 'error';

export type Ecosystem = 'apple_home' | 'google_home' | 'smartthings' | 'alexa' | 'other';

interface CommissioningState {
  stage: Stage;
  ecosystem?: Ecosystem;
  setupCode: string;
  /** Сеть для устройства, которое ещё не в сети. Пусто — значит не нужна. */
  wifiSsid: string;
  wifiPassword: string;
  found: DiscoveredDevice[];
  addedRefs: Set<string>;
  currentRef?: string;
  /** Имя устройства, выбранного в списке, — показываем его на экране ввода кода. */
  candidateName?: string;
  progress: CommissioningEvent[];
  finalDevice?: RawDevice;
  errorMessage?: string;
  errorKind?: string;

  setStage: (s: Stage) => void;
  pickEcosystem: (e: Ecosystem) => void;
  setSetupCode: (code: string) => void;
  setWifi: (ssid: string, password: string) => void;
  submitCode: () => void;
  backTo: (s: Stage) => void;

  addFound: (d: DiscoveredDevice) => void;
  /** Пользователь выбрал найденное устройство — дальше нужен его setup-код. */
  chooseFound: (d: DiscoveredDevice) => void;
  /** Устройства нет в списке (или скан пуст) — ввести код вручную. */
  enterCodeManually: () => void;
  startCommission: (ref: string) => void;
  pushEvent: (e: CommissioningEvent) => void;
  markAdded: (ref: string) => void;
  /** Мягкий сброс — сохраняем found и addedRefs (для «Найти ещё»). */
  reset: () => void;
  /** Полный сброс при свежем заходе на экран. */
  resetAll: () => void;
}

const INITIAL: Pick<
  CommissioningState,
  | 'stage'
  | 'ecosystem'
  | 'setupCode'
  | 'wifiSsid'
  | 'wifiPassword'
  | 'found'
  | 'addedRefs'
  | 'currentRef'
  | 'candidateName'
  | 'progress'
  | 'finalDevice'
  | 'errorMessage'
  | 'errorKind'
> = {
  stage: 'pick-ecosystem',
  ecosystem: undefined,
  setupCode: '',
  wifiSsid: '',
  wifiPassword: '',
  found: [],
  addedRefs: new Set(),
  currentRef: undefined,
  candidateName: undefined,
  progress: [],
  finalDevice: undefined,
  errorMessage: undefined,
  errorKind: undefined,
};

export const useCommissioningStore = create<CommissioningState>((set) => ({
  ...INITIAL,

  setStage: (stage) => set({ stage }),

  // После выбора экосистемы сначала показываем, что вообще есть в эфире:
  // так пользователь опознаёт своё устройство до того, как искать код.
  pickEcosystem: (e) => set({ ecosystem: e, stage: 'scanning', found: [], currentRef: undefined }),

  setSetupCode: (code) => set({ setupCode: code }),
  setWifi: (wifiSsid, wifiPassword) => set({ wifiSsid, wifiPassword }),

  submitCode: () =>
    set((s) => ({
      stage: 'commissioning',
      progress: [],
      currentRef: `setup:${s.setupCode}`,
      errorMessage: undefined,
    })),

  backTo: (stage) => set({ stage }),

  addFound: (d) =>
    set((s) => {
      if (s.found.some((x) => x.ref === d.ref)) return {};
      return { found: [...s.found, d] };
    }),
  chooseFound: (d) =>
    set({ currentRef: d.ref, candidateName: d.name, stage: 'enter-code', setupCode: '' }),
  enterCodeManually: () =>
    set({ currentRef: undefined, candidateName: undefined, stage: 'enter-code', setupCode: '' }),
  startCommission: (ref) => set({ currentRef: ref, stage: 'commissioning', progress: [] }),
  pushEvent: (e) =>
    set((s) => {
      const next = { progress: [...s.progress, e] } as Partial<CommissioningState>;
      if (e.stage === 'done' && e.device) {
        next.stage = 'success';
        next.finalDevice = e.device;
        if (s.currentRef) {
          const added = new Set(s.addedRefs);
          added.add(s.currentRef);
          next.addedRefs = added;
        }
      } else if (e.stage === 'error') {
        next.stage = 'error';
        next.errorMessage = e.message;
        next.errorKind = e.kind;
      }
      return next;
    }),
  markAdded: (ref) =>
    set((s) => {
      const added = new Set(s.addedRefs);
      added.add(ref);
      return { addedRefs: added };
    }),
  reset: () =>
    set({
      stage: 'pick-ecosystem',
      ecosystem: undefined,
      setupCode: '',
      currentRef: undefined,
      progress: [],
      finalDevice: undefined,
      errorMessage: undefined,
    }),
  resetAll: () => set({ ...INITIAL, addedRefs: new Set() }),
}));
