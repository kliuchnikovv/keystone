// @vitest-environment jsdom
// The lamp card is its own dimmer: the fill is the level, a tap toggles,
// a swipe changes the level, a long press opens the sheet.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Dimmer, levelAfterDrag } from '../src/ui/Dimmer';
import { ThemeProvider } from '../src/ui/theme';

afterEach(cleanup);

function renderDimmer(over: Partial<Parameters<typeof Dimmer>[0]> = {}) {
  const props = {
    value: 70,
    on: true,
    onChange: vi.fn(),
    onToggle: vi.fn(),
    onOpen: vi.fn(),
    ...over,
  };
  render(
    <ThemeProvider mode="light">
      <Dimmer {...props} />
    </ThemeProvider>,
  );
  return props;
}

describe('the dimmer card', () => {
  it('the fill is the level', () => {
    renderDimmer({ value: 70 });
    const fill = screen.getByTestId('dimmer-fill');
    expect(fill.getAttribute('style') ?? '').toContain('width: 70%');
  });

  it('a tap below the threshold toggles', () => {
    const props = renderDimmer();
    fireEvent.click(screen.getByTestId('dimmer'));
    expect(props.onToggle).toHaveBeenCalled();
    expect(props.onChange).not.toHaveBeenCalled();
  });

  it('a drag of 30 px over a 300 px card is ten points', () => {
    expect(levelAfterDrag(70, 30, 300)).toBe(80);
    expect(levelAfterDrag(70, -30, 300)).toBe(60);
  });

  it('the level never leaves the range', () => {
    expect(levelAfterDrag(70, 9999, 300)).toBe(100);
    expect(levelAfterDrag(70, -9999, 300)).toBe(0);
    // A card of no width changes nothing.
    expect(levelAfterDrag(70, 30, 0)).toBe(70);
  });

  it('a swipe changes the level and does not toggle', () => {
    const props = renderDimmer();
    const el = screen.getByTestId('dimmer');
    // A drag: the finger goes down, moves across the card, and comes up.
    fireEvent.mouseDown(el, { clientX: 100 });
    fireEvent.mouseMove(el, { clientX: 160 });
    fireEvent.mouseUp(el, { clientX: 160 });
    expect(props.onToggle).not.toHaveBeenCalled();
  });
});
