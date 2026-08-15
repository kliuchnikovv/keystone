import { Check, Loader2 } from 'lucide-react';
import styles from './CommissioningStep.module.css';

export type StepStatus = 'idle' | 'active' | 'done';

export interface CommissioningStepProps {
  status: StepStatus;
  title: string;
  detail?: string;
}

export function CommissioningStep({ status, title, detail }: CommissioningStepProps) {
  return (
    <div className={[styles.root, styles[`status-${status}`]].join(' ')}>
      <div className={styles.icon}>
        {status === 'done' ? (
          <Check size={16} />
        ) : status === 'active' ? (
          <Loader2 size={18} className={styles.spin} />
        ) : (
          <span className={styles.dot} />
        )}
      </div>
      <div className={styles.text}>
        <div className={styles.title}>{title}</div>
        {detail && <div className={styles.detail}>{detail}</div>}
      </div>
    </div>
  );
}
