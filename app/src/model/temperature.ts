// The unit a temperature is shown in.
//
// The wire always speaks 0.1 °C (int16) — the unit only changes how it is
// written, never what goes on the wire.
import type { TemperatureUnit } from '../palette';

export type { TemperatureUnit };

export function cToF(c: number): number {
  return c * 9 / 5 + 32;
}

export function fToC(f: number): number {
  return (f - 32) * 5 / 9;
}

/** A temperature in the unit the person reads: «23.4 °C» / «74.1 °F». */
export function formatTemperature(celsius: number, unit: TemperatureUnit = 'C'): string {
  if (unit === 'F') return `${cToF(celsius).toFixed(1)} °F`;
  return `${celsius.toFixed(1)} °C`;
}

/** A target temperature in the unit the person reads. */
export function setpointLabel(celsius: number, unit: TemperatureUnit = 'C'): string {
  if (unit === 'F') return `${cToF(celsius).toFixed(1)} °F`;
  return `${celsius.toFixed(1)} °C`;
}
