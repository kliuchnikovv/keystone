import { createContext, useContext, type ReactNode } from 'react';
import { useColorScheme } from 'react-native';

// The keystone idiom, restated in the casanomi-paper language: a warm page,
// ink text, a single honey-gold accent that only ever means two things —
// affordance, or live heat — and a near-black primary action.
//
// There are two themes in that one language: `paperLight` (a warm cream page)
// and `paperDark` (a warm dark ground, never grey). The app chooses between
// them with a mode: System (follow the phone), Light, or Dark.

/** How the app chooses its theme. */
export type ThemeMode = 'system' | 'light' | 'dark';

export interface ModeInfo {
  id: ThemeMode;
  name: string;
  hint: string;
}

/** The modes a picker offers, in the order it offers them. */
export const MODES: ModeInfo[] = [
  { id: 'system', name: 'System', hint: 'Follow the phone' },
  { id: 'light', name: 'Light', hint: 'Warm paper' },
  { id: 'dark', name: 'Dark', hint: 'Warm night' },
];

export interface Theme {
  /** Page background. */
  bg: string;
  /** An opaque surface: fields, sheets, solid rows. */
  card: string;
  /** An opaque surface for what floats above the page: sheets, dialogs. */
  surface: string;
  /** A translucent surface for cards that sit on the room's light. */
  glass: string;
  /** The hairline that gives a glass card its edge. */
  glassBorder: string;
  /** The specular highlight along a glass card's top edge. */
  glassSheen: string;
  text: string;
  muted: string;
  border: string;
  accent: string;
  /** The deeper ink of the accent — accent text on a soft accent chip. */
  accentInk: string;
  /** A 10–16 % wash of the accent, for selected chips and soft chips. */
  accentSoft: string;
  /** A quiet filled surface: secondary buttons, pressed rows. */
  fill: string;
  /** The quiet button: a filled surface in the dark theme, an outline on paper. */
  secondaryBg: string;
  /** The line around the quiet button. */
  secondaryBorder: string;
  /** A cool accent — the one place the theme is allowed a second hue. */
  cool: string;
  onAccent: string;
  /** The main action: near-black ink, not the gold accent. */
  primary: string;
  /** The ink on the main action. */
  onPrimary: string;
  good: string;
  /** The ink of a note that says everything works. */
  goodInk: string;
  /** The wash behind such a note. */
  goodSoft: string;
  /** The line around such a note. */
  goodBorder: string;
  warn: string;
  bad: string;
  /** The radius of a card-like surface. */
  cardRadius: number;
  /** The bottom bar that holds the tabs. */
  tabBar: string;
}

// Paper: a warm cream page, cards a touch warmer than white, honey gold for
// what can be pressed, green for what works.
const paperLight: Theme = {
  bg: '#f4efe4',
  card: '#fffdf8',
  surface: '#fffdf8',
  glass: 'rgba(255,253,248,0.72)',
  glassBorder: 'rgba(232,225,208,0.90)',
  glassSheen: 'rgba(255,253,248,0.95)',
  text: '#1a1813',
  muted: '#746d5d',
  border: '#e8e1d0',
  accent: '#cf9f33',
  accentInk: '#9a7117',
  accentSoft: 'rgba(207,159,51,0.16)',
  fill: 'rgba(26,24,19,0.05)',
  // On paper a quiet button is an outline, not a grey slab.
  secondaryBg: 'transparent',
  secondaryBorder: '#e8e1d0',
  cool: '#2f6fb8',
  onAccent: '#fffdf8',
  primary: '#1a1813',
  onPrimary: '#fdf7ea',
  good: '#2f7d4f',
  goodInk: '#215c3a',
  goodSoft: 'rgba(47,125,79,0.09)',
  goodBorder: 'rgba(47,125,79,0.22)',
  warn: '#a8760f',
  bad: '#b53b3b',
  cardRadius: 22,
  tabBar: 'rgba(255,253,248,0.97)',
};

// Night: the same language, warm instead of cool — a dark brown ground, cards
// a touch lighter, the same honey gold, and a near-white ink as the action.
const paperDark: Theme = {
  bg: '#1c1813',
  card: '#272119',
  surface: '#2c251c',
  glass: 'rgba(58,49,38,0.66)',
  glassBorder: 'rgba(232,225,208,0.14)',
  glassSheen: 'rgba(242,236,224,0.10)',
  text: '#f2ece0',
  muted: '#a89d89',
  border: '#3b332a',
  accent: '#e0b23f',
  accentInk: '#f0c96a',
  accentSoft: 'rgba(224,178,63,0.18)',
  fill: 'rgba(242,236,224,0.08)',
  secondaryBg: 'rgba(242,236,224,0.08)',
  secondaryBorder: 'rgba(242,236,224,0.12)',
  cool: '#7fb2e8',
  onAccent: '#1c1813',
  primary: '#f7f1e4',
  onPrimary: '#1a1813',
  good: '#5fbf85',
  goodInk: '#b6e8cb',
  goodSoft: 'rgba(95,191,133,0.12)',
  goodBorder: 'rgba(95,191,133,0.28)',
  warn: '#e0b23f',
  bad: '#e97a6a',
  cardRadius: 22,
  tabBar: 'rgba(28,24,19,0.97)',
};

const ThemeContext = createContext<Theme>(paperLight);

/** Both themes, so a picker can show what a choice looks like before it is made. */
export const THEMES: Record<'light' | 'dark', Theme> = { light: paperLight, dark: paperDark };

/** The theme the whole app wears: the chosen mode, and the phone's own scheme for System. */
export function ThemeProvider({ mode, children }: { mode: ThemeMode; children: ReactNode }) {
  const scheme = useColorScheme();
  const theme = mode === 'system' ? (scheme === 'dark' ? paperDark : paperLight) : mode === 'dark' ? paperDark : paperLight;
  return <ThemeContext.Provider value={theme}>{children}</ThemeContext.Provider>;
}

export function useTheme(): Theme {
  return useContext(ThemeContext);
}

/** Spacing/radius scale, so screens stay on the same grid. */
export const radius = { card: 22, chip: 11, pill: 999, field: 12 } as const;
export const space = { xs: 6, sm: 10, md: 14, lg: 20, xl: 28 } as const;
