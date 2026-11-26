// @vitest-environment jsdom
// One casanomi-paper language in two moods: a warm light "paper" and a warm
// dark. The mode is System / Light / Dark — System follows the phone and
// reacts to it.
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { Text } from 'react-native';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { MODES, THEMES, ThemeProvider, useTheme } from '../src/ui/theme';

// The phone is in dark mode for the whole file.
vi.mock('react-native', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-native')>();
  return { ...actual, useColorScheme: () => 'dark' };
});

afterEach(cleanup);

function Probe() {
  const t = useTheme();
  return <Text testID="probe">{`${t.bg}|${t.text}|${t.card}`}</Text>;
}

describe('casanomi-paper themes', () => {
  it('light wears the spec colours', () => {
    const l = THEMES.light;
    expect(l.bg).toBe('#f4efe4');
    expect(l.card).toBe('#fffdf8');
    expect(l.border).toBe('#e8e1d0');
    expect(l.text).toBe('#1a1813');
    expect(l.accent).toBe('#cf9f33'); // gold, not terracotta
    expect(l.good).toBe('#2f7d4f');
    expect(l.cardRadius).toBe(22);
  });

  it('primary action is ink, not the accent', () => {
    expect(THEMES.light.primary).toBe('#1a1813');
    expect(THEMES.light.accent).not.toBe(THEMES.light.primary);
  });

  it('dark is warm and legible', () => {
    const d = THEMES.dark;
    expect(d.bg).toBe('#1c1813');
    expect(d.accent).toBe('#e0b23f');
    expect(d.text.toLowerCase()).not.toBe(d.card.toLowerCase()); // not ink-on-ink
  });

  it('system follows the OS: dark', () => {
    render(
      <ThemeProvider mode="system">
        <Probe />
      </ThemeProvider>,
    );
    expect(screen.getByTestId('probe').textContent).toContain(THEMES.dark.bg);
  });

  it('light mode stays light even when the OS is dark', () => {
    render(
      <ThemeProvider mode="light">
        <Probe />
      </ThemeProvider>,
    );
    expect(screen.getByTestId('probe').textContent).toContain(THEMES.light.bg);
  });

  it('dark mode stays dark even when the OS is light', () => {
    render(
      <ThemeProvider mode="dark">
        <Probe />
      </ThemeProvider>,
    );
    expect(screen.getByTestId('probe').textContent).toContain(THEMES.dark.bg);
  });

  it('offers System, Light and Dark to choose from', () => {
    expect(MODES.map((m) => m.id)).toEqual(['system', 'light', 'dark']);
    expect(MODES.map((m) => m.name)).toEqual(['System', 'Light', 'Dark']);
  });
});
