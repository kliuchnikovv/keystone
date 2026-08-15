import { useEffect, useRef } from 'react';
import styles from './SetupCodeField.module.css';

export interface SetupCodeFieldProps {
  value: string;
  onChange: (digitsOnly: string) => void;
  autoFocus?: boolean;
}

function formatDisplay(digits: string): string {
  const d = digits.slice(0, 11);
  const a = d.slice(0, 4);
  const b = d.slice(4, 8);
  const c = d.slice(8, 11);
  return [a, b, c].filter(Boolean).join(' — ');
}

/**
 * 11-значный setup code. Хранит значение как чистые цифры;
 * отображает `1234 — 5678 — 901` с авто-форматированием.
 */
export function SetupCodeField({ value, onChange, autoFocus }: SetupCodeFieldProps) {
  const ref = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (autoFocus) ref.current?.focus();
  }, [autoFocus]);

  const handleChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const digits = e.target.value.replace(/\D/g, '').slice(0, 11);
    onChange(digits);
  };

  const isComplete = value.length === 11;

  return (
    <label className={styles.root}>
      <input
        ref={ref}
        type="text"
        inputMode="numeric"
        autoComplete="one-time-code"
        aria-label="11-значный setup code"
        className={[styles.input, isComplete && styles.complete].filter(Boolean).join(' ')}
        value={formatDisplay(value)}
        onChange={handleChange}
        placeholder="1234 — 5678 — 901"
      />
      <span className={styles.hint}>{value.length} / 11</span>
    </label>
  );
}
