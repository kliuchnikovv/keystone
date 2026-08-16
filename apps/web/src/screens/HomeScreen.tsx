import { useMemo } from 'react';
import { Plus, Plug } from 'lucide-react';
import { useNavigate } from 'react-router-dom';
import styles from './HomeScreen.module.css';
import { useDevices } from '../hooks/useDevices';
import { useDevicesStore, liveKey } from '../state/devicesStore';
import { SectionHeader } from '../components/SectionHeader/SectionHeader';
import { DeviceTile } from '../components/DeviceTile/DeviceTile';
import { NavBar } from '../components/NavBar/NavBar';
import { EmptyState } from '../components/EmptyState/EmptyState';
import { LoadingSkeleton } from '../components/LoadingSkeleton/LoadingSkeleton';
import { Button } from '../components/Button/Button';
import { ConnectionBanner } from '../components/ConnectionBanner/ConnectionBanner';
import type { Device } from '../api/types';

interface Section {
  key: string;
  title: string;
  layout: 'hero' | 'grid2' | 'grid3';
  devices: Device[];
}

const SECTION_META: Record<string, { title: string; layout: Section['layout'] }> = {
  light: { title: 'Свет', layout: 'hero' },
  plug: { title: 'Розетки', layout: 'grid2' },
  sensor: { title: 'Датчики', layout: 'grid3' },
  motion: { title: 'Движение', layout: 'grid3' },
};

const TYPE_ORDER = ['light', 'plug', 'sensor', 'motion'];

function groupDevices(devices: Device[]): Section[] {
  const map = new Map<string, Device[]>();
  for (const d of devices) {
    const key = SECTION_META[d.type] ? d.type : 'other';
    if (!map.has(key)) map.set(key, []);
    map.get(key)!.push(d);
  }
  const sections: Section[] = [];
  for (const key of [...TYPE_ORDER, ...[...map.keys()].filter((k) => !TYPE_ORDER.includes(k))]) {
    const list = map.get(key);
    if (!list?.length) continue;
    const meta = SECTION_META[key] ?? { title: key, layout: 'grid2' as const };
    sections.push({ key, title: meta.title, layout: meta.layout, devices: list });
  }
  return sections;
}

function sectionCounter(
  section: Section,
  snap: { isOn: (id: string) => boolean; watts: (id: string) => number | undefined },
): { text?: string; count?: number } {
  if (section.key === 'light' || section.key === 'plug') {
    const on = section.devices.filter((d) => snap.isOn(d.id)).length;
    if (section.key === 'plug') {
      const totalW = section.devices.reduce((sum, d) => sum + (snap.watts(d.id) ?? 0), 0);
      return { text: `${on} из ${section.devices.length} · ${Math.round(totalW)} W` };
    }
    return { text: `${on} из ${section.devices.length} включено` };
  }
  return { count: section.devices.length };
}

export function HomeScreen() {
  const nav = useNavigate();
  const { isLoading, isError } = useDevices();
  const devices = useDevicesStore((s) => s.devices);
  const liveState = useDevicesStore((s) => s.liveState);

  const list = useMemo(() => [...devices.values()], [devices]);
  const sections = useMemo(() => groupDevices(list), [list]);

  const openAdd = () => nav('/add-device');

  const snap = {
    isOn: (id: string) => liveState.get(liveKey(id, 'onoff', 'value')) === true,
    watts: (id: string) => {
      const v = liveState.get(liveKey(id, 'power_meter', 'watts'));
      return typeof v === 'number' ? v : undefined;
    },
  };

  const content = isLoading ? (
    <LoadingSkeleton />
  ) : isError ? (
    <EmptyState
      emoji="⚠️"
      title="Не получилось загрузить"
      body="Проверь, что keystone запущен на localhost:7777"
    />
  ) : list.length === 0 ? (
    <EmptyState
      emoji="🏠"
      title="Пока ни одного устройства"
      body="Добавь первое — это займёт минуту. Начнём с лампы, розетки или датчика."
      primary={
        <Button variant="primary" size="lg" onClick={openAdd}>
          + Добавить устройство
        </Button>
      }
    />
  ) : (
    <>
      {sections.map((s) => {
        const counter = sectionCounter(s, snap);
        const gridClass =
          s.layout === 'hero' ? styles.hero : s.layout === 'grid3' ? styles.grid3 : styles.grid2;
        return (
          <section key={s.key} className={styles.section}>
            <SectionHeader
              title={s.title.toUpperCase()}
              count={counter.count}
              right={counter.text}
            />
            <div className={gridClass}>
              {s.devices.map((d, i) => (
                <DeviceTile
                  key={d.id}
                  device={d}
                  onOpen={(id) => nav(`/device/${encodeURIComponent(id)}`)}
                  size={
                    s.layout === 'hero' && i === 0
                      ? 'hero'
                      : s.layout === 'grid3'
                        ? 'compact'
                        : 'default'
                  }
                />
              ))}
            </div>
          </section>
        );
      })}
    </>
  );

  return (
    <div className={styles.screen}>
      <header className={styles.header}>
        <div className={styles.heading}>
          <h1 className={styles.greeting}>Доброе утро, Валерий</h1>
          <p className={styles.subtitle}>
            {isLoading ? 'Ищем устройства…' : '22° · солнечно · дом просыпается'}
          </p>
        </div>
        <button
          type="button"
          className={styles.pluginsBtn}
          aria-label="Плагины"
          onClick={() => nav('/plugins')}
        >
          <Plug size={20} />
        </button>
      </header>
      <ConnectionBanner />
      <main className={styles.mainCol}>{content}</main>
      <div className={styles.navSpacer} />

      <button
        type="button"
        className={styles.fab}
        aria-label="Добавить устройство"
        onClick={openAdd}
      >
        <Plus size={24} />
      </button>

      <NavBar active="home" />
    </div>
  );
}
