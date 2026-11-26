// @vitest-environment jsdom
// The dock: three tabs in a floating pill, and the round ink button beside it.
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Dock } from '../src/ui/Home';

afterEach(cleanup);

describe('the dock', () => {
  it('has three tabs and a separate add button', () => {
    render(<Dock tab="home" onTab={() => {}} onAdd={() => {}} />);
    expect(screen.getByLabelText('Home')).toBeTruthy();
    expect(screen.getByLabelText('Automations')).toBeTruthy();
    expect(screen.getByLabelText('Settings')).toBeTruthy();
    // Hub and History live inside Settings now, not in the dock.
    expect(screen.queryByLabelText('Hub')).toBeNull();
    expect(screen.queryByLabelText('History')).toBeNull();
    // The add button stands beside the pill, not in it.
    expect(screen.getByLabelText('Add')).toBeTruthy();
  });

  it('says which tab is the current one', () => {
    render(<Dock tab="settings" onTab={() => {}} onAdd={() => {}} />);
    // The chosen tab is written in the accent colour; the others are quiet.
    expect(screen.getByTestId('tab-Settings').getAttribute('style') ?? '').toContain('rgb(207, 159, 51)');
    expect(screen.getByTestId('tab-Home').getAttribute('style') ?? '').toContain('rgb(116, 109, 93)');
  });

  it('the add button is a separate pressable, not a tab', () => {
    render(<Dock tab="home" onTab={() => {}} onAdd={() => {}} />);
    expect(screen.getByLabelText('Add').getAttribute('role')).toBe('button');
    expect(screen.getByLabelText('Home').getAttribute('role')).toBe('tab');
  });

  it('pressing it calls back', () => {
    const onAdd = vi.fn();
    const onTab = vi.fn();
    render(<Dock tab="home" onTab={onTab} onAdd={onAdd} />);
    screen.getByLabelText('Add').click();
    expect(onAdd).toHaveBeenCalled();
    screen.getByLabelText('Settings').click();
    expect(onTab).toHaveBeenCalledWith('settings');
  });
});
