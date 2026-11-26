// The app must never sit on a blank screen because a font did not load: it
// waits for Geist, and when the wait fails it draws anyway (system font).
// @vitest-environment jsdom
import './setup-dom';
import { cleanup, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';

// expo-font: the fonts never arrive.
vi.mock('expo-font', () => ({
  useFonts: () => [false, new Error('no fonts here')],
  loadAsync: async () => {
    throw new Error('no fonts here');
  },
}));

// The phone's keychain: nothing saved, so the app asks for the hub address.
vi.mock('expo-secure-store', () => ({
  getItemAsync: async () => null,
  setItemAsync: async () => {},
  deleteItemAsync: async () => {},
}));

afterEach(cleanup);

describe('the app without its fonts', () => {
  it('still renders', async () => {
    const { default: App } = await import('../App');
    render(<App />);
    // The Connect screen is there, not a blank page.
    await waitFor(() => expect(screen.getByLabelText('Hub address')).toBeTruthy());
  });
});
