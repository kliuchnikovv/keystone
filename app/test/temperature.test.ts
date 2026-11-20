// The unit a temperature is shown in. The wire always speaks 0.1 °C.
import { describe, expect, it } from 'vitest';
import { formatValue } from '../src/model/format';
import { cToF, fToC, formatTemperature, setpointLabel } from '../src/model/temperature';

describe('the temperature unit', () => {
  it('converts both ways', () => {
    expect(cToF(0)).toBe(32);
    expect(cToF(100)).toBe(212);
    expect(fToC(32)).toBe(0);
    expect(fToC(212)).toBe(100);
    // Round trip.
    expect(fToC(cToF(23.4))).toBeCloseTo(23.4, 10);
  });

  it('writes a temperature in the unit the person reads', () => {
    expect(formatTemperature(23.4, 'C')).toBe('23.4 °C');
    expect(formatTemperature(23.4, 'F')).toBe('74.1 °F');
    expect(setpointLabel(24, 'C')).toBe('24.0 °C');
    expect(setpointLabel(24, 'F')).toBe('75.2 °F');
  });

  it('the default is Celsius, so the old calls keep working', () => {
    expect(formatTemperature(23.4)).toBe('23.4 °C');
  });

  it('formatValue speaks the unit, and leaves the other attributes alone', () => {
    const temp = { name: 'temperature', value: 234 };
    expect(formatValue(temp)).toBe('23.4 °C');
    expect(formatValue(temp, 'F')).toBe('74.1 °F');
    // The humidity is a percent in either unit.
    expect(formatValue({ name: 'humidity', value: 580 }, 'F')).toBe('58 %');
    expect(formatValue({ name: 'brightness', value: 254 }, 'F')).toBe('100 %');
  });
});
