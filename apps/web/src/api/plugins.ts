import { apiFetch } from './client';

export type PluginState = 'discovered' | 'running' | 'stopped' | 'failed';

export interface ManifestConfig {
  Schema?: string;
}

export interface ManifestSpec {
  Config?: ManifestConfig | null;
}

export interface Manifest {
  Metadata?: { Name?: string; Description?: string };
  Spec?: ManifestSpec;
}

export interface PluginStatus {
  name: string;
  version: string;
  state: PluginState;
  connected: boolean;
  last_error?: string;
  manifest?: Manifest;
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

export function getPlugin(name: string): Promise<PluginStatus> {
  return apiFetch<PluginStatus>(`/plugins/${encodeURIComponent(name)}`);
}

export function getPluginConfig(name: string): Promise<Record<string, unknown>> {
  return apiFetch<Record<string, unknown>>(`/plugins/${encodeURIComponent(name)}/config`);
}

export function putPluginConfig(name: string, value: unknown): Promise<unknown> {
  return apiFetch(`/plugins/${encodeURIComponent(name)}/config`, {
    method: 'PUT',
    body: JSON.stringify(value),
  });
}

// Layer 2 — Config Flow wizard. State is client-driven: on the first
// step the client sends {step: "init", data: {}}; subsequent calls
// echo whatever ConfigFlowStep.next the plugin returned and carry
// the user's submitted data.
export type ConfigFlowStepType =
  | 'info'
  | 'form'
  | 'oauth'
  | 'qr-scan'
  | 'progress'
  | 'confirm'
  | 'pick-device'
  | 'manual-action'
  | 'error'
  | 'complete';

export interface ConfigFlowOption {
  label: string;
  value: string;
  description?: string;
}

export interface ConfigFlowStep {
  type: ConfigFlowStepType;
  id?: string;
  next?: string;
  title?: string;
  body?: string;
  schema?: string;
  authUrl?: string;
  redirectUri?: string;
  provider?: string;
  qrHint?: string;
  progress?: number;
  cancel?: string;
  field?: string;
  options?: ConfigFlowOption[];
  instruction?: string;
  message?: string;
  retry?: string;
}

export interface ConfigFlowRequest {
  step: string;
  data?: Record<string, unknown>;
}

export function configFlow(name: string, req: ConfigFlowRequest): Promise<ConfigFlowStep> {
  return apiFetch<ConfigFlowStep>(`/plugins/${encodeURIComponent(name)}/flow`, {
    method: 'POST',
    body: JSON.stringify(req),
  });
}
