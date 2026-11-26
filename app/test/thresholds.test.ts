// The widgets that need more than a name and a value: the thermostat's ring
// and the grid of readings.
import { describe, expect, it } from 'vitest';
import { toneFor } from '../src/model/thresholds';

describe('the thresholds of a reading', () => {
  it('a normal temperature is green', () => {
    expect(toneFor('temperature', 22)).toBe('good');
  });

  it('a freezing one is not', () => {
    expect(toneFor('temperature', 5)).toBe('warn');
  });

  it('a normal humidity is green', () => {
    expect(toneFor('humidity', 50)).toBe('good');
    expect(toneFor('humidity', 85)).toBe('warn');
  });

  it('an unknown reading has no tone', () => {
    expect(toneFor('battery', 80)).toBe('none');
  });
});
