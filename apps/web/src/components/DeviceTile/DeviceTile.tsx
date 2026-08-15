import { useMemo } from 'react';
import { Lightbulb, Plug, Thermometer, Droplet, Eye, HelpCircle, RadioTower } from 'lucide-react';
import styles from './DeviceTile.module.css';
import { Toggle } from '../Toggle/Toggle';
import { BrightnessSlider } from '../BrightnessSlider/BrightnessSlider';
import { useDevicesStore, liveKey } from '../../state/devicesStore';
import { invokeAction, writeState } from '../../api/devices';
import type { Device } from '../../api/types';
import { kelvinTone } from '../../lib/format';
import { useRecentEvent } from '../../hooks/useRecentEvent';

export type TileState = 'idle' | 'active' | 'pending' | 'offline' | 'error';

export interface DeviceTileProps {
  device: Device;
  size?: 'compact' | 'default' | 'hero';
  onOpen?: (id: string) => void;
}

function IconFor({ type }: { type: string }) {
  switch (type) {
    case 'light':
      return <Lightbulb size={20} />;
    case 'plug':
    case 'switch':
      return <Plug size={20} />;
    case 'sensor':
      return <Thermometer size={20} />;
    case 'humidity':
      return <Droplet size={20} />;
    case 'motion':
    case 'motion_sensor':
      return <Eye size={20} />;
    case 'button':
      return <RadioTower size={20} />;
    default:
      return <HelpCircle size={20} />;
  }
}

function hasFeature(d: Device, key: string): boolean {
  return d.features.some((f) => f.key === key);
}

export function DeviceTile({ device, size = 'default', onOpen }: DeviceTileProps) {
  const on = useDevicesStore((s) =>
    hasFeature(device, 'onoff')
      ? (s.liveState.get(liveKey(device.id, 'onoff', 'value')) as boolean | undefined)
      : undefined,
  );
  const brightness = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'brightness', 'level')) as number | undefined,
  );
  const kelvin = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'color_temp', 'kelvin')) as number | undefined,
  );
  const temperature = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'temperature', 'celsius')) as number | undefined,
  );
  const power = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'power_meter', 'watts')) as number | undefined,
  );
  const online = useDevicesStore(
    (s) =>
      (s.liveState.get(liveKey(device.id, 'availability', 'online')) as boolean | undefined) ??
      true,
  );
  const motion = useDevicesStore(
    (s) => s.liveState.get(liveKey(device.id, 'motion', 'detected')) as boolean | undefined,
  );
  // Кнопка не имеет состояния вообще: нажатие живёт долю секунды и существует
  // только как событие. Без этой подсветки пульт выглядит неисправным.
  const recentEvent = useRecentEvent(device.id);
  const pending = useDevicesStore((s) => s.pending.get(liveKey(device.id, 'onoff', 'value')));
  const markPending = useDevicesStore((s) => s.markPending);

  const state: TileState = useMemo(() => {
    if (pending) return 'pending';
    if (!online) return 'offline';
    if (recentEvent) return 'active';
    if (motion === true) return 'active';
    if (on === true) return 'active';
    return 'idle';
  }, [pending, online, on, motion, recentEvent]);

  const clearPending = useDevicesStore((s) => s.clearPending);

  const toggle = async () => {
    if (state === 'offline' || pending) return;
    if (!hasFeature(device, 'onoff')) return;
    const next = !(on ?? false);
    markPending(device.id, 'onoff', 'value');

    // Watchdog: если стрим не подтвердил за 3s — снимаем pending, стейт вернётся к предыдущему.
    const watchdog = window.setTimeout(() => {
      clearPending(device.id, 'onoff', 'value');
    }, 3000);

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

  const setBrightness = (level: number) => {
    if (!hasFeature(device, 'brightness')) return;
    void writeState(device.id, { feature: 'brightness', key: 'level', value: level }).catch(
      console.warn,
    );
  };

  return (
    <article
      className={[
        styles.root,
        styles[`size-${size}`],
        styles[`state-${state}`],
        styles[`type-${device.type}`],
        recentEvent && styles.fired,
      ]
        .filter(Boolean)
        .join(' ')}
      aria-label={device.name}
    >
      {recentEvent && (
        <span
          // Ключ по времени перезапускает анимацию, когда два нажатия приходят
          // подряд: без него второе нажатие визуально теряется.
          key={recentEvent.at}
          className={styles.eventBadge}
          data-tone={recentEvent.tone}
          role="status"
          aria-live="polite"
        >
          {recentEvent.short}
        </span>
      )}
      <header className={styles.header}>
        <span className={styles.icon}>
          <IconFor type={device.type} />
        </span>
        {hasFeature(device, 'onoff') && (
          <Toggle
            checked={on === true}
            pending={state === 'pending'}
            disabled={state === 'offline'}
            onChange={toggle}
            size={size === 'hero' ? 'lg' : 'md'}
            aria-label={`Питание ${device.name}`}
          />
        )}
      </header>

      <div
        className={styles.body}
        onClick={onOpen ? () => onOpen(device.id) : undefined}
        role={onOpen ? 'button' : undefined}
        tabIndex={onOpen ? 0 : undefined}
        onKeyDown={
          onOpen
            ? (e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  onOpen(device.id);
                }
              }
            : undefined
        }
        style={onOpen ? { cursor: 'pointer' } : undefined}
      >
        <h3 className={styles.name}>{device.name}</h3>
        <div className={styles.meta}>{renderMeta(device, { online, motion, power, temperature })}</div>

        {size !== 'compact' &&
          hasFeature(device, 'brightness') &&
          on !== false &&
          brightness !== undefined && (
            <div className={styles.slider}>
              <BrightnessSlider
                value={brightness}
                size={size === 'hero' ? 'lg' : 'md'}
                disabled={state === 'offline'}
                onCommit={setBrightness}
              />
            </div>
          )}

        {size === 'hero' && kelvin !== undefined && (
          <div className={styles.kelvin}>
            <span className={styles.kelvinDot} style={{ background: kelvinCss(kelvin) }} />
            <span>
              {kelvin}K · {kelvinTone(kelvin)}
            </span>
          </div>
        )}

        {device.type === 'sensor' && temperature !== undefined && (
          <div className={styles.big}>
            {temperature.toFixed(0)}
            <span className={styles.bigUnit}>°C</span>
          </div>
        )}

        {device.type === 'plug' && power !== undefined && on && (
          <div className={styles.big}>
            {Math.round(power)}
            <span className={styles.bigUnit}>W</span>
          </div>
        )}
      </div>
    </article>
  );
}

function kelvinCss(k: number): string {
  const t = Math.max(0, Math.min(1, (k - 2200) / (4000 - 2200)));
  const r = Math.round(255 - t * 40);
  const g = Math.round(180 + t * 30);
  const b = Math.round(120 + t * 120);
  return `rgb(${r}, ${g}, ${b})`;
}

function renderMeta(
  d: Device,
  live: { online: boolean; motion?: boolean; power?: number; temperature?: number },
): string {
  const bits: string[] = [];
  if (d.room) bits.push(d.room);
  if (!live.online) bits.push('нет связи');
  else if (d.type === 'motion') bits.push(live.motion ? 'сейчас' : 'спокойно');
  else if (d.type === 'plug' && live.power !== undefined && live.power < 1) bits.push('0 W');
  return bits.join(' · ');
}
