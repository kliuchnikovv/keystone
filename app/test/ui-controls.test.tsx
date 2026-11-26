// @vitest-environment jsdom
// The main action is ink, not the gold accent: the gold is decoration.
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { Button } from '../src/ui/controls';
import { THEMES, ThemeProvider } from '../src/ui/theme';

afterEach(cleanup);

function renderButton(kind: 'primary' | 'secondary' | 'plain' | 'danger') {
  render(
    <ThemeProvider mode="light">
      <Button kind={kind} label="Go" onPress={() => {}} />
    </ThemeProvider>,
  );
  return screen.getByLabelText('Go');
}

describe('the button', () => {
  it('the primary one is ink', () => {
    const el = renderButton('primary');
    const style = el.getAttribute('style') ?? '';
    expect(style).toContain('rgb(26, 24, 19)'); // #1a1813
    expect(style).not.toContain('rgb(207, 159, 51)'); // not the gold accent
    // The label on it is the cream ink.
    expect(screen.getByText('Go').getAttribute('style') ?? '').toContain('rgb(253, 247, 234)');
  });

  it('the quiet one is still quiet', () => {
    const el = renderButton('secondary');
    const style = el.getAttribute('style') ?? '';
    // An outline, not a filled slab.
    expect(style).toContain('rgba(0, 0, 0, 0)');
    expect(style).toContain('rgb(232, 225, 208)'); // the hairline
    expect(style).not.toContain('rgb(207, 159, 51)');
    // Its label is ink.
    expect(screen.getByText('Go').getAttribute('style') ?? '').toContain('rgb(26, 24, 19)');
  });

  it('the theme says which colours those are', () => {
    expect(THEMES.light.primary).toBe('#1a1813');
    expect(THEMES.light.onPrimary).toBe('#fdf7ea');
    expect(THEMES.light.accent).toBe('#cf9f33');
  });
});
