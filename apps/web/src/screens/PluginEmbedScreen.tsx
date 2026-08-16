import { useEffect, useRef, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { ChevronLeft } from 'lucide-react';
import styles from './PluginEmbedScreen.module.css';
import { getPlugin, type Manifest } from '../api/plugins';
import { attachEmbedBridge } from '../lib/embed-bridge';
import { API_BASE } from '../api/client';

interface EmbedBinding {
  url: string;
  title?: string;
}

function readEmbed(manifest?: Manifest): EmbedBinding | null {
  const raw = (manifest as any)?.UI?.embed ?? (manifest as any)?.ui?.embed;
  if (!raw || typeof raw.url !== 'string') return null;
  // A hostile manifest could try to escape ui/ with "../evil.html".
  // The backend refuses that on the wire, but as belt-and-braces here
  // we drop anything that starts with ".." or contains a scheme.
  if (raw.url.includes('..')) return null;
  if (/^[a-z]+:/i.test(raw.url)) return null;
  return { url: raw.url, title: raw.title };
}

export function PluginEmbedScreen() {
  const nav = useNavigate();
  const { name = '' } = useParams<{ name: string }>();
  const [binding, setBinding] = useState<EmbedBinding | null>(null);
  const [error, setError] = useState<string | null>(null);
  const iframeRef = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    let alive = true;
    (async () => {
      try {
        const p = await getPlugin(name);
        if (!alive) return;
        const b = readEmbed(p.manifest);
        if (!b) {
          setError('Плагин не объявил ui.embed');
          return;
        }
        setBinding(b);
      } catch (e) {
        if (alive) setError(String(e));
      }
    })();
    return () => { alive = false; };
  }, [name]);

  // Attach the postMessage bridge as soon as the iframe element mounts.
  // Detach on unmount so the plugin's subscriptions are released.
  useEffect(() => {
    const el = iframeRef.current;
    if (!el || !binding) return;
    const detach = attachEmbedBridge(el);
    return detach;
  }, [binding]);

  const src = binding
    ? `${API_BASE}/plugins/${encodeURIComponent(name)}/ui/${binding.url}`
    : '';

  return (
    <div className={styles.root}>
      <header className={styles.header}>
        <button className={styles.back} onClick={() => nav('/plugins')} aria-label="Назад">
          <ChevronLeft size={20} />
        </button>
        <h1>{binding?.title ?? name}</h1>
      </header>
      {error && <p className={styles.error}>{error}</p>}
      {binding && (
        <iframe
          ref={iframeRef}
          className={styles.frame}
          src={src}
          // Same-origin: parent and iframe are served from the same
          // keystone host, so postMessage's origin field verifies
          // strictly. allow-same-origin is required for
          // window.location.origin to be non-null inside the iframe,
          // which the embed bootstrap needs for the "hello" pin.
          sandbox="allow-scripts allow-same-origin"
          title={binding.title ?? name}
        />
      )}
    </div>
  );
}
