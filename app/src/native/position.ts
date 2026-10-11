// The phone's rough position, for the hub's location (what makes "at sunset" mean something).

export type PositionResult = { ok: true; lat: number; lon: number } | { ok: false; error: string };

/** The part of expo-location this needs, so the logic can be tested without a native runtime. */
export interface LocationApi {
  requestForegroundPermissionsAsync(): Promise<{ granted: boolean; canAskAgain: boolean }>;
  getCurrentPositionAsync(options: { accuracy: number }): Promise<{ coords: { latitude: number; longitude: number } }>;
  /** expo-location's LocationAccuracy.Low (about a kilometre): the sun does not care about more. */
  readonly lowAccuracy: number;
}

/** About 1 km: a precise address is never sent to the hub, and sunrise moves by seconds over that distance. */
export const roundCoordinate = (v: number) => Math.round(v * 100) / 100;

export async function readPosition(api: LocationApi): Promise<PositionResult> {
  try {
    const perm = await api.requestForegroundPermissionsAsync();
    if (!perm.granted) {
      return {
        ok: false,
        error: perm.canAskAgain
          ? 'Location access was not allowed. Type the coordinates instead.'
          : 'Location access is off for this app. Allow it in the system settings or type the coordinates.',
      };
    }
    const p = await api.getCurrentPositionAsync({ accuracy: api.lowAccuracy });
    const lat = roundCoordinate(p.coords.latitude);
    const lon = roundCoordinate(p.coords.longitude);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return { ok: false, error: 'The phone did not report a position.' };
    return { ok: true, lat, lon };
  } catch {
    return { ok: false, error: 'Could not get the position (is location turned on for the phone?). Type the coordinates instead.' };
  }
}

/** The phone's position through expo-location (loaded on demand: the web build and tests never pay for it). */
export async function locatePhone(): Promise<PositionResult> {
  try {
    const mod = await import('expo-location');
    return await readPosition({
      requestForegroundPermissionsAsync: mod.requestForegroundPermissionsAsync,
      getCurrentPositionAsync: mod.getCurrentPositionAsync,
      lowAccuracy: mod.LocationAccuracy.Low,
    });
  } catch {
    return { ok: false, error: 'This build cannot read the phone’s location. Type the coordinates instead.' };
  }
}
