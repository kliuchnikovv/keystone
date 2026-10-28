// Test helpers.
import { fireEvent } from '@testing-library/react';

/** A long press: react-native-web starts its long-press timer on mouse down.
 * The device sheet opens that way now — the "⋯" button is gone. */
export async function longPress(el: Element, ms = 700): Promise<void> {
  fireEvent.mouseDown(el);
  await new Promise((resolve) => setTimeout(resolve, ms));
  fireEvent.mouseUp(el);
}
