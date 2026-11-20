// When a reading is worth worrying about.
//
// The hub sends raw numbers; the app decides what is normal. A temperature of
// 5 °C is not fine, a humidity of 85 % is not fine — the reading that has no
// threshold (the battery) is just a number.
export type Tone = 'good' | 'warn' | 'bad' | 'none';

interface Threshold {
  /** Below this, the reading is low. */
  low?: number;
  /** Above this, the reading is high. */
  high?: number;
}

/** What counts as normal, per reading. */
const THRESHOLDS: Record<string, Threshold> = {
  temperature: { low: 5, high: 30 },
  humidity: { low: 20, high: 75 },
  illuminance: { high: 5000 },
  co2: { high: 1200 },
  pm25: { high: 35 },
};

/** The tone of a reading: green while it is normal, amber when it is not. */
export function toneFor(name: string, value: number): Tone {
  const limit = THRESHOLDS[name];
  if (limit === undefined) return 'none';
  if (limit.low !== undefined && value <= limit.low) return 'warn';
  if (limit.high !== undefined && value >= limit.high) return 'warn';
  return 'good';
}
