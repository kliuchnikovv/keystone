import { useEffect, useState } from 'react';
import { Loader2, ScanLine, ExternalLink, CheckCircle2, AlertCircle } from 'lucide-react';
import styles from './ConfigFlow.module.css';
import { SchemaForm, type JSONSchema } from '../SchemaForm/SchemaForm';
import { configFlow, type ConfigFlowStep } from '../../api/plugins';

// Layer 2 wizard: walk a plugin through its declared config flow.
// State is client-driven — each step's next id + a data payload is
// what advances the wizard. The plugin owns state; the core is a
// proxy; this component renders the current step.

export interface ConfigFlowProps {
  plugin: string;
  onClose: () => void;
  onDone?: () => void;
}

export function ConfigFlow({ plugin, onClose, onDone }: ConfigFlowProps) {
  const [step, setStep] = useState<ConfigFlowStep | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Initial fetch: send {step: "init"} to start the flow.
  useEffect(() => {
    let live = true;
    (async () => {
      try {
        const s = await configFlow(plugin, { step: 'init' });
        if (live) setStep(s);
      } catch (e) {
        if (live) setError(String(e));
      }
    })();
    return () => { live = false; };
  }, [plugin]);

  const advance = async (next: string, data: Record<string, unknown> = {}) => {
    setBusy(true);
    setError(null);
    try {
      const s = await configFlow(plugin, { step: next, data });
      setStep(s);
      if (s.type === 'complete' && onDone) onDone();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  };

  if (error && !step) return <div className={styles.panel}><ErrorView message={error} onClose={onClose} /></div>;
  if (!step) return <div className={styles.panel}><Loader2 className={styles.spin} /></div>;

  return (
    <div className={styles.panel}>
      {step.title && <h3 className={styles.title}>{step.title}</h3>}
      {step.body && <p className={styles.body}>{step.body}</p>}
      {error && <p className={styles.error}>{error}</p>}
      <StepBody step={step} busy={busy} onAdvance={advance} onClose={onClose} />
    </div>
  );
}

function StepBody({
  step,
  busy,
  onAdvance,
  onClose,
}: {
  step: ConfigFlowStep;
  busy: boolean;
  onAdvance: (next: string, data?: Record<string, unknown>) => void;
  onClose: () => void;
}) {
  switch (step.type) {
    case 'info':
      // Read-only page with a "Дальше" button that advances to next.
      return (
        <button
          type="button"
          className={styles.primaryBtn}
          disabled={busy}
          onClick={() => onAdvance(step.next ?? 'init')}
        >
          Дальше
        </button>
      );

    case 'form': {
      let schema: JSONSchema | null = null;
      try { schema = step.schema ? (JSON.parse(step.schema) as JSONSchema) : null; } catch { schema = null; }
      if (!schema) return <p className={styles.error}>Не удалось разобрать схему шага.</p>;
      return (
        <SchemaForm
          schema={schema}
          value={{}}
          submitting={busy}
          submitLabel="Дальше"
          onSubmit={(v) => onAdvance(step.next ?? 'init', v)}
        />
      );
    }

    case 'oauth':
      return (
        <div className={styles.actions}>
          <p className={styles.description}>Откройте OAuth-провайдера в новом окне и вернитесь сюда после подтверждения.</p>
          {step.authUrl && (
            <a className={styles.linkBtn} href={step.authUrl} target="_blank" rel="noreferrer">
              <ExternalLink size={14} /> {step.provider ?? 'Открыть'}
            </a>
          )}
          {/* MVP: user pastes the code manually. A real deployment
              would run a callback listener on RedirectURI. */}
          <label className={styles.field}>
            <span>Код авторизации</span>
            <CodeInput onSubmit={(code) => onAdvance(step.next ?? 'init', { code })} busy={busy} />
          </label>
        </div>
      );

    case 'qr-scan':
      // MVP: paste the decoded string; a real QR-scanner component
      // (getUserMedia + jsQR) replaces the input in Phase G+.
      return (
        <div className={styles.actions}>
          <p className={styles.description}>
            <ScanLine size={14} /> Отсканируйте QR-код и введите содержимое ниже.
            {step.qrHint && ` Формат: ${step.qrHint}.`}
          </p>
          <CodeInput onSubmit={(qr) => onAdvance(step.next ?? 'init', { qr })} busy={busy} placeholder="MT:…" />
        </div>
      );

    case 'progress':
      return <ProgressView progress={step.progress ?? 0} message={step.message} />;

    case 'confirm':
      return (
        <div className={styles.actions}>
          <button
            type="button"
            className={styles.primaryBtn}
            disabled={busy}
            onClick={() => onAdvance(step.next ?? 'init', { accepted: true })}
          >
            Продолжить
          </button>
          {step.cancel && (
            <button
              type="button"
              className={styles.ghostBtn}
              disabled={busy}
              onClick={() => onAdvance(step.cancel!, { accepted: false })}
            >
              Отмена
            </button>
          )}
        </div>
      );

    case 'pick-device':
      return (
        <PickDevice
          field={step.field ?? 'ref'}
          options={step.options ?? []}
          busy={busy}
          onSubmit={(picked) => onAdvance(step.next ?? 'init', picked)}
        />
      );

    case 'manual-action':
      return (
        <div className={styles.actions}>
          {step.instruction && <p className={styles.description}>{step.instruction}</p>}
          <button
            type="button"
            className={styles.primaryBtn}
            disabled={busy}
            onClick={() => onAdvance(step.next ?? 'init')}
          >
            Готово
          </button>
        </div>
      );

    case 'error':
      return <ErrorView message={step.message ?? 'Ошибка'} retry={step.retry} onRetry={onAdvance} onClose={onClose} />;

    case 'complete':
      return (
        <div className={styles.actions}>
          <p className={styles.done}>
            <CheckCircle2 size={16} /> {step.message ?? 'Готово.'}
          </p>
          <button type="button" className={styles.primaryBtn} onClick={onClose}>
            Закрыть
          </button>
        </div>
      );

    default:
      return <p className={styles.error}>Неизвестный тип шага: {step.type}</p>;
  }
}

function CodeInput({
  onSubmit,
  busy,
  placeholder,
}: {
  onSubmit: (v: string) => void;
  busy?: boolean;
  placeholder?: string;
}) {
  const [v, setV] = useState('');
  return (
    <div className={styles.row}>
      <input
        className={styles.input}
        value={v}
        placeholder={placeholder}
        onChange={(e) => setV(e.target.value)}
        disabled={busy}
      />
      <button
        type="button"
        className={styles.primaryBtn}
        disabled={busy || !v}
        onClick={() => onSubmit(v)}
      >
        Дальше
      </button>
    </div>
  );
}

function ProgressView({ progress, message }: { progress: number; message?: string }) {
  const pct = Math.max(0, Math.min(1, progress)) * 100;
  return (
    <div className={styles.actions}>
      <div className={styles.progressTrack}>
        <div className={styles.progressFill} style={{ width: `${pct}%` }} />
      </div>
      {message && <p className={styles.description}>{message}</p>}
    </div>
  );
}

function PickDevice({
  field,
  options,
  busy,
  onSubmit,
}: {
  field: string;
  options: Array<{ label: string; value: string; description?: string }>;
  busy: boolean;
  onSubmit: (payload: Record<string, unknown>) => void;
}) {
  const [picked, setPicked] = useState<Record<string, boolean>>({});
  const toggle = (v: string) => setPicked({ ...picked, [v]: !picked[v] });
  const selected = Object.keys(picked).filter((k) => picked[k]);
  return (
    <div className={styles.actions}>
      <ul className={styles.list}>
        {options.map((o) => (
          <li key={o.value} className={styles.deviceRow}>
            <label>
              <input type="checkbox" checked={!!picked[o.value]} onChange={() => toggle(o.value)} />
              <span>{o.label}</span>
              {o.description && <em className={styles.description}>{o.description}</em>}
            </label>
          </li>
        ))}
      </ul>
      <button
        type="button"
        className={styles.primaryBtn}
        disabled={busy || selected.length === 0}
        onClick={() => onSubmit({ [field]: selected })}
      >
        Добавить ({selected.length})
      </button>
    </div>
  );
}

function ErrorView({
  message,
  retry,
  onRetry,
  onClose,
}: {
  message: string;
  retry?: string;
  onRetry?: (step: string) => void;
  onClose: () => void;
}) {
  return (
    <div className={styles.actions}>
      <p className={styles.error}>
        <AlertCircle size={16} /> {message}
      </p>
      {retry && onRetry && (
        <button type="button" className={styles.primaryBtn} onClick={() => onRetry(retry)}>
          Повторить
        </button>
      )}
      <button type="button" className={styles.ghostBtn} onClick={onClose}>
        Закрыть
      </button>
    </div>
  );
}
