import { apiFetch } from './client';

export type PluginState = 'discovered' | 'running' | 'stopped' | 'failed';

export interface PluginStatus {
  name: string;
  version: string;
  state: PluginState;
  connected: boolean;
  last_error?: string;
}

interface ListResponse {
  plugins?: PluginStatus[];
}

export async function listPlugins(): Promise<PluginStatus[]> {
  const r = await apiFetch<ListResponse>('/plugins');
  return r.plugins ?? [];
}

export function enablePlugin(name: string): Promise<PluginStatus> {
  return apiFetch<PluginStatus>(`/plugins/${encodeURIComponent(name)}/enable`, { method: 'POST' });
}

export function disablePlugin(name: string): Promise<PluginStatus> {
  return apiFetch<PluginStatus>(`/plugins/${encodeURIComponent(name)}/disable`, { method: 'POST' });
}

export function restartPlugin(name: string): Promise<PluginStatus> {
  return apiFetch<PluginStatus>(`/plugins/${encodeURIComponent(name)}/restart`, { method: 'POST' });
}

export function uninstallPlugin(name: string): Promise<unknown> {
  return apiFetch(`/plugins/${encodeURIComponent(name)}`, { method: 'DELETE' });
}

export interface InstallRequest {
  name: string;
  version?: string;
  registry: string;
  force?: boolean;
}

export function installPlugin(req: InstallRequest): Promise<{ name: string; version: string }> {
  return apiFetch('/plugins/install', {
    method: 'POST',
    body: JSON.stringify(req),
  });
}

export interface RegistryPackage {
  url: string;
  sha256: string;
  signature?: string;
}

export interface RegistryEntry {
  description?: string;
  homepage?: string;
  versions: Record<string, RegistryPackage>;
}

export interface RegistryIndex {
  plugins: Record<string, RegistryEntry>;
}

export function browseRegistry(url: string): Promise<RegistryIndex> {
  return apiFetch<RegistryIndex>(`/plugins/registry?url=${encodeURIComponent(url)}`);
}
