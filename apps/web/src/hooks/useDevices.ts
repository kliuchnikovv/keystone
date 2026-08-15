import { useQuery } from '@tanstack/react-query';
import { useEffect } from 'react';
import { listDevices } from '../api/devices';
import { useDevicesStore } from '../state/devicesStore';

export function useDevices() {
  const setDevices = useDevicesStore((s) => s.setDevices);
  const query = useQuery({ queryKey: ['devices'], queryFn: listDevices });

  useEffect(() => {
    if (query.data) setDevices(query.data);
  }, [query.data, setDevices]);

  return query;
}
