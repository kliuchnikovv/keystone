import { useEffect, useMemo, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { useQueryClient } from '@tanstack/react-query';
import { ChevronLeft, Trash2 } from 'lucide-react';
import styles from './DeviceDetailScreen.module.css';
import { CameraView } from '../components/CameraView/CameraView';
import { ButtonActivity } from '../components/ButtonActivity/ButtonActivity';
import { useDevicesStore, liveKey } from '../state/devicesStore';
import { useDevices } from '../hooks/useDevices';
import { useEventsStore } from '../state/eventsStore';
import { Toggle } from '../components/Toggle/Toggle';
import { BrightnessSlider } from '../components/BrightnessSlider/BrightnessSlider';
import { ColorTempSlider } from '../components/ColorTempSlider/ColorTempSlider';
import { ColorSlider } from '../components/ColorSlider/ColorSlider';
import { Button } from '../components/Button/Button';
import { SectionHeader } from '../components/SectionHeader/SectionHeader';
import { EmptyState } from '../components/EmptyState/EmptyState';
import { invokeAction, writeState, deleteDevice } from '../api/devices';
import { kelvinTone, minutesAgo } from '../lib/format';
import { useUnconfirmed } from '../hooks/useRecentEvent';
import type { Device } from '../api/types';

/** Сколько ждём список, прежде чем признать сервер недоступным. */
const LOAD_GRACE_MS = 4000;

/**
 * useStalled сообщает, что ожидание затянулось. Нужен, потому что «загружаем»
 * без конца — худшее из состояний: пользователь не понимает, сломано ли всё
 * или просто медленно.
 */
function useStalled(waiting: boolean): boolean {
  const [stalled, setStalled] = useState(false);
  useEffect(() => {
    if (!waiting) {
      setStalled(false);
      return;
    }
    const timer = window.setTimeout(() => setStalled(true), LOAD_GRACE_MS);
    return () => window.clearTimeout(timer);
  }, [waiting]);
  return stalled;
}

export function DeviceDetailScreen() {
  const { id: rawId } = useParams<{ id: string }>();
  const id = rawId ? decodeURIComponent(rawId) : '';
  const nav = useNavigate();
  const qc = useQueryClient();

  // Экран может открыться первым — по ссылке, из закладки или после
  // перезагрузки, — а не только переходом с домашнего. Тогда список устройств
  // ещё никто не запрашивал, и без этого вызова экран навсегда оставался бы
  // «устройство не найдено». react-query дедуплицирует по ключу, так что при
  // переходе изнутри приложения второго запроса не будет.
  // Состояние выводится из наличия данных и времени ожидания, а не из флагов
  // react-query: isLoading гаснет в паузах между ретраями, failureCount не
  // сбрасывается после удачного повтора, а до статуса error запрос при
  // недоступном сервере не доходит вовсе. «Данные есть» и «ждём слишком долго»
  // — признаки, которые видно снаружи и которые не зависят от версии
  // библиотеки.
  const [notice, setNotice] = useState<{ tone: 'warn' | 'error'; text: string } | undefined>();
  const devicesQuery = useDevices();
  const loaded = devicesQuery.data !== undefined;
  const stalled = useStalled(!loaded);
  const device = useDevicesStore((s) => (id ? s.devices.get(id) : undefined));

  if (!device) {
    return (
      <div className={styles.screen}>
        <button type="button" className={styles.back} onClick={() => nav('/')}>
          <ChevronLeft size={18} />
          <span>Дом</span>
        </button>
        {loaded ? (
          <EmptyState emoji="🔎" title="Устройство не найдено" body="Возможно, оно было удалено." />
        ) : stalled ? (
          <EmptyState
            emoji="📡"
            title="Нет связи с keystone"
            body="Проверь, что сервер запущен."
            primary={
              <Button
                variant="primary"
                size="md"
                // Полная перезагрузка, а не refetch/invalidate: запрос,
                // умерший на недоступном сервере, ни тем ни другим не
                // оживает — проверено. Кнопка, которая молча ничего не делает,
                // хуже её отсутствия, а перезагрузка сработает всегда.
                onClick={() => window.location.reload()}
              >
                Повторить
              </Button>
            }
          />
        ) : (
          // Список ещё едет. Говорить «не найдено» в этот момент — враньё:
          // устройство может быть на месте.
          <EmptyState emoji="⏳" title="Загружаем устройство" body="Секунду." />
        )}
      </div>
    );
  }

  const leaveToHome = () => {
    void qc.invalidateQueries({ queryKey: ['devices'] });
    nav('/');
  };

  // Подтверждение уже спрашивается в самой секции («Отмена» / «Точно
  // удалить»), поэтому никакого window.confirm здесь нет: он был вторым
  // вопросом поверх первого и, когда браузер подавляет диалоги, молча
  // возвращал false — нажатие просто ничего не делало.
  const onDelete = async () => {
    setNotice(undefined);
    try {
      const res = await deleteDevice(device.id);
      if (res.persist_warning) console.warn('persist warning:', res.persist_warning);
      if (res.adapter_warning) {
        // Устройство удалено из keystone, но транспорт не смог договориться с
        // ним самим — оно продолжит показывать нас среди своих подключённых
        // сервисов. Убрать нас оттуда сможет только пользователь, поэтому
        // остаёмся на экране, пока он не прочитает.
        console.warn('adapter decommission warning:', res.adapter_warning);
        setNotice({
          tone: 'warn',
          text: `«${device.name}» удалено из Keystone, но устройство не ответило и всё ещё считает нас подключённым сервисом. Убери нас в приложении производителя или сбрось устройство к заводским настройкам.`,
        });
        return;
      }
    } catch (e) {
      console.warn('delete failed', e);
      setNotice({ tone: 'error', text: 'Не удалось удалить устройство. Проверь, что keystone запущен.' });
      return;
    }
    leaveToHome();
  };

  return (
    <div className={styles.screen}>
      <button type="button" className={styles.back} onClick={() => nav('/')}>
        <ChevronLeft size={18} />
        <span>Дом</span>
      </button>

      <header className={styles.head}>
        <h1 className={styles.name}>{device.name}</h1>
        <p className={styles.meta}>
          {[device.room, device.transport, 'Активна'].filter(Boolean).join(' · ')}
        </p>
      </header>

      <DeviceMainControl device={device} />

      <HistoryList deviceId={device.id} />

      {notice && (
        <div className={styles.notice} data-tone={notice.tone} role="alert">
          <p>{notice.text}</p>
          <Button
            variant="ghost"
            size="md"
            onClick={() => (notice.tone === 'warn' ? leaveToHome() : setNotice(undefined))}
          >
            {notice.tone === 'warn' ? 'Понятно' : 'Закрыть'}
          </Button>
        </div>
      )}

      <DeviceSettingsSection device={device} onDelete={onDelete} />
    </div>
  );
}

function hasFeature(d: Device, key: string): boolean {
  return d.features.some((f) => f.key === key);
}

function DeviceMainControl({ device }: { device: Device }) {
  const on = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'onoff', 'value')) as boolean | undefined,
  );
  const brightness = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'brightness', 'level')) as number | undefined,
  );
  const hue = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'color', 'hue')) as number | undefined,
  );
  const saturation = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'color', 'saturation')) as number | undefined,
  );
  const kelvin = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'color_temp', 'kelvin')) as number | undefined,
  );
  const power = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'power_meter', 'watts')) as number | undefined,
  );
  const temperature = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'temperature', 'celsius')) as number | undefined,
  );
  const motion = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'motion', 'detected')) as boolean | undefined,
  );
  const pendingOn = useDevicesStore((s) => s.pending.get(liveKey(device.id, 'onoff', 'value')));
  const markPending = useDevicesStore((s) => s.markPending);
  const clearPending = useDevicesStore((s) => s.clearPending);

  const toggle = async () => {
    if (!hasFeature(device, 'onoff') || pendingOn) return;
    const next = !(on ?? false);
    markPending(device.id, 'onoff', 'value');
    const watchdog = window.setTimeout(() => clearPending(device.id, 'onoff', 'value'), 3000);
    try {
      await invokeAction(device.id, {
        feature: 'onoff',
        action: next ? 'turn_on' : 'turn_off',
      });
    } catch (e) {
      window.clearTimeout(watchdog);
      clearPending(device.id, 'onoff', 'value');
      console.warn('toggle failed', e);
    }
  };

  // Отказ в управлении обязан быть виден. Раньше он уходил в console.warn, и
  // отвергнутая устройством команда выглядела как «ползунок просто не
  // работает» — на диагностику этого ушёл целый прогон.
  const [controlError, setControlError] = useState<string | undefined>();
  const control = (what: string, run: Promise<unknown>) =>
    void run.catch((e: Error) => {
      console.warn(`${what} failed`, e);
      setControlError(`Не удалось изменить ${what}: ${e.message}`);
    });

  const setBrightness = (v: number) =>
    control('яркость', writeState(device.id, { feature: 'brightness', key: 'level', value: v }));
  const setKelvin = (v: number) =>
    control('температуру', writeState(device.id, { feature: 'color_temp', key: 'kelvin', value: v }));
  const setHue = (v: number) =>
    control('цвет', writeState(device.id, { feature: 'color', key: 'hue', value: v }));
  const setSaturation = (v: number) =>
    control('насыщенность', writeState(device.id, { feature: 'color', key: 'saturation', value: v }));

  // У кнопки нет ни одного состояния — только события. Экран показывает
  // последний жест и историю, иначе смотреть просто не на что.
  // Отказ и молчание — разные отказы, и оба должны быть видны. Отказ приходит
  // из ответа на запрос, молчание — событием с бэкенда через несколько секунд.
  const unconfirmed = useUnconfirmed(device.id);
  const problem = controlError ?? unconfirmed;
  const errorBanner = problem ? (
    <p className={styles.controlError} role="alert">
      {problem}
    </p>
  ) : null;

  if (hasFeature(device, 'button')) {
    return (
      <section className={styles.main}>
        <ButtonActivity deviceId={device.id} />
      </section>
    );
  }

  // Камера — своя раскладка: видео занимает верх экрана, остальные фичи
  // (звонок, PTZ) идут под ним.
  if (hasFeature(device, 'camera')) {
    return (
      <section className={styles.main}>
        <CameraView deviceId={device.id} name={device.name} />
      </section>
    );
  }

  if (device.type === 'light') {
    return (
      <section className={styles.main}>
        <div className={styles.mainRow}>
          <div>
            <p className={styles.mainLabel}>{on ? 'Включено' : 'Выключено'}</p>
            {brightness !== undefined && on && (
              <p className={styles.mainValue}>
                {Math.round(brightness)}<span className={styles.unit}>%</span>
              </p>
            )}
          </div>
          {hasFeature(device, 'onoff') && (
            <Toggle
              checked={on === true}
              pending={!!pendingOn}
              onChange={toggle}
              size="lg"
              aria-label="Питание"
            />
          )}
        </div>

        {/* Регуляторы показываем по наличию возможности, а не по наличию
            значения: иначе слайдер прячется, пока не приедет состояние, и
            лампа выглядит как «не умеет диммироваться».
            Выключенный свет их НЕ блокирует: выставить яркость на выключенной
            лампе — обычное действие, она от этого включается (в Matter для
            этого есть MoveToLevelWithOnOff), а цвет применяется флагом
            ExecuteIfOff. Блокировка «пока выключено» просто съедала нажатия. */}
        {hasFeature(device, 'brightness') && (
          <div className={styles.sliderBlock}>
            <p className={styles.sliderLabel}>Яркость</p>
            <BrightnessSlider
              value={brightness ?? 0}
              size="lg"
              disabled={brightness === undefined}
              onCommit={setBrightness}
            />
          </div>
        )}

        {hasFeature(device, 'color_temp') && (
          <div className={styles.sliderBlock}>
            <p className={styles.sliderLabel}>
              Цветовая температура{kelvin !== undefined ? ` — ${kelvinTone(kelvin)}` : ''}
            </p>
            <ColorTempSlider
              value={kelvin ?? 2700}
              disabled={kelvin === undefined}
              onCommit={setKelvin}
            />
          </div>
        )}

        {errorBanner}

        {hasFeature(device, 'color') && (
          <div className={styles.sliderBlock}>
            <p className={styles.sliderLabel}>Цвет</p>
            <ColorSlider
              hue={hue ?? 0}
              saturation={saturation ?? 100}
              onCommitHue={setHue}
              onCommitSaturation={setSaturation}
            />
          </div>
        )}
      </section>
    );
  }

  if (device.type === 'plug' || device.type === 'switch') {
    return (
      <section className={styles.main}>
        <div className={styles.mainRow}>
          <div>
            <p className={styles.mainLabel}>{on ? 'Включено' : 'Выключено'}</p>
            {power !== undefined && (
              <p className={styles.mainValue}>
                {Math.round(power)}<span className={styles.unit}>W</span>
              </p>
            )}
          </div>
          {hasFeature(device, 'onoff') && (
            <Toggle
              checked={on === true}
              pending={!!pendingOn}
              onChange={toggle}
              size="lg"
              aria-label="Питание"
            />
          )}
        </div>
      </section>
    );
  }

  if (device.type === 'sensor') {
    return (
      <section className={styles.main}>
        {temperature !== undefined && (
          <p className={styles.mainValue}>
            {temperature.toFixed(1)}<span className={styles.unit}>°C</span>
          </p>
        )}
        <p className={styles.mainLabel}>Температура</p>
      </section>
    );
  }

  if (device.type === 'motion') {
    return (
      <section className={styles.main}>
        <p className={styles.mainValue}>{motion ? 'Движение' : 'Спокойно'}</p>
        <p className={styles.mainLabel}>{motion ? 'сейчас' : 'нет активности'}</p>
      </section>
    );
  }

  return (
    <section className={styles.main}>
      <p className={styles.mainLabel}>Управление для этого типа устройства пока не реализовано.</p>
    </section>
  );
}

function HistoryList({ deviceId }: { deviceId: string }) {
  const events = useEventsStore((s) => s.events);
  const filtered = useMemo(
    () => events.filter((e) => e.deviceId === deviceId).slice(0, 20),
    [events, deviceId],
  );

  return (
    <section>
      <SectionHeader title="ИСТОРИЯ" count={filtered.length} />
      {filtered.length === 0 ? (
        <p className={styles.emptyHint}>Пока пусто. Изменения появятся здесь.</p>
      ) : (
        <ol className={styles.historyList}>
          {filtered.map((e) => (
            <li key={e.id} className={styles.historyItem}>
              <span className={styles.historyTime}>{minutesAgo(new Date(e.at))}</span>
              <span className={styles.historyLabel}>{e.label}</span>
              {e.detail && <span className={styles.historyDetail}>{e.detail}</span>}
            </li>
          ))}
        </ol>
      )}
    </section>
  );
}

function DeviceSettingsSection({
  device,
  onDelete,
}: {
  device: Device;
  onDelete: () => void;
}) {
  const [confirming, setConfirming] = useState(false);
  return (
    <section>
      <SectionHeader title="НАСТРОЙКИ" />
      <dl className={styles.settings}>
        <div className={styles.row}>
          <dt>Комната</dt>
          <dd>{device.room ?? '—'}</dd>
        </div>
        <div className={styles.row}>
          <dt>Имя</dt>
          <dd>{device.name}</dd>
        </div>
        <div className={styles.row}>
          <dt>Тип</dt>
          <dd>{device.type}</dd>
        </div>
        <div className={styles.row}>
          <dt>Транспорт</dt>
          <dd>{device.transport}</dd>
        </div>
        {device.transportRef && (
          <div className={styles.row}>
            <dt>Ref</dt>
            <dd className={styles.mono}>{device.transportRef}</dd>
          </div>
        )}
      </dl>
      <div className={styles.deleteBlock}>
        {confirming ? (
          <div className={styles.confirmRow}>
            <Button variant="ghost" size="md" onClick={() => setConfirming(false)}>
              Отмена
            </Button>
            <Button variant="primary" size="md" onClick={onDelete}>
              Точно удалить
            </Button>
          </div>
        ) : (
          <Button variant="ghost" size="md" block onClick={() => setConfirming(true)}>
            <Trash2 size={14} style={{ marginRight: 6, verticalAlign: 'text-bottom' }} />
            Удалить устройство
          </Button>
        )}
      </div>
    </section>
  );
}
