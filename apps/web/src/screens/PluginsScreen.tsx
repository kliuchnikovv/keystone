import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { ChevronLeft, Download, ExternalLink, Play, Square, RefreshCw, Settings, Trash2, Search, Wand2 } from 'lucide-react';
import styles from './PluginsScreen.module.css';
import { Button } from '../components/Button/Button';
import { ConfigFlow } from '../components/ConfigFlow/ConfigFlow';
import { SchemaForm, type JSONSchema } from '../components/SchemaForm/SchemaForm';
import {
  browseRegistry,
  disablePlugin,
  enablePlugin,
  getPluginConfig,
  installPlugin,
  listPlugins,
  putPluginConfig,
  restartPlugin,
  uninstallPlugin,
  type PluginState,
  type PluginStatus,
  type RegistryEntry,
} from '../api/plugins';

// The screen is one column split into "installed" and "browse". Kept
// deliberately dense — an operator screen, not a device screen — so
// every plugin's state, actions and metadata are visible without a
// second click.
export function PluginsScreen() {
  const nav = useNavigate();
  const qc = useQueryClient();

  const installed = useQuery({
    queryKey: ['plugins'],
    queryFn: listPlugins,
    refetchInterval: 5000,
  });

  const [registryUrl, setRegistryUrl] = useState('');
  const [browseError, setBrowseError] = useState<string | null>(null);
  const browse = useMutation({
    mutationFn: (url: string) => browseRegistry(url),
    onError: (e: Error) => setBrowseError(e.message),
    onSuccess: () => setBrowseError(null),
  });

  const refresh = () => qc.invalidateQueries({ queryKey: ['plugins'] });

  const enableMut = useMutation({ mutationFn: enablePlugin, onSuccess: refresh });
  const disableMut = useMutation({ mutationFn: disablePlugin, onSuccess: refresh });
  const restartMut = useMutation({ mutationFn: restartPlugin, onSuccess: refresh });
  const uninstallMut = useMutation({ mutationFn: uninstallPlugin, onSuccess: refresh });

  const [configOpen, setConfigOpen] = useState<string | null>(null);
  const [flowOpen, setFlowOpen] = useState<string | null>(null);

  const installMut = useMutation({
    mutationFn: installPlugin,
    onSuccess: () => {
      refresh();
      // Re-fetch the browse index so the version list shows the new
      // "installed" indicator without a manual re-search.
      if (registryUrl) browse.mutate(registryUrl);
    },
  });

  return (
    <div className={styles.root}>
      <header className={styles.header}>
        <button className={styles.back} onClick={() => nav('/')} aria-label="Назад">
          <ChevronLeft size={20} />
        </button>
        <h1>Плагины</h1>
      </header>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Установлены</h2>
        {installed.isLoading && <p className={styles.muted}>Загрузка…</p>}
        {installed.error && <p className={styles.error}>Не удалось получить список: {String(installed.error)}</p>}
        {installed.data?.length === 0 && (
          <p className={styles.muted}>Плагины ещё не установлены — начните с поиска по реестру.</p>
        )}
        <ul className={styles.list}>
          {installed.data?.map((p) => (
            <li key={p.name} className={styles.installedRow}>
              <div className={styles.rowMain}>
                <span className={styles.name}>{p.name}</span>
                <span className={styles.version}>{p.version}</span>
                <StateBadge state={p.state} connected={p.connected} />
              </div>
              {p.last_error && <p className={styles.rowError}>{p.last_error}</p>}
              {configOpen === p.name && (
                <ConfigPanel
                  name={p.name}
                  schemaSource={p.manifest?.Spec?.Config?.Schema}
                  onClose={() => setConfigOpen(null)}
                />
              )}
              {flowOpen === p.name && (
                <ConfigFlow
                  plugin={p.name}
                  onClose={() => setFlowOpen(null)}
                />
              )}
              <div className={styles.rowActions}>
                {p.state !== 'running' ? (
                  <Button
                    size="sm"
                    variant="primary"
                    onClick={() => enableMut.mutate(p.name)}
                    disabled={enableMut.isPending || p.state === 'failed'}
                  >
                    <Play size={14} /> Включить
                  </Button>
                ) : (
                  <Button
                    size="sm"
                    variant="ghost"
                    onClick={() => disableMut.mutate(p.name)}
                    disabled={disableMut.isPending}
                  >
                    <Square size={14} /> Выключить
                  </Button>
                )}
                <Button
                  size="sm"
                  variant="ghost"
                  onClick={() => restartMut.mutate(p.name)}
                  disabled={restartMut.isPending || p.state !== 'running'}
                  aria-label="Перезапустить"
                >
                  <RefreshCw size={14} />
                </Button>
                {p.manifest?.Spec?.Config?.Schema && (
                  <Button
                    size="sm"
                    variant="ghost"
                    onClick={() => setConfigOpen(configOpen === p.name ? null : p.name)}
                    aria-label="Настройки"
                  >
                    <Settings size={14} />
                  </Button>
                )}
                {p.state === 'running' && (
                  <Button
                    size="sm"
                    variant="ghost"
                    onClick={() => setFlowOpen(flowOpen === p.name ? null : p.name)}
                    aria-label="Мастер настройки"
                  >
                    <Wand2 size={14} />
                  </Button>
                )}
                {p.manifest && ((p.manifest as any).UI?.embed || (p.manifest as any).ui?.embed) && (
                  <Button
                    size="sm"
                    variant="ghost"
                    onClick={() => nav(`/plugins/${encodeURIComponent(p.name)}/embed`)}
                    aria-label="Открыть UI плагина"
                  >
                    <ExternalLink size={14} />
                  </Button>
                )}
                <Button
                  size="sm"
                  variant="ghost"
                  onClick={() => {
                    if (window.confirm(`Удалить плагин ${p.name}?`)) uninstallMut.mutate(p.name);
                  }}
                  disabled={uninstallMut.isPending}
                  aria-label="Удалить"
                >
                  <Trash2 size={14} />
                </Button>
              </div>
            </li>
          ))}
        </ul>
      </section>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Каталог</h2>
        <form
          className={styles.browseForm}
          onSubmit={(e) => {
            e.preventDefault();
            if (registryUrl) browse.mutate(registryUrl);
          }}
        >
          <input
            className={styles.input}
            type="url"
            placeholder="https://plugins.keystone.io"
            value={registryUrl}
            onChange={(e) => setRegistryUrl(e.target.value)}
            required
          />
          <Button type="submit" size="sm" disabled={browse.isPending}>
            <Search size={14} /> Открыть
          </Button>
        </form>
        {browseError && <p className={styles.error}>{browseError}</p>}
        {browse.data && (
          <ul className={styles.list}>
            {Object.entries(browse.data.plugins).map(([name, entry]) => (
              <BrowseRow
                key={name}
                name={name}
                entry={entry}
                installed={installed.data ?? []}
                busy={installMut.isPending}
                onInstall={(version) =>
                  installMut.mutate({ name, version, registry: registryUrl, force: false })
                }
              />
            ))}
          </ul>
        )}
      </section>
    </div>
  );
}

function BrowseRow({
  name,
  entry,
  installed,
  busy,
  onInstall,
}: {
  name: string;
  entry: RegistryEntry;
  installed: PluginStatus[];
  busy: boolean;
  onInstall: (version: string) => void;
}) {
  const versions = Object.keys(entry.versions).sort().reverse();
  const [chosen, setChosen] = useState(versions[0] ?? 'latest');
  const installedVersion = installed.find((p) => p.name === name)?.version;

  return (
    <li className={styles.browseRow}>
      <div className={styles.rowMain}>
        <span className={styles.name}>{name}</span>
        {installedVersion && <span className={styles.installedBadge}>установлен: {installedVersion}</span>}
      </div>
      {entry.description && <p className={styles.description}>{entry.description}</p>}
      <div className={styles.rowActions}>
        <select
          className={styles.versionSelect}
          value={chosen}
          onChange={(e) => setChosen(e.target.value)}
          aria-label="Версия"
        >
          {versions.map((v) => (
            <option key={v} value={v}>
              {v}
            </option>
          ))}
        </select>
        <Button size="sm" onClick={() => onInstall(chosen)} disabled={busy}>
          <Download size={14} /> Установить
        </Button>
      </div>
    </li>
  );
}

function ConfigPanel({
  name,
  schemaSource,
  onClose,
}: {
  name: string;
  schemaSource?: string;
  onClose: () => void;
}) {
  const qc = useQueryClient();
  const config = useQuery({
    queryKey: ['plugin-config', name],
    queryFn: () => getPluginConfig(name),
  });
  const putMut = useMutation({
    mutationFn: (value: unknown) => putPluginConfig(name, value),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['plugin-config', name] });
      onClose();
    },
  });

  const schema: JSONSchema | null = (() => {
    if (!schemaSource) return null;
    try {
      return JSON.parse(schemaSource) as JSONSchema;
    } catch {
      return null;
    }
  })();

  if (!schema) {
    return <p className={styles.rowError}>Не удалось разобрать схему конфигурации.</p>;
  }
  if (config.isLoading) return <p className={styles.muted}>Загружаем настройки…</p>;
  if (config.error) return <p className={styles.rowError}>Не удалось загрузить конфиг: {String(config.error)}</p>;

  return (
    <div className={styles.configPanel}>
      <SchemaForm
        schema={schema}
        value={(config.data as Record<string, unknown>) ?? {}}
        onSubmit={(v) => putMut.mutate(v)}
        submitting={putMut.isPending}
      />
      {putMut.error && <p className={styles.rowError}>{String(putMut.error)}</p>}
    </div>
  );
}

function StateBadge({ state, connected }: { state: PluginState; connected: boolean }) {
  const label =
    state === 'running' ? (connected ? 'работает' : 'не отвечает') :
      state === 'stopped' ? 'выключен' :
        state === 'failed' ? 'ошибка' : 'найден';
  const cls = state === 'running' && connected ? styles.badgeOk
    : state === 'failed' ? styles.badgeFail
      : styles.badgeMuted;
  return <span className={`${styles.badge} ${cls}`}>{label}</span>;
}
