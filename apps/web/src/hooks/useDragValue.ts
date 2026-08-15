import { useEffect, useRef, useState } from 'react';

/**
 * Значение слайдера во время перетаскивания.
 *
 * `<input type="range">` с `value` из пропсов — контролируемый: React
 * возвращает DOM-значение обратно на каждое событие input, если состояние не
 * изменилось. Слайдер, которому передали только `onCommit`, из-за этого не
 * едет за пальцем, а в момент отпускания отдаёт **старое** значение. Именно
 * так «не менялась яркость»: команда уходила с тем же числом, что уже стоит.
 *
 * Хук держит значение локально, пока пользователь тянет, и снова слушает
 * пропсы, когда тот отпустил — чтобы устройство могло поправить нас, если оно
 * округлило или ограничило значение.
 */
export function useDragValue(value: number): {
  /** Что показывать: живое во время перетаскивания, из пропсов в остальное время. */
  current: number;
  /** Повесить на onChange. */
  onInput: (v: number) => void;
  /** Обернуть onCommit: вернёт финальное значение и отпустит контроль. */
  onRelease: (v: number, commit?: (v: number) => void) => void;
} {
  const [local, setLocal] = useState<number | undefined>();
  // Значение из пропсов, пришедшее во время перетаскивания, игнорируем: иначе
  // эхо от устройства дёргало бы ползунок под рукой.
  const dragging = useRef(false);

  useEffect(() => {
    if (!dragging.current) setLocal(undefined);
  }, [value]);

  return {
    current: local ?? value,
    onInput: (v) => {
      dragging.current = true;
      setLocal(v);
    },
    onRelease: (v, commit) => {
      dragging.current = false;
      setLocal(v);
      commit?.(v);
    },
  };
}
