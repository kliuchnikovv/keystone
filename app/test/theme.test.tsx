// @vitest-environment jsdom
// The palette decides what the app wears. Paper is the always-light look of the
// remote-access mock: a cream page, white cards with a hairline, terracotta for
// what can be pressed, green for what works — and it stays light even when the
// phone says it is dark.
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { Text } from 'react-native';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ThemeProvider, THEMES, useTheme } from '../src/ui/theme';

// The phone is in dark mode for the whole file.
vi.mock('react-native', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-native')>();
  return { ...actual, useColorScheme: () => 'dark' };
});

afterEach(cleanup);

function Probe() {
  const t = useTheme();
  return <Text testID="probe">{`${t.palette}|${t.bg}|${t.secondaryBg}|${t.secondaryBorder}`}</Text>;
}

describe('the paper palette', () => {
  it('wears the mock colours', () => {
    const p = THEMES.paper;
    expect(p.bg).toBe('#f5f1e8'); // the cream page
    expect(p.card).toBe('#ffffff'); // a white card
    expect(p.border).toBe('#e3ddcd'); // with a hairline
    expect(p.accent).toBe('#c4512a'); // terracotta
    expect(p.good).toBe('#2f7d4f'); // green for what works
    expect(p.cardRadius).toBe(16);
  });

  it('is always light, even when the phone is dark', () => {
    render(
      <ThemeProvider palette="paper">
        <Probe />
      </ThemeProvider>,
    );
    const text = screen.getByTestId('probe').textContent ?? '';
    expect(text).toContain('paper');
    expect(text).toContain(THEMES.paper.bg);
  });

  it('draws its quiet button as an outline', () => {
    const p = THEMES.paper;
    expect(p.secondaryBg).toBe('transparent');
    expect(p.secondaryBorder).toBe(p.border);
  });

  it('leaves sandstone to the phone', () => {
    render(
      <ThemeProvider palette="sandstone">
        <Probe />
      </ThemeProvider>,
    );
    // The phone is dark, so sandstone is the dark one — not the light paper.
    expect(screen.getByTestId('probe').textContent).toContain('#14161b');
  });
});
