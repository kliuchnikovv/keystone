import { useId } from 'react';
import styles from './BrightnessSlider.module.css';
import { useDragValue } from '../../hooks/useDragValue';

export interface BrightnessSliderProps {
  value: number;
  size?: 'sm' | 'md' | 'lg';
  disabled?: boolean;
  onChange?: (v: number) => void;
  onCommit?: (v: number) => void;
}

export function BrightnessSlider({
  value,
  size = 'md',
  disabled,
  onChange,
  onCommit,
}: BrightnessSliderProps) {
  const id = useId();
  const drag = useDragValue(value);
  const pct = Math.max(0, Math.min(100, drag.current));
  return (
    <div className={[styles.root, styles[`size-${size}`]].join(' ')}>
      <div className={styles.track}>
        <div className={styles.fill} style={{ width: `${pct}%` }} />
      </div>
      <input
        id={id}
        type="range"
        min={0}
        max={100}
        step={1}
        value={pct}
        disabled={disabled}
        aria-label="Яркость"
        className={styles.input}
        onChange={(e) => {
          const v = Number(e.currentTarget.value);
          drag.onInput(v);
          onChange?.(v);
        }}
        onMouseUp={(e) => drag.onRelease(Number(e.currentTarget.value), onCommit)}
        onTouchEnd={(e) => drag.onRelease(Number(e.currentTarget.value), onCommit)}
      />
    </div>
  );
}
