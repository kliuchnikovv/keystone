// @vitest-environment jsdom
// The screen behind «+ → Automation»: what should trigger it?
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { AutomationChooser } from '../src/ui/AutomationChooser';

afterEach(cleanup);

function renderChooser(over: Partial<Parameters<typeof AutomationChooser>[0]> = {}) {
  const props = {
    onBack: vi.fn(),
    onSchedule: vi.fn(),
    onTimer: vi.fn(),
    onRule: vi.fn(),
    ...over,
  };
  render(<AutomationChooser {...props} />);
  return props;
}

describe('the automation chooser', () => {
  it('offers the three kinds', () => {
    renderChooser();
    expect(screen.getByLabelText('Schedule')).toBeTruthy();
    expect(screen.getByLabelText('Timer')).toBeTruthy();
    expect(screen.getByLabelText('Rule')).toBeTruthy();
    expect(screen.getByText('What should trigger it?')).toBeTruthy();
  });

  it('each kind opens its own editor', () => {
    const props = renderChooser();
    fireEvent.click(screen.getByLabelText('Schedule'));
    expect(props.onSchedule).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Timer'));
    expect(props.onTimer).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Rule'));
    expect(props.onRule).toHaveBeenCalled();
  });

  it('the suggestions sit above the three kinds', () => {
    const onPickSuggestion = vi.fn();
    renderChooser({
      suggestions: [
        {
          id: 'motion-1',
          title: 'Lamp on when there is motion',
          detail: 'You have a motion sensor.',
          draft: { trigger: null, conditions: [], action: null, window: null, cooldown_s: null },
        },
      ],
      onPickSuggestion,
    });
    const labels = screen.getAllByText(/Lamp on when there is motion|Schedule/);
    expect(labels.length).toBeGreaterThan(0);
    fireEvent.click(screen.getByLabelText(/^Add Lamp on when/));
    expect(onPickSuggestion).toHaveBeenCalled();
  });

  it('there is a way back', () => {
    const props = renderChooser();
    fireEvent.click(screen.getByLabelText('Back'));
    expect(props.onBack).toHaveBeenCalled();
  });
});
