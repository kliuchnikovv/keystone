import { Lightbulb, Plug, Thermometer, HelpCircle } from 'lucide-react';
import styles from './FoundDeviceItem.module.css';
import { Chip } from '../Chip/Chip';
import { Button } from '../Button/Button';
import type { DiscoveredDevice } from '../../api/types';

export interface FoundDeviceItemProps {
  device: DiscoveredDevice;
  added?: boolean;
  isNew?: boolean;
  onAdd?: () => void;
}

function Icon({ type }: { type?: string }) {
  switch (type) {
    case 'light':
      return <Lightbulb size={20} />;
    case 'plug':
      return <Plug size={20} />;
    case 'sensor':
      return <Thermometer size={20} />;
    default:
      return <HelpCircle size={20} />;
  }
}

export function FoundDeviceItem({ device, added, isNew, onAdd }: FoundDeviceItemProps) {
  return (
    <article
      className={[styles.root, isNew && styles.new, added && styles.added]
        .filter(Boolean)
        .join(' ')}
    >
      <div className={styles.icon}>
        <Icon type={device.type} />
      </div>
      <div className={styles.body}>
        <h3 className={styles.name}>{device.name}</h3>
        <div className={styles.meta}>
          {added ? (
            <Chip size="sm" tone="success">
              ✓ добавлен
            </Chip>
          ) : (
            <>
              <Chip size="sm" tone="accent">
                {device.transport}
              </Chip>
              {device.manufacturer && (
                <span className={styles.manuf}>{device.manufacturer}</span>
              )}
              {device.discriminator !== undefined && (
                <span className={styles.manuf}>код {device.discriminator}</span>
              )}
            </>
          )}
        </div>
      </div>
      {!added && (
        <Button variant="primary" size="sm" onClick={onAdd}>
          Добавить
        </Button>
      )}
    </article>
  );
}
