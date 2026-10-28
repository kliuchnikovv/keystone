import { createContext, useContext, type ReactNode } from 'react';
import { useColorScheme } from 'react-native';

// The keystone idiom: warm sandstone paper, ink text, a single terracotta accent
// ("ember") that only ever means two things — affordance, or live heat.
//
// The palette is a setting: "sandstone" is this paper-and-ink look (it follows the
// system light/dark), "ember" is the dark glass-and-amber look of the room mock —
// always dark, with warm translucent surfaces.

/** The palettes the app can wear. */
export type PaletteId = 'sandstone' | 'ember';

export interface PaletteInfo {
  id: PaletteId;
  name: string;
  hint: string;
}

export const PALETTES: PaletteInfo[] = [
  { id: 'sandstone', name: 'Sandstone', hint: 'Warm paper, terracotta accent' },
  { id: 'ember', name: 'Ember', hint: 'Dark glass, amber accent' },
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
  /** A 10–14 % wash of the accent, for selected chips and soft chips. */
  accentSoft: string;
  /** A quiet filled surface: secondary buttons, pressed rows. */
  fill: string;
  /** A cool accent — the one place the palette is allowed a second hue. */
  cool: string;
  onAccent: string;
  good: string;
  warn: string;
  bad: string;
  /** Which palette this theme came from. */
  palette: PaletteId;
  /** The radius of a card-like surface: 18 in sandstone, 22 in ember. */
  cardRadius: number;
  /** The bottom bar that holds the tabs. */
  tabBar: string;
}

const light: Theme = {
  bg: '#f5f1e8',
  card: '#ffffff',
  surface: '#ffffff',
  glass: 'rgba(255,255,255,0.70)',
  glassBorder: 'rgba(255,255,255,0.70)',
  glassSheen: 'rgba(255,255,255,0.95)',
  text: '#15171c',
  muted: '#6f6a5c',
  border: '#e3ddcd',
  accent: '#c4512a',
  accentSoft: 'rgba(196,81,42,0.12)',
  fill: 'rgba(0,0,0,0.06)',
  cool: '#2f6fb8',
  onAccent: '#ffffff',
  good: '#2f7a4d',
  warn: '#a8760f',
  bad: '#b53b3b',
  palette: 'sandstone',
  cardRadius: 18,
  tabBar: 'rgba(255,255,255,0.97)',
};

const dark: Theme = {
  bg: '#14161b',
  card: '#1d2027',
  surface: '#1d2027',
  glass: 'rgba(40,44,54,0.72)',
  glassBorder: 'rgba(255,255,255,0.10)',
  glassSheen: 'rgba(255,255,255,0.18)',
  text: '#f3efe7',
  muted: '#9a9384',
  border: '#2c303a',
  accent: '#e07a4a',
  accentSoft: 'rgba(224,122,74,0.18)',
  fill: 'rgba(255,255,255,0.08)',
  cool: '#5aa2e8',
  onAccent: '#14161b',
  good: '#4fbe7f',
  warn: '#e0a63c',
  bad: '#ef6b6b',
  palette: 'sandstone',
  cardRadius: 18,
  tabBar: 'rgba(20,22,27,0.97)',
};

// Ember: the room mock. Near-black warm ground, amber as the only accent,
// everything else a warm translucent pane of glass.
const ember: Theme = {
  bg: '#0d0602',
  card: 'rgba(255,255,255,0.07)',
  surface: '#171009',
  glass: 'rgba(255,255,255,0.058)',
  glassBorder: 'rgba(255,182,92,0.12)',
  glassSheen: 'rgba(255,238,148,0.12)',
  text: '#fffcf5',
  muted: 'rgba(255,205,128,0.62)',
  border: 'rgba(255,182,92,0.14)',
  accent: '#FF9F0A',
  accentSoft: 'rgba(255,152,32,0.26)',
  fill: 'rgba(255,255,255,0.06)',
  cool: '#64b4ff',
  onAccent: '#1a0b00',
  good: '#78f096',
  warn: '#ffc048',
  bad: '#ff6e5a',
  palette: 'ember',
  cardRadius: 22,
  tabBar: 'rgba(8,3,1,0.97)',
};

const ThemeContext = createContext<Theme>(light);

/** The theme the whole app wears: the chosen palette, and the system's light/dark for sandstone. */
export function ThemeProvider({ palette, children }: { palette: PaletteId; children: ReactNode }) {
  const scheme = useColorScheme();
  const theme = palette === 'ember' ? ember : scheme === 'dark' ? dark : light;
  return <ThemeContext.Provider value={theme}>{children}</ThemeContext.Provider>;
}

export function useTheme(): Theme {
  return useContext(ThemeContext);
}

/** Spacing/radius scale, so screens stay on the same grid. */
export const radius = { card: 18, chip: 11, pill: 999, field: 12 } as const;
export const space = { xs: 6, sm: 10, md: 14, lg: 20, xl: 28 } as const;
