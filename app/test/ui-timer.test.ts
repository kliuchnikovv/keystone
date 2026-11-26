// The duration wheel: hours and minutes, the way a phone does it.
//
// The wheel snaps to the item in the middle band, so what the person sees in
// the band is what the timer will use.
import { describe, expect, it } from 'vitest';
import { hoursAt, minutesAt, wheelValue } from '../src/ui/DurationWheel';

describe('the duration wheel', () => {
  it('picks the value under the band', () => {
    // The band is 38 px tall; an item is 34 px tall.
    expect(wheelValue(0, 38, 34)).toBe(0);
    expect(wheelValue(17, 38, 34)).toBe(1);
    expect(wheelValue(51, 38, 34)).toBe(2);
    expect(wheelValue(85, 38, 34)).toBe(3);
  });

  it('never goes below the first item', () => {
    expect(wheelValue(-500, 38, 34)).toBe(0);
  });

  it('the hours go from 0 to 23', () => {
    expect(hoursAt(0)).toBe(0);
    expect(hoursAt(23)).toBe(23);
  });

  it('the minutes step by five', () => {
    expect(minutesAt(0)).toBe(0);
    expect(minutesAt(1)).toBe(5);
    expect(minutesAt(11)).toBe(55);
    expect(minutesAt(12)).toBe(0); // it wraps around
  });
});
