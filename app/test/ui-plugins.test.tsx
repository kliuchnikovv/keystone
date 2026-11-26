// @vitest-environment jsdom
// The plugin store is a shell: the hub has no plugin mechanism yet.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { PluginsScreen } from '../src/ui/PluginsScreen';

afterEach(cleanup);

describe('the plugin store', () => {
  it('has the two tabs', () => {
    render(<PluginsScreen onBack={() => {}} />);
    expect(screen.getByLabelText('Browse')).toBeTruthy();
    expect(screen.getByLabelText('Installed')).toBeTruthy();
  });

  it('Browse shows the plugins and their tags', () => {
    render(<PluginsScreen onBack={() => {}} />);
    expect(screen.getByLabelText('Plugin Energy')).toBeTruthy();
    expect(screen.getByLabelText('Install Energy')).toBeTruthy();
    expect(screen.getByLabelText('Tag Devices')).toBeTruthy();
  });

  it('Installed starts empty', () => {
    render(<PluginsScreen onBack={() => {}} />);
    fireEvent.click(screen.getByLabelText('Installed'));
    expect(screen.getByText('No plugins yet')).toBeTruthy();
  });

  it('installing moves the plugin to Installed', () => {
    render(<PluginsScreen onBack={() => {}} />);
    fireEvent.click(screen.getByLabelText('Install Energy'));
    fireEvent.click(screen.getByLabelText('Installed'));
    expect(screen.getByLabelText('Plugin Energy')).toBeTruthy();
  });

  it('the search narrows the list', () => {
    render(<PluginsScreen onBack={() => {}} />);
    fireEvent.change(screen.getByLabelText('Search plugins'), { target: { value: 'weather' } });
    expect(screen.getByLabelText('Plugin Weather')).toBeTruthy();
    expect(screen.queryByLabelText('Plugin Guard')).toBeNull();
  });

  it('there is a way back', () => {
    const onBack = vi.fn();
    render(<PluginsScreen onBack={onBack} />);
    fireEvent.click(screen.getByLabelText('Settings'));
    expect(onBack).toHaveBeenCalled();
  });
});
