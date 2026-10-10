// Chip health as shown on the Hub screen.
import type { Chip } from '../protocol/messages';
import { formatUptime } from './format';

export type Tone = 'good' | 'warn' | 'bad';

export interface ChipSummary {
  title: string;
  tone: Tone;
  status: string;
  details: string[];
}

export function chipSummary(c: Chip): ChipSummary {
  const t = c.telemetry;
  const details: string[] = [];
  if (t.uptime_s !== undefined) details.push(`Up ${formatUptime(t.uptime_s)}`);
  if (t.heap !== undefined) details.push(`${Math.round(t.heap / 1024)} KB free`);
  if (c.reboots > 0) details.push(`${c.reboots} reboot${c.reboots === 1 ? '' : 's'} seen`);
  if (t.crc_errors !== undefined && t.crc_errors > 0) details.push(`${t.crc_errors} link errors`);

  let tone: Tone = 'good';
  let status = 'Online';
  if (c.status === 'silent') {
    tone = 'bad';
    status = 'Not responding';
  } else if (c.status === 'seen') {
    tone = 'warn';
    status = 'Starting';
  } else if (c.reboots >= 3 || (t.crc_errors ?? 0) > 100) {
    tone = 'warn';
  }
  return { title: c.label.charAt(0).toUpperCase() + c.label.slice(1), tone, status, details };
}
