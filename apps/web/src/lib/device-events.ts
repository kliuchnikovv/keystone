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

  not_confirmed: { short: 'Без ответа', long: 'Устройство не подтвердило изменение', tone: 'danger' },
};

/** Русские названия того, что мы просили изменить, — для текста ошибки. */
const CONTROL_NAMES: Record<string, string> = {
  value: 'включение',
  level: 'яркость',
  kelvin: 'температуру',
  hue: 'цвет',
  saturation: 'насыщенность',
  locked: 'замок',
  mode: 'режим',
  valve_open: 'клапан',
  target_temp: 'уставку температуры',
  playback: 'воспроизведение',
  run_state: 'работу',
};

/**
 * describeUnconfirmed формулирует, что именно не сработало.
 *
 * Команда уходит без ошибки и устройство молчит — сама по себе такая тишина
 * читается как «приложение сломано». Текст должен назвать конкретный контрол,
 * иначе пользователю нечего проверять.
 */
export function describeUnconfirmed(data?: Record<string, unknown>): string {
  const state = typeof data?.state === 'string' ? data.state : undefined;
  const name = (state && CONTROL_NAMES[state]) ?? state;
  const waited = typeof data?.waited_ms === 'number' ? Math.round(data.waited_ms / 1000) : undefined;
  const tail = waited ? ` за ${waited} с` : '';
  return name
    ? `Устройство не подтвердило ${name}${tail}. Команда ушла, но состояние не изменилось.`
    : `Устройство не подтвердило изменение${tail}.`;
}

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
