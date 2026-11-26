// Test helpers.
import { fireEvent, screen } from '@testing-library/react';

/** A long press: react-native-web starts its long-press timer on mouse down.
 * The device sheet opens that way now — the "⋯" button is gone. */
export async function longPress(el: Element, ms = 700): Promise<void> {
  fireEvent.mouseDown(el);
  await new Promise((resolve) => setTimeout(resolve, ms));
  fireEvent.mouseUp(el);
}

/** The Hub and the History live inside Settings now: the dock has three tabs. */
export function openHubTab(): void {
  fireEvent.click(screen.getByLabelText('Settings'));
  fireEvent.click(screen.getByLabelText('Hub & health'));
}

export function openHistoryTab(): void {
  fireEvent.click(screen.getByLabelText('Settings'));
  fireEvent.click(screen.getByLabelText('History'));
}
