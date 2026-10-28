// @vitest-environment jsdom
// The keyboard must not cover the field being typed into: the sheet rides up with it.
import './setup-dom';
import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Sheet } from '../src/ui/Sheet';

let listeners: Record<string, () => void> = {};
const view = {
  height: 768,
  offsetTop: 0,
  addEventListener(type: string, fn: () => void) {
    listeners[type] = fn;
  },
  removeEventListener() {},
};

beforeEach(() => {
  listeners = {};
  view.height = 768;
  view.offsetTop = 0;
  Object.defineProperty(window, 'visualViewport', { configurable: true, value: view });
  window.innerHeight = 768;
});

afterEach(() => {
  cleanup();
  Reflect.deleteProperty(window, 'visualViewport');
});

/** The sheet itself: the box the title sits in. */
function sheetBox(): HTMLElement {
  const title = screen.getAllByText('Kitchen plug')[0];
  if (!title) throw new Error('the sheet is not on the screen');
  return title.parentElement as HTMLElement;
}

describe('the sheet and the keyboard', () => {
  it('sits at the bottom while the keyboard is closed', () => {
    render(
      <Sheet visible title="Kitchen plug" onCancel={() => {}}>
        <input aria-label="Name" />
      </Sheet>,
    );
    expect(sheetBox().getAttribute('style') ?? '').toContain('translateY(0px)');
  });

  it('rides up when the keyboard takes the room', () => {
    render(
      <Sheet visible title="Kitchen plug" onCancel={() => {}}>
        <input aria-label="Name" />
      </Sheet>,
    );
    // The soft keyboard covers 320 pt of the screen.
    view.height = 768 - 320;
    act(() => {
      listeners.resize?.();
    });
    const style = sheetBox().getAttribute('style') ?? '';
    expect(style).toContain('translateY(-320px)');
  });
});
