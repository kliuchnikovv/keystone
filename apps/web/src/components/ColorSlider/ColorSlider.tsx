import { useId } from 'react';
import styles from './ColorSlider.module.css';
import { useDragValue } from '../../hooks/useDragValue';

export interface ColorSliderProps {
  /** Оттенок 0..360. */
  hue: number;
  /** Насыщенность 0..100. */
  saturation: number;
  disabled?: boolean;
  onCommitHue?: (v: number) => void;
  onCommitSaturation?: (v: number) => void;
}

/**
 * Оттенок и насыщенность отдельными полосами.
 *
 * Разделены намеренно: в Matter это независимые команды (`MoveToHue` и
 * `MoveToSaturation`), и цветовой круг заставлял бы отправлять обе даже когда
 * пользователь двигает одну.
 */
export function ColorSlider({
  hue,
  saturation,
  disabled,
  onCommitHue,
  onCommitSaturation,
}: ColorSliderProps) {
  const hueId = useId();
  const satId = useId();
  const hueDrag = useDragValue(hue);
  const satDrag = useDragValue(saturation);
  const h = clamp(hueDrag.current, 0, 360);
  const s = clamp(satDrag.current, 0, 100);
  const current = `hsl(${h} ${Math.max(s, 12)}% 55%)`;

  return (
    <div className={styles.root}>
      <div className={styles.row}>
        <div className={styles.track} data-kind="hue">
          <div className={styles.thumb} style={{ left: `${(h / 360) * 100}%`, background: current }} />
        </div>
        <input
          id={hueId}
          type="range"
          min={0}
          max={360}
          step={1}
          value={h}
          disabled={disabled}
          aria-label="Оттенок"
          className={styles.input}
          onChange={(e) => hueDrag.onInput(Number(e.currentTarget.value))}
          onMouseUp={(e) => hueDrag.onRelease(Number(e.currentTarget.value), onCommitHue)}
          onTouchEnd={(e) => hueDrag.onRelease(Number(e.currentTarget.value), onCommitHue)}
        />
      </div>

      <div className={styles.row}>
        {/* Полоса насыщенности красится текущим оттенком — иначе непонятно,
            что именно станет бледнее. */}
        <div
          className={styles.track}
          style={{
            background: `linear-gradient(90deg, hsl(${h} 0% 55%), hsl(${h} 100% 55%))`,
          }}
        >
          <div className={styles.thumb} style={{ left: `${s}%`, background: current }} />
        </div>
        <input
          id={satId}
          type="range"
          min={0}
          max={100}
          step={1}
          value={s}
          disabled={disabled}
          aria-label="Насыщенность"
          className={styles.input}
          onChange={(e) => satDrag.onInput(Number(e.currentTarget.value))}
          onMouseUp={(e) => satDrag.onRelease(Number(e.currentTarget.value), onCommitSaturation)}
          onTouchEnd={(e) => satDrag.onRelease(Number(e.currentTarget.value), onCommitSaturation)}
        />
      </div>

      <div className={styles.readout}>
        <span className={styles.swatch} style={{ background: current }} />
        <span>
          {h}° · {s}%
        </span>
      </div>
    </div>
  );
}

function clamp(v: number, min: number, max: number): number {
  if (Number.isNaN(v)) return min;
  return Math.max(min, Math.min(max, v));
}
