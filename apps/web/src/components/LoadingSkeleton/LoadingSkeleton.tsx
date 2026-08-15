import styles from './LoadingSkeleton.module.css';

export function LoadingSkeleton() {
  return (
    <div className={styles.root} aria-hidden>
      <div className={styles.section}>
        <div className={[styles.pill, styles.headerPill].join(' ')} />
        <div className={[styles.tile, styles.tall].join(' ')} />
      </div>
      <div className={styles.section}>
        <div className={[styles.pill, styles.headerPill].join(' ')} />
        <div className={styles.row}>
          <div className={styles.tile} />
          <div className={styles.tile} />
        </div>
      </div>
      <div className={styles.section}>
        <div className={[styles.pill, styles.headerPill].join(' ')} />
        <div className={styles.row}>
          <div className={[styles.tile, styles.compact].join(' ')} />
          <div className={[styles.tile, styles.compact].join(' ')} />
          <div className={[styles.tile, styles.compact].join(' ')} />
        </div>
      </div>
    </div>
  );
}
