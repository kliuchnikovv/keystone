import { useConnectionStore } from '../../state/connectionStore';
import styles from './ConnectionBanner.module.css';

export function ConnectionBanner() {
  const status = useConnectionStore((s) => s.status);
  if (status === 'live' || status === 'connecting' || status === 'idle') return null;
  return (
    <div className={styles.root} role="status">
      <span className={styles.dot} />
      <span>{status === 'reconnecting' ? 'Переподключаемся…' : 'Нет связи с домом'}</span>
    </div>
  );
}
