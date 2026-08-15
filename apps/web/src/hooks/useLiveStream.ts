import { useEffect } from 'react';
import { openStream } from '../api/stream';
import { useDevicesStore, liveKey } from '../state/devicesStore';
import { useConnectionStore } from '../state/connectionStore';
import { useEventsStore } from '../state/eventsStore';

export function useLiveStream() {
  const setLive = useDevicesStore((s) => s.setLive);
  const setStatus = useConnectionStore((s) => s.setStatus);
  const pushEvent = useEventsStore((s) => s.push);

  useEffect(() => {
    const handle = openStream(
      (msg) => {
        if (msg.type === 'state') {
          const p = msg.payload as {
            DeviceID: string;
            Feature: string;
            Key: string;
            Value: unknown;
          };
          const prev = useDevicesStore.getState().liveState.get(liveKey(p.DeviceID, p.Feature, p.Key));
          setLive(p.DeviceID, p.Feature, p.Key, p.Value);
          if (prev !== p.Value) {
            pushEvent({
              kind: 'state',
              deviceId: p.DeviceID,
              feature: p.Feature,
              label: `${p.Feature} · ${describeChange(prev, p.Value)}`,
            });
          }
        } else if (msg.type === 'event') {
          const p = msg.payload as {
            DeviceID: string;
            Feature: string;
            Name: string;
          };
          pushEvent({
            kind: 'event',
            deviceId: p.DeviceID,
            feature: p.Feature,
            label: p.Name,
          });
        }
      },
      (status) => setStatus(status),
    );
    return () => handle.close();
  }, [setLive, setStatus, pushEvent]);
}

function describeChange(prev: unknown, next: unknown): string {
  if (typeof prev === 'boolean' || typeof next === 'boolean') {
    return next ? 'включено' : 'выключено';
  }
  if (typeof prev === 'number' && typeof next === 'number') {
    return `${prev} → ${next}`;
  }
  return String(next);
}
