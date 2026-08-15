/**
 * Человеческие названия событий устройств.
 *
 * Кнопка — единственный класс устройств, у которого нет состояния вообще:
 * «нажали» существует только как событие и живёт доли секунды. Поэтому UI
 * обязан показать сам факт нажатия, иначе устройство выглядит неисправным.
 */
export interface EventLabel {
  /** Короткая подпись для плитки. */
  short: string;
  /** Развёрнутая — для экрана устройства. */
  long: string;
  tone: 'neutral' | 'accent' | 'danger';
}

const LABELS: Record<string, EventLabel> = {
  button_pressed: { short: 'Нажатие', long: 'Кнопка нажата', tone: 'accent' },
  button_released: { short: 'Отпущено', long: 'Кнопка отпущена', tone: 'neutral' },
  button_long_press: { short: 'Удержание', long: 'Долгое нажатие', tone: 'accent' },
  button_multi_press: { short: 'Серия', long: 'Несколько нажатий', tone: 'accent' },

  motion_detected: { short: 'Движение', long: 'Обнаружено движение', tone: 'accent' },
  smoke_alarm: { short: 'Дым!', long: 'Сработал датчик дыма', tone: 'danger' },
  co_alarm: { short: 'CO!', long: 'Сработал датчик угарного газа', tone: 'danger' },
  battery_low: { short: 'Батарея', long: 'Низкий заряд батареи', tone: 'danger' },

  cycle_complete: { short: 'Готово', long: 'Цикл завершён', tone: 'accent' },
  valve_changed: { short: 'Клапан', long: 'Клапан переключился', tone: 'neutral' },
  ev_connected: { short: 'Авто', long: 'Автомобиль подключён', tone: 'accent' },
  ev_disconnected: { short: 'Отключено', long: 'Автомобиль отключён', tone: 'neutral' },
};

/**
 * describeEvent превращает доменное имя события в подпись. Незнакомое имя
 * показывается как есть: новое событие на бэкенде должно быть видно, а не
 * молча исчезать.
 */
export function describeEvent(name: string, data?: Record<string, unknown>): EventLabel {
  const base = LABELS[name];
  if (!base) {
    return { short: name, long: name, tone: 'neutral' };
  }
  // Matter сообщает, сколько раз нажали; «двойное» и «тройное» — разные жесты,
  // и рулить ими пользователь будет по-разному.
  if (name === 'button_multi_press') {
    const count = pressCount(data);
    if (count === 2) return { short: 'Двойное', long: 'Двойное нажатие', tone: 'accent' };
    if (count === 3) return { short: 'Тройное', long: 'Тройное нажатие', tone: 'accent' };
    if (count && count > 3) {
      return { short: `×${count}`, long: `${count} нажатий подряд`, tone: 'accent' };
    }
  }
  return base;
}

function pressCount(data?: Record<string, unknown>): number | undefined {
  const raw =
    data?.totalNumberOfPressesCounted ??
    data?.TotalNumberOfPressesCounted ??
    data?.newPosition;
  return typeof raw === 'number' ? raw : undefined;
}
