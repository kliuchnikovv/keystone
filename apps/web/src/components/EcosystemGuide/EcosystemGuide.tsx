import styles from './EcosystemGuide.module.css';
import type { EcosystemMeta } from '../../lib/ecosystem-guides';

export interface EcosystemGuideProps {
  eco: EcosystemMeta;
}

export function EcosystemGuide({ eco }: EcosystemGuideProps) {
  return (
    <div className={styles.root}>
      <header className={styles.head}>
        <span className={styles.glyph} aria-hidden>
          {eco.glyph}
        </span>
        <div>
          <p className={styles.eyebrow}>Инструкция для</p>
          <h2 className={styles.title}>{eco.label}</h2>
        </div>
      </header>
      <ol className={styles.steps}>
        {eco.steps.map((s, i) => (
          <li key={i} className={styles.step}>
            <span className={styles.num}>{i + 1}</span>
            <span className={styles.text}>{s}</span>
          </li>
        ))}
      </ol>
    </div>
  );
}
