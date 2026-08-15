/**
 * Design tokens — единый источник правды.
 * Значения — плоские JS-константы, чтобы шариться с React Native без CSS-переменных.
 * Для web они дублируются в theme.module.css как CSS custom properties.
 */

export const colors = {
  bg: '#141210',
  surface: '#1E1B18',
  surface2: '#262220',
  line: '#322D28',

  text: '#F4EFE7',
  text2: '#A69F94',
  text3: '#6D655C',

  accent: '#F4A860',
  accent2: '#D96B4A',

  success: '#7FB069',
  warn: '#E4A649',
  danger: '#D45D5D',
} as const;

export const radii = {
  sm: 12,
  md: 14,
  lg: 22,
  pill: 999,
} as const;

export const spacing = {
  xs: 4,
  sm: 8,
  md: 12,
  lg: 20,
  xl: 32,
  xxl: 48,
} as const;

export const typeface = {
  display: '"Fraunces", ui-serif, Georgia, serif',
  sans: '"Inter", system-ui, -apple-system, sans-serif',
  mono: 'ui-monospace, "SF Mono", Menlo, monospace',
} as const;

export const fontSize = {
  display: 32,
  title: 22,
  body: 15,
  caption: 12,
  micro: 11,
} as const;

export const motion = {
  fast: 160,
  base: 220,
  slow: 320,
  spring: 'cubic-bezier(0.34, 1.56, 0.64, 1)',
  easeOut: 'cubic-bezier(0.2, 0.8, 0.2, 1)',
} as const;
