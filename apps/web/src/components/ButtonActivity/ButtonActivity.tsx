import { useEffect, useState } from 'react';
import styles from './ButtonActivity.module.css';
import { useLastEvent, EVENT_HIGHLIGHT_MS } from '../../hooks/useRecentEvent';

/**
 * Экран кнопки или пульта.
 *
 * У такого устройства нет состояния: нажатие существует только как событие.
 * Поэтому здесь крупный индикатор последнего жеста: он же подтверждает, что
 * связь с устройством жива. Историю показывать не надо — на экране устройства
 * уже есть общая секция журнала, и второй список тех же событий только
 * дробит внимание.
 */
export function ButtonActivity({ deviceId }: { deviceId: string }) {
  const last = useLastEvent(deviceId);
  const [fresh, setFresh] = useState(false);

  useEffect(() => {
    if (!last) return;
    setFresh(true);
    const left = last.at + EVENT_HIGHLIGHT_MS - Date.now();
    if (left <= 0) {
      setFresh(false);
      return;
    }
    const timer = window.setTimeout(() => setFresh(false), left);
    return () => window.clearTimeout(timer);
  }, [last]);

  return (
    <div className={styles.root}>
      <div className={styles.stage} data-fresh={fresh} data-tone={last?.tone ?? 'neutral'}>
        {/* key перезапускает пульс на каждом новом жесте */}
        <span key={last?.at ?? 'idle'} className={styles.ring} aria-hidden />
        <span className={styles.label}>{last ? last.long : 'Нажми кнопку на устройстве'}</span>
        {last && <time className={styles.when}>{timeAgo(last.at)}</time>}
      </div>
    </div>
  );
}

function timeAgo(at: number): string {
  const sec = Math.max(0, Math.round((Date.now() - at) / 1000));
  if (sec < 5) return 'только что';
  if (sec < 60) return `${sec} с назад`;
  const min = Math.round(sec / 60);
  return `${min} мин назад`;
}
