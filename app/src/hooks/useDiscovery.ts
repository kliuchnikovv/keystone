import { useEffect, useState } from 'react';
import { startDiscovery } from '../native/discovery';
import { keystoneNet } from '../native/module';
import type { NativeHub } from '../native/types';

export interface Discovery {
  /** False where the build cannot browse mDNS (web, Expo Go): the screen shows no list at all. */
  available: boolean;
  hubs: NativeHub[];
}

/** Hubs advertising `_keystone._tcp` while the component is mounted. */
export function useDiscovery(): Discovery {
  const mod = keystoneNet();
  const [hubs, setHubs] = useState<NativeHub[]>([]);
  useEffect(() => {
    if (!mod) return;
    return startDiscovery(mod, setHubs);
  }, [mod]);
  return { available: mod !== null, hubs };
}
