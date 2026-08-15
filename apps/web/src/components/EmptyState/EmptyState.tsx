import type { ReactNode } from 'react';
import styles from './EmptyState.module.css';

export interface EmptyStateProps {
  emoji?: string;
  title: string;
  body?: string;
  primary?: ReactNode;
  secondary?: ReactNode;
}

export function EmptyState({ emoji = '🏠', title, body, primary, secondary }: EmptyStateProps) {
  return (
    <div className={styles.root}>
      <div className={styles.halo} aria-hidden>
        <span className={styles.emoji}>{emoji}</span>
      </div>
      <h2 className={styles.title}>{title}</h2>
      {body && <p className={styles.body}>{body}</p>}
      {primary && <div className={styles.primary}>{primary}</div>}
      {secondary && <div className={styles.secondary}>{secondary}</div>}
    </div>
  );
}
