import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { ChevronLeft, Download, Play, Square, RefreshCw, Trash2, Search } from 'lucide-react';
import styles from './PluginsScreen.module.css';
import { Button } from '../components/Button/Button';
import {
  browseRegistry,
  disablePlugin,
  enablePlugin,
  installPlugin,
  listPlugins,
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
