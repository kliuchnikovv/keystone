import type { Ecosystem } from '../state/commissioningStore';

export interface EcosystemMeta {
  id: Ecosystem;
  label: string;
  /** Односимвольная эмблема (заменим на иллюстрации, когда придут от дизайнера). */
  glyph: string;
  steps: string[];
}

export const ECOSYSTEMS: EcosystemMeta[] = [
  {
    id: 'apple_home',
    label: 'Apple Home',
    glyph: '⌂',
    steps: [
      'Открой приложение «Дом» на iPhone или iPad.',
      'Долгий тап на нужном устройстве → «Настройки».',
      'Прокрути вниз → «Turn on Pairing Mode».',
      'Появится 11-значный код — введи его ниже.',
    ],
  },
  {
    id: 'google_home',
    label: 'Google Home',
    glyph: 'G',
    steps: [
      'Открой приложение Google Home.',
      'Тап на устройство → значок настроек.',
      '«Linked services» → «Add another Matter-enabled app».',
      'Скопируй 11-значный код.',
    ],
  },
  {
    id: 'smartthings',
    label: 'SmartThings',
    glyph: '◉',
    steps: [
      'Открой SmartThings → устройство → шестерёнка.',
      '«Share with Matter» → «Generate Code».',
      'Скопируй 11-значный код.',
    ],
  },
  {
    id: 'alexa',
    label: 'Amazon Alexa',
    glyph: '▷',
    steps: [
      'Открой Alexa → устройство → Settings.',
      '«Other Assistants and Apps» → «Add another Matter service».',
      'Скопируй 11-значный код.',
    ],
  },
];

export function ecosystemById(id?: Ecosystem): EcosystemMeta | undefined {
  return ECOSYSTEMS.find((e) => e.id === id);
}
