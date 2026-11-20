// @vitest-environment jsdom
// The «+» menu: three ways to add something.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { AddMenu } from '../src/ui/AddMenu';

afterEach(cleanup);

describe('the add menu', () => {
  it('offers a device, a scene and an automation', () => {
    render(<AddMenu visible onClose={() => {}} onDevice={() => {}} onScene={() => {}} onAutomation={() => {}} />);
    expect(screen.getByLabelText('Device')).toBeTruthy();
    expect(screen.getByLabelText('Scene')).toBeTruthy();
    expect(screen.getByLabelText('Automation')).toBeTruthy();
    // The transport is the hub's business, not the user's.
    expect(screen.queryByText(/Zigbee/)).toBeNull();
    expect(screen.queryByText(/Bluetooth/)).toBeNull();
    expect(screen.queryByText(/Matter/)).toBeNull();
  });

  it('each row calls back', () => {
    const onDevice = vi.fn();
    const onScene = vi.fn();
    const onAutomation = vi.fn();
    render(<AddMenu visible onClose={() => {}} onDevice={onDevice} onScene={onScene} onAutomation={onAutomation} />);
    fireEvent.click(screen.getByLabelText('Device'));
    expect(onDevice).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Scene'));
    expect(onScene).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Automation'));
    expect(onAutomation).toHaveBeenCalled();
  });

  it('shows nothing when it is closed', () => {
    render(<AddMenu visible={false} onClose={() => {}} onDevice={() => {}} onScene={() => {}} onAutomation={() => {}} />);
    expect(screen.queryByLabelText('Device')).toBeNull();
  });
});
