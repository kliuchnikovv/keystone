import styles from './Toggle.module.css';

export interface ToggleProps {
  checked: boolean;
  pending?: boolean;
  disabled?: boolean;
  size?: 'md' | 'lg';
  onChange?: (v: boolean) => void;
  'aria-label'?: string;
}

export function Toggle({
  checked,
  pending = false,
  disabled = false,
  size = 'md',
  onChange,
  'aria-label': ariaLabel,
}: ToggleProps) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={ariaLabel}
      disabled={disabled}
      className={[
        styles.root,
        styles[`size-${size}`],
        checked && styles.on,
        pending && styles.pending,
        disabled && styles.disabled,
      ]
        .filter(Boolean)
        .join(' ')}
      onClick={() => onChange?.(!checked)}
    >
      <span className={styles.thumb} />
    </button>
  );
}
