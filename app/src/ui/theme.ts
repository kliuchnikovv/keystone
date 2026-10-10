import { useColorScheme } from 'react-native';

export interface Theme {
  bg: string;
  card: string;
  text: string;
  muted: string;
  border: string;
  accent: string;
  onAccent: string;
  good: string;
  warn: string;
  bad: string;
}

const light: Theme = {
  bg: '#f2f3f7', card: '#ffffff', text: '#14161c', muted: '#6a7080', border: '#e1e4ec',
  accent: '#2f6df6', onAccent: '#ffffff', good: '#1f9d55', warn: '#c77d0a', bad: '#d33b3b',
};
const dark: Theme = {
  bg: '#0e1015', card: '#181b22', text: '#eef0f6', muted: '#98a0b3', border: '#2a2f3b',
  accent: '#5b8cff', onAccent: '#0e1015', good: '#3ccf7e', warn: '#e8a33d', bad: '#ff6b6b',
};

export function useTheme(): Theme {
  return useColorScheme() === 'dark' ? dark : light;
}
