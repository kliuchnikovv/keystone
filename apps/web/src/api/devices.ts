import { apiFetch } from './client';
import {
  toDevice,
  type ActionRequest,
  type Device,
  type RawDevice,
  type WriteStateRequest,
} from './types';

interface ListResponse {
  devices?: RawDevice[];
}

export async function listDevices(): Promise<Device[]> {
  const r = await apiFetch<ListResponse>('/devices');
  return (r.devices ?? []).map(toDevice);
}

export async function getDevice(id: string): Promise<Device> {
  const raw = await apiFetch<RawDevice>(`/devices/${encodeURIComponent(id)}`);
  return toDevice(raw);
}

export function invokeAction(id: string, req: ActionRequest): Promise<unknown> {
  return apiFetch(`/devices/${encodeURIComponent(id)}/actions`, {
    method: 'POST',
    body: JSON.stringify(req),
  });
}

export function writeState(id: string, req: WriteStateRequest): Promise<unknown> {
  return apiFetch(`/devices/${encodeURIComponent(id)}/state`, {
    method: 'POST',
    body: JSON.stringify(req),
  });
}

// deleteDevice — DELETE /devices/{id}. Best-effort on backend: 200 всегда,
// warnings в теле если адаптер/диск отвалились. UI обрабатывает как success.
export interface DeleteDeviceResponse {
  ok: boolean;
  adapter_warning?: string;
  persist_warning?: string;
}

export function deleteDevice(id: string): Promise<DeleteDeviceResponse> {
  return apiFetch<DeleteDeviceResponse>(`/devices/${encodeURIComponent(id)}`, {
    method: 'DELETE',
  });
}
