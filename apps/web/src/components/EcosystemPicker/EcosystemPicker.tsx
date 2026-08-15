import { ChevronRight } from 'lucide-react';
import styles from './EcosystemPicker.module.css';
import { ECOSYSTEMS } from '../../lib/ecosystem-guides';
import type { Ecosystem } from '../../state/commissioningStore';

export interface EcosystemPickerProps {
  onPick: (e: Ecosystem) => void;
  onOther?: () => void;
}

export function EcosystemPicker({ onPick, onOther }: EcosystemPickerProps) {
  return (
    <div className={styles.root}>
      <div className={styles.list}>
        {ECOSYSTEMS.map((e) => (
          <button
            key={e.id}
            type="button"
            className={styles.card}
            onClick={() => onPick(e.id)}
          >
            <span className={styles.glyph} aria-hidden>
              {e.glyph}
            </span>
            <span className={styles.label}>{e.label}</span>
            <ChevronRight size={18} className={styles.chev} />
          </button>
        ))}
      </div>

      <div className={styles.divider}>
        <span>или</span>
      </div>

      <div className={styles.other}>
        <p className={styles.otherHint}>Устройство ещё не настроено?</p>
        <button type="button" className={styles.otherBtn} onClick={onOther}>
          Другой способ
        </button>
      </div>
    </div>
  );
}
