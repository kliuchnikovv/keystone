import type { ReactNode } from 'react';
import styles from './SectionHeader.module.css';

export interface SectionHeaderProps {
  title: string;
  count?: number;
  right?: ReactNode;
}

export function SectionHeader({ title, count, right }: SectionHeaderProps) {
  return (
    <div className={styles.root}>
      <span className={styles.title}>{title}</span>
      <span className={styles.right}>
        {right}
        {typeof count === 'number' && <span className={styles.count}>{count}</span>}
      </span>
    </div>
  );
}
