import { useColorScheme } from 'react-native';

// The keystone idiom: warm sandstone paper, ink text, a single terracotta accent
// ("ember") that only ever means two things — affordance, or live heat.
export interface Theme {
  /** Page background. */
  bg: string;
  /** An opaque surface: fields, sheets, solid rows. */
  card: string;
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
  onAccent: string;
  good: string;
  warn: string;
  bad: string;
}

const light: Theme = {
  bg: '#f5f1e8',
  card: '#ffffff',
  glass: 'rgba(255,255,255,0.70)',
  glassBorder: 'rgba(255,255,255,0.70)',
  glassSheen: 'rgba(255,255,255,0.95)',
  text: '#15171c',
  muted: '#6f6a5c',
  border: '#e3ddcd',
  accent: '#c4512a',
  accentSoft: 'rgba(196,81,42,0.12)',
  onAccent: '#ffffff',
  good: '#2f7a4d',
  warn: '#a8760f',
  bad: '#b53b3b',
};

const dark: Theme = {
  bg: '#14161b',
  card: '#1d2027',
  glass: 'rgba(40,44,54,0.72)',
  glassBorder: 'rgba(255,255,255,0.10)',
  glassSheen: 'rgba(255,255,255,0.18)',
  text: '#f3efe7',
  muted: '#9a9384',
  border: '#2c303a',
  accent: '#e07a4a',
  accentSoft: 'rgba(224,122,74,0.18)',
  onAccent: '#14161b',
  good: '#4fbe7f',
  warn: '#e0a63c',
  bad: '#ef6b6b',
};

export function useTheme(): Theme {
  return useColorScheme() === 'dark' ? dark : light;
}

/** Spacing/radius scale, so screens stay on the same grid. */
export const radius = { card: 18, chip: 11, pill: 999, field: 12 } as const;
export const space = { xs: 6, sm: 10, md: 14, lg: 20, xl: 28 } as const;