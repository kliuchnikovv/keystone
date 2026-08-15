import { useId } from 'react';
import styles from './ColorTempSlider.module.css';
import { useDragValue } from '../../hooks/useDragValue';
import { kelvinToCss } from '../../lib/colorTemp';
import { kelvinTone } from '../../lib/format';

export interface ColorTempSliderProps {
  value: number;
  min?: number;
  max?: number;
  disabled?: boolean;
  onChange?: (v: number) => void;
  onCommit?: (v: number) => void;
}

export function ColorTempSlider({
  value,
  min = 2200,
  max = 4000,
  disabled,
  onChange,
  onCommit,
}: ColorTempSliderProps) {
  const id = useId();
  const drag = useDragValue(value);
  const v = Math.max(min, Math.min(max, drag.current));
  const pct = ((v - min) / (max - min)) * 100;
  const gradient = `linear-gradient(90deg, ${kelvinToCss(min)}, ${kelvinToCss((min + max) / 2)}, ${kelvinToCss(max)})`;

  return (
    <div className={styles.root}>
      <div className={styles.track} style={{ background: gradient }}>
        <div className={styles.thumb} style={{ left: `${pct}%` }} />
      </div>
      <input
        id={id}
        type="range"
        min={min}
        max={max}
        step={50}
        value={v}
        disabled={disabled}
        aria-label="Цветовая температура"
        className={styles.input}
        onChange={(e) => {
          const next = Number(e.currentTarget.value);
          drag.onInput(next);
          onChange?.(next);
        }}
        onMouseUp={(e) => drag.onRelease(Number(e.currentTarget.value), onCommit)}
        onTouchEnd={(e) => drag.onRelease(Number(e.currentTarget.value), onCommit)}
      />
      <div className={styles.readout}>
        <span className={styles.label}>тёплый</span>
        <span className={styles.value}>
          {v}K · {kelvinTone(v)}
        </span>
        <span className={styles.label}>холодный</span>
      </div>
    </div>
  );
}
