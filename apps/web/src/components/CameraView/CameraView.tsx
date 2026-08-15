import { useEffect, useRef, useState } from 'react';
import { VideoOff } from 'lucide-react';
import styles from './CameraView.module.css';
import { openCameraStream } from '../../api/camera';

/**
 * Живой поток с камеры. Компонент держит peer connection ровно пока он
 * смонтирован: уход с экрана закрывает сессию, иначе камера продолжит
 * кодировать для несуществующего зрителя.
 */
export function CameraView({ deviceId, name }: { deviceId: string; name?: string }) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const [state, setState] = useState<'connecting' | 'live' | 'ended' | 'failed'>('connecting');
  const [detail, setDetail] = useState<string>();

  useEffect(() => {
    const session = openCameraStream(deviceId, {
      onStream: (stream) => {
        if (videoRef.current) videoRef.current.srcObject = stream;
      },
      onState: (s) => {
        if (s === 'connected') setState('live');
        else if (s === 'failed') setState('failed');
      },
      onEnded: (reason) => {
        setState('ended');
        setDetail(reason);
      },
      onError: (e) => {
        setState('failed');
        setDetail(e.message);
      },
    });
    return () => session.close();
  }, [deviceId]);

  return (
    <figure className={styles.root}>
      <video
        ref={videoRef}
        className={styles.video}
        autoPlay
        playsInline
        muted
        data-live={state === 'live'}
      />
      {state !== 'live' && (
        <div className={styles.overlay}>
          {state === 'connecting' ? (
            <span className={styles.pulse}>Подключаемся к камере…</span>
          ) : (
            <>
              <VideoOff size={20} />
              <span>{state === 'ended' ? 'Трансляция завершена' : 'Нет видео'}</span>
              {detail && <span className={styles.detail}>{detail}</span>}
            </>
          )}
        </div>
      )}
      {name && <figcaption className={styles.caption}>{name}</figcaption>}
    </figure>
  );
}
