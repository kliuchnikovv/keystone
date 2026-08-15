import { Home, Zap, Sparkles, User } from 'lucide-react';
import styles from './NavBar.module.css';

type Tab = 'home' | 'energy' | 'rules' | 'me';

export interface NavBarProps {
  active?: Tab;
  onSelect?: (t: Tab) => void;
}

const items: Array<{ id: Tab; label: string; Icon: typeof Home }> = [
  { id: 'home', label: 'Дом', Icon: Home },
  { id: 'energy', label: 'Энергия', Icon: Zap },
  { id: 'rules', label: 'Правила', Icon: Sparkles },
  { id: 'me', label: 'Я', Icon: User },
];

export function NavBar({ active = 'home', onSelect }: NavBarProps) {
  return (
    <nav className={styles.root} aria-label="Основная навигация">
      {items.map(({ id, label, Icon }) => (
        <button
          key={id}
          type="button"
          className={[styles.item, active === id && styles.active].filter(Boolean).join(' ')}
          aria-current={active === id ? 'page' : undefined}
          onClick={() => onSelect?.(id)}
          disabled={id !== 'home'}
        >
          <Icon size={22} strokeWidth={active === id ? 2 : 1.6} />
          <span>{label}</span>
        </button>
      ))}
    </nav>
  );
}
