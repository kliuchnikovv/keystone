// The phone's presence: watching the home zone and telling the hub about it.
//
// The watching happens in the NATIVE code (modules/keystone-net), not here:
// iOS and Android both kill the app in the background, but their system's
// geofence service still wakes the app when the phone crosses the zone. This
// hook only starts the watcher and forwards what it says to the hub.
//
// The hub is told through `presence.report {name, home}`. The phone is a
// virtual entity there (see `docs/automations.md` §5.2), so a rule watches it
// exactly as it watches a motion sensor.

import { useCallback, useEffect, useRef } from 'react';
import { keystoneNet } from '../native/module';
import type { NativePresenceEvent } from '../native/types';
import { PRESENCE_PHONE_NAME, presenceReportArgs } from '../model/presence';

/** How large the watched zone is, in metres. */
export const HOME_ZONE_RADIUS_M = 150;

/**
 * Start watching the home zone and report every change to the hub.
 *
 * The zone is the hub's own position (the hub knows where it is, for sunrise
 * and sunset), so the person does not have to say where home is twice.
 *
 * `report` is called with the new state; it is the app's job to tell the hub.
 * Returns a stop function.
 */
export function useGeofence(
  hubPosition: { lat: number; lon: number } | null,
  report: (home: boolean) => void,
  onError?: (message: string) => void,
): void {
  const net = keystoneNet();
  // The report function changes on every render; keep the last one so the
  // watcher is not torn down and set up again for nothing.
  const reportRef = useRef(report);
  reportRef.current = report;
  const errorRef = useRef(onError);
  errorRef.current = onError;

  const onPresence = useCallback((e: NativePresenceEvent) => {
    if (e.error) {
      errorRef.current?.(e.error);
      return;
    }
    if (typeof e.home === 'boolean') reportRef.current(e.home);
  }, []);

  useEffect(() => {
    const module = keystoneNet();
    if (!module || !hubPosition) return;
    const sub = module.addListener('onPresence', onPresence);
    module.startGeofence(hubPosition.lat, hubPosition.lon, HOME_ZONE_RADIUS_M);
    return () => {
      sub.remove();
      module.stopGeofence();
    };
  }, [hubPosition?.lat, hubPosition?.lon, onPresence]);

  // The zone is watched even when the app is in the background; the native
  // service does that part. Nothing to do here when the module is missing
  // (web build, Expo Go).
  void net;
}

/** The arguments of the `presence.report` request. */
export function presenceRequest(home: boolean): Record<string, unknown> {
  return presenceReportArgs(PRESENCE_PHONE_NAME, home);
}
