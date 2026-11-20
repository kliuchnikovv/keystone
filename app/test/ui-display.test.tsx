// @vitest-environment jsdom
// Display: the theme, the units, and what the Home screen shows.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { DisplayScreen } from '../src/ui/DisplayScreen';

afterEach(cleanup);

function renderDisplay(over: Partial<Parameters<typeof DisplayScreen>[0]> = {}) {
  const props = {
    mode: 'system' as const,
    onMode: vi.fn(),
    temperature: 'C' as const,
    onTemperature: vi.fn(),
    showScenes: true,
    onShowScenes: vi.fn(),
    groupByRoom: true,
    onGroupByRoom: vi.fn(),
    onBack: vi.fn(),
    ...over,
  };
  render(<DisplayScreen {...props} />);
  return props;
}

describe('the display screen', () => {
  it('offers the three themes', () => {
    renderDisplay();
    expect(screen.getByLabelText('System')).toBeTruthy();
    expect(screen.getByLabelText('Light')).toBeTruthy();
    expect(screen.getByLabelText('Dark')).toBeTruthy();
  });

  it('picking a theme says so', () => {
    const props = renderDisplay();
    fireEvent.click(screen.getByLabelText('Dark'));
    expect(props.onMode).toHaveBeenCalledWith('dark');
  });

  it('picking Fahrenheit says so', () => {
    const props = renderDisplay();
    fireEvent.click(screen.getByLabelText('°F'));
    expect(props.onTemperature).toHaveBeenCalledWith('F');
  });

  it('the two switches say so', () => {
    const props = renderDisplay();
    fireEvent.click(screen.getByLabelText('Show scenes on Home'));
    expect(props.onShowScenes).toHaveBeenCalledWith(false);
    fireEvent.click(screen.getByLabelText('Group by room'));
    expect(props.onGroupByRoom).toHaveBeenCalledWith(false);
  });

  it('there is a way back', () => {
    const props = renderDisplay();
    fireEvent.click(screen.getByLabelText('Settings'));
    expect(props.onBack).toHaveBeenCalled();
  });
});
