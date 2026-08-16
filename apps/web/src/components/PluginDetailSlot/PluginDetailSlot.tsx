import { useEffect, useRef, useState } from 'react';
import { usePluginDetail } from '../../lib/plugin-runtime';
import type { Device } from '../../api/types';

// PluginDetailSlot mounts a plugin-declared custom element for a
// device when any running plugin's ui.deviceDetail entry matches its
// type or features. The slot renders nothing when no plugin matches
// so the built-in detail markup stays visible.
export function PluginDetailSlot({ device }: { device: Device }) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [mounted, setMounted] = useState(false);

  useEffect(() => {
    let alive = true;
    const host = hostRef.current;
    if (!host) return;
    (async () => {
      const ok = await usePluginDetail(
        host,
        device.id,
        device.type ?? '',
        (device.features ?? []).map((f) => f.key),
      );
      if (alive) setMounted(ok);
    })();
    return () => { alive = false; };
  }, [device.id, device.type, device.features]);

  return (
    <div
      ref={hostRef}
      data-plugin-detail={mounted ? 'yes' : 'no'}
      style={{ display: mounted ? 'block' : 'none' }}
    />
  );
}
