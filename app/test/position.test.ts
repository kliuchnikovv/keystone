import { describe, expect, it } from 'vitest';
import { readPosition, roundCoordinate, type LocationApi } from '../src/native/position';

const api = (over: Partial<LocationApi> = {}): LocationApi => ({
  requestForegroundPermissionsAsync: async () => ({ granted: true, canAskAgain: true }),
  getCurrentPositionAsync: async () => ({ coords: { latitude: 50.087654, longitude: 14.421234 } }),
  lowAccuracy: 2,
  ...over,
});

describe('the phone position', () => {
  it('is rounded to about a kilometre before it goes to the hub', async () => {
    expect(await readPosition(api())).toEqual({ ok: true, lat: 50.09, lon: 14.42 });
    expect(roundCoordinate(-33.8688)).toBe(-33.87);
  });

  it('asks for low accuracy only', async () => {
    let asked = -1;
    await readPosition(api({ getCurrentPositionAsync: async (o) => ((asked = o.accuracy), { coords: { latitude: 1, longitude: 2 } }) }));
    expect(asked).toBe(2);
  });

  it('explains a refusal, and a permanent one differently', async () => {
    const soft = await readPosition(api({ requestForegroundPermissionsAsync: async () => ({ granted: false, canAskAgain: true }) }));
    const hard = await readPosition(api({ requestForegroundPermissionsAsync: async () => ({ granted: false, canAskAgain: false }) }));
    expect(soft).toMatchObject({ ok: false, error: expect.stringContaining('not allowed') });
    expect(hard).toMatchObject({ ok: false, error: expect.stringContaining('system settings') });
  });

  it('turns a failure of the positioning itself into a message, not an exception', async () => {
    const r = await readPosition(api({ getCurrentPositionAsync: async () => { throw new Error('location unavailable'); } }));
    expect(r).toMatchObject({ ok: false, error: expect.stringContaining('Could not get the position') });
  });

  it('does not accept a non-number from the platform', async () => {
    const r = await readPosition(api({ getCurrentPositionAsync: async () => ({ coords: { latitude: NaN, longitude: 3 } }) }));
    expect(r.ok).toBe(false);
  });
});
