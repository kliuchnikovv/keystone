import { describe, expect, it } from 'vitest';
import type { Attr, Entity, Value } from '../src/protocol/messages';
import {
  PRESENCE_PHONE_NAME,
  phoneEntity,
  presenceReportArgs,
  presenceState,
  presenceText,
  presenceTitle,
} from '../src/model/presence';

const phone = (value: Value | null): Entity => ({
  id: 0x0f80,
  name: 'Anna phone',
  room: null,
  chip: 0xfe,
  device: 'phone',
  attrs: [{ id: 0x52, name: 'home', vt: 'bool', flags: ['r', 'rep'], value } as Attr],
});

const home = (): Entity => phone(true);
const away = (): Entity => phone(false);
const silent = (): Entity => phone(null);

describe('the phone as a device', () => {
  it('is found among the entities', () => {
    expect(phoneEntity([home()])?.id).toBe(0x0f80);
    expect(phoneEntity([])).toBeUndefined();
  });

  it('shows the name the person gave it', () => {
    expect(presenceTitle(home())).toBe('Anna phone');
    expect(presenceTitle(undefined)).toBe(PRESENCE_PHONE_NAME);
  });
});

describe('the presence state', () => {
  it('is home when the phone says so', () => {
    expect(presenceState([home()])).toBe('home');
  });
  it('is away when the phone says so', () => {
    expect(presenceState([away()])).toBe('away');
  });
  it('is unknown when the phone has not reported — NOT away', () => {
    // A phone that stopped talking must not switch the lights off.
    expect(presenceState([silent()])).toBe('unknown');
    expect(presenceState([])).toBe('unknown');
  });
  it('reads in words', () => {
    expect(presenceText('home')).toBe('At home');
    expect(presenceText('away')).toBe('Away');
    expect(presenceText('unknown')).toBe('Not known yet');
  });
});

describe('the report', () => {
  it('names the phone, so each phone is its own entity', () => {
    expect(presenceReportArgs('My phone', true)).toEqual({ name: 'My phone', home: true });
    expect(presenceReportArgs('My phone', false)).toEqual({ name: 'My phone', home: false });
  });
});
