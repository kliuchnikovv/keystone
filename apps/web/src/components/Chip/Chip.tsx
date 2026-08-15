import type { ReactNode } from 'react';
import styles from './Chip.module.css';

export type ChipTone = 'neutral' | 'accent' | 'success' | 'warn' | 'danger' | 'muted';

export interface ChipProps {
  tone?: ChipTone;
  size?: 'sm' | 'md';
  selected?: boolean;
  onClick?: () => void;
  children: ReactNode;
}

export function Chip({ tone = 'neutral', size = 'md', selected, onClick, children }: ChipProps) {
  const Tag = onClick ? 'button' : 'span';
  return (
    <Tag
      type={onClick ? 'button' : undefined}
      className={[
        styles.root,
        styles[`tone-${tone}`],
        styles[`size-${size}`],
        selected && styles.selected,
        onClick && styles.clickable,
      ]
        .filter(Boolean)
        .join(' ')}
      onClick={onClick}
    >
      {children}
    </Tag>
  );
}
