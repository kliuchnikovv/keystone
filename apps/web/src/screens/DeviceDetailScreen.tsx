import { useMemo, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { useQueryClient } from '@tanstack/react-query';
import { ChevronLeft, Trash2 } from 'lucide-react';
import styles from './DeviceDetailScreen.module.css';
import { useDevicesStore, liveKey } from '../state/devicesStore';
import { useEventsStore } from '../state/eventsStore';
import { Toggle } from '../components/Toggle/Toggle';
import { BrightnessSlider } from '../components/BrightnessSlider/BrightnessSlider';
import { ColorTempSlider } from '../components/ColorTempSlider/ColorTempSlider';
import { Button } from '../components/Button/Button';
import { SectionHeader } from '../components/SectionHeader/SectionHeader';
import { EmptyState } from '../components/EmptyState/EmptyState';
import { invokeAction, writeState, deleteDevice } from '../api/devices';
import { kelvinTone, minutesAgo } from '../lib/format';
import type { Device } from '../api/types';

export function DeviceDetailScreen() {
  const { id: rawId } = useParams<{ id: string }>();
  const id = rawId ? decodeURIComponent(rawId) : '';
  const nav = useNavigate();
  const qc = useQueryClient();

  const device = useDevicesStore((s) => (id ? s.devices.get(id) : undefined));

  if (!device) {
    return (
      <div className={styles.screen}>
        <button type="button" className={styles.back} onClick={() => nav('/')}>
          <ChevronLeft size={18} />
          <span>Дом</span>
        </button>
        <EmptyState emoji="🔎" title="Устройство не найдено" body="Возможно, оно было удалено." />
      </div>
    );
  }

  const onDelete = async () => {
    if (!confirm(`Удалить «${device.name}»?`)) return;
    try {
      const res = await deleteDevice(device.id);
      if (res.adapter_warning) console.warn('adapter decommission warning:', res.adapter_warning);
      if (res.persist_warning) console.warn('persist warning:', res.persist_warning);
    } catch (e) {
      console.warn('delete failed', e);
      alert('Не удалось удалить устройство. Проверь, что keystone запущен.');
      return;
    }
    void qc.invalidateQueries({ queryKey: ['devices'] });
    nav('/');
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

  const setBrightness = (v: number) =>
    void writeState(device.id, { feature: 'brightness', key: 'level', value: v }).catch(
      console.warn,
    );
  const setKelvin = (v: number) =>
    void writeState(device.id, { feature: 'color_temp', key: 'kelvin', value: v }).catch(
      console.warn,
    );

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

        {hasFeature(device, 'brightness') && brightness !== undefined && on && (
          <div className={styles.sliderBlock}>
            <p className={styles.sliderLabel}>Яркость</p>
            <BrightnessSlider value={brightness} size="lg" onCommit={setBrightness} />
          </div>
        )}

        {hasFeature(device, 'color_temp') && kelvin !== undefined && on && (
          <div className={styles.sliderBlock}>
            <p className={styles.sliderLabel}>Цветовая температура — {kelvinTone(kelvin)}</p>
            <ColorTempSlider value={kelvin} onCommit={setKelvin} />
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
