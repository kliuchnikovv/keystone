// Small building blocks shared by the forms.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View, type TextInputProps } from 'react-native';
import { radius, space, useTheme } from './theme';

/** Button looks from the design spec: .btn-primary, .btn-secondary, .btn-quiet, .btn-danger. */
type ButtonKind = 'primary' | 'secondary' | 'plain' | 'danger';
/** default — a whole button; compact — inside a card; mini — inside a list row. */
type ButtonSize = 'default' | 'compact' | 'mini';

const BUTTON_GEOM: Record<ButtonKind, Record<ButtonSize, { padV: number; padH: number; r: number; font: number }>> = {
  primary: {
    default: { padV: 14, padH: 18, r: 14, font: 15 },
    compact: { padV: 8, padH: 12, r: 14, font: 13 },
    mini: { padV: 6, padH: 10, r: 14, font: 12.5 },
  },
  secondary: {
    default: { padV: 13, padH: 18, r: 14, font: 15 },
    compact: { padV: 8, padH: 12, r: 14, font: 13 },
    mini: { padV: 6, padH: 10, r: 14, font: 12.5 },
  },
  danger: {
    default: { padV: 13, padH: 18, r: 14, font: 15 },
    compact: { padV: 8, padH: 12, r: 14, font: 13 },
    mini: { padV: 6, padH: 10, r: 14, font: 12.5 },
  },
  plain: {
    default: { padV: 11, padH: 14, r: 12, font: 14 },
    compact: { padV: 8, padH: 12, r: 12, font: 13 },
    mini: { padV: 6, padH: 10, r: 12, font: 12.5 },
  },
};

export function Chip({ label, selected, onPress, a11y }: { label: string; selected: boolean; onPress(): void; a11y?: string }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={a11y ?? label}
      accessibilityState={{ selected }}
      onPress={onPress}
      style={[styles.chip, { borderColor: selected ? t.accent : t.border, backgroundColor: selected ? t.accent : 'transparent' }]}
    >
      <Text style={{ color: selected ? t.onAccent : t.text, fontWeight: selected ? '700' : '500', fontSize: 12.5 }}>{label}</Text>
    </Pressable>
  );
}

export function ChipRow({ children }: { children: ReactNode }) {
  return <View style={styles.chips}>{children}</View>;
}

export function Field({ label, children, hint }: { label: string; children: ReactNode; hint?: string | null }) {
  const t = useTheme();
  return (
    <View style={styles.field}>
      <Text style={[styles.fieldLabel, { color: t.muted }]}>{label}</Text>
      {children}
      {hint ? <Text style={[styles.hint, { color: t.muted }]}>{hint}</Text> : null}
    </View>
  );
}

export function Input(props: TextInputProps & { label: string }) {
  const t = useTheme();
  const { label, style, ...rest } = props;
  return (
    <TextInput
      accessibilityLabel={label}
      placeholderTextColor={t.muted}
      autoCapitalize="none"
      autoCorrect={false}
      {...rest}
      style={[styles.input, { color: t.text, backgroundColor: t.bg, borderColor: t.border }, style]}
    />
  );
}

export function Button({ label, onPress, kind = 'primary', size = 'default', disabled, a11y }: {
  label: string;
  onPress(): void;
  kind?: ButtonKind;
  size?: ButtonSize;
  disabled?: boolean;
  a11y?: string;
}) {
  const t = useTheme();
  const g = BUTTON_GEOM[kind][size];
  const bg = kind === 'primary' ? t.accent : kind === 'secondary' ? t.fill : 'transparent';
  const fg = kind === 'primary' ? t.onAccent : kind === 'danger' ? t.bad : kind === 'secondary' ? t.text : t.accent;
  // A quiet button is text on the card, not an outlined pill.
  const border = kind === 'danger' ? t.bad : kind === 'plain' ? bg : bg;
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={a11y ?? label}
      accessibilityState={{ disabled: !!disabled }}
      disabled={disabled}
      onPress={onPress}
      style={[styles.button, {
        backgroundColor: bg,
        borderColor: border,
        borderRadius: g.r,
        paddingVertical: g.padV,
        paddingHorizontal: g.padH,
        opacity: disabled ? 0.5 : 1,
      }]}
    >
      <Text style={{ color: fg, fontWeight: '600', fontSize: g.font }}>{label}</Text>
    </Pressable>
  );
}

/** Delete that must be tapped twice (a confirmation that works on every platform).
 * Armed, the button asks "Delete?" and a cross takes it back; three seconds of
 * silence and it forgets it was ever armed. */
export function ConfirmDelete({ onConfirm, a11y, label = 'Delete', size = 'default' }: { onConfirm(): void; a11y: string; label?: string; size?: ButtonSize }) {
  const t = useTheme();
  const [armed, setArmed] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);
  const arm = () => {
    setArmed(true);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => setArmed(false), 3000);
  };
  if (!armed) {
    return <Button kind="danger" size={size} a11y={a11y} label={label} onPress={arm} />;
  }
  return (
    <View style={styles.armed}>
      <Text style={[styles.armedLabel, { color: t.muted }]}>Delete?</Text>
      <Button kind="danger" size={size} a11y={a11y} label="Confirm" onPress={onConfirm} />
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={`Cancel ${label.toLowerCase()}`}
        onPress={() => setArmed(false)}
        style={styles.cross}
        hitSlop={8}
      >
        <Text style={{ color: t.muted, fontSize: 16 }}>✕</Text>
      </Pressable>
    </View>
  );
}

/** A collapsed picker: shows the choice, opens a list on tap. */
export function Picker<T>({ label, placeholder, options, selected, onSelect, keyOf, textOf }: {
  label: string;
  placeholder: string;
  options: T[];
  selected: T | null;
  onSelect(o: T): void;
  keyOf(o: T): string;
  textOf(o: T): string;
}) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  return (
    <View>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={label}
        onPress={() => setOpen((o) => !o)}
        style={[styles.input, styles.picker, { backgroundColor: t.bg, borderColor: t.border }]}
      >
        <Text style={{ color: selected ? t.text : t.muted, fontSize: 16, flex: 1 }} numberOfLines={1}>
          {selected ? textOf(selected) : placeholder}
        </Text>
        <Text style={{ color: t.muted }}>{open ? '▴' : '▾'}</Text>
      </Pressable>
      {open && (
        <View style={[styles.list, { borderColor: t.border, backgroundColor: t.card }]}>
          {options.length === 0 && <Text style={{ color: t.muted, padding: 12 }}>Nothing to choose from yet</Text>}
          {options.map((o) => (
            <Pressable
              key={keyOf(o)}
              accessibilityRole="button"
              accessibilityLabel={`Choose ${textOf(o)}`}
              onPress={() => {
                onSelect(o);
                setOpen(false);
              }}
              style={styles.option}
            >
              <Text style={{ color: t.text, fontSize: 15 }}>{textOf(o)}</Text>
            </Pressable>
          ))}
        </View>
      )}
    </View>
  );
}

export function ErrorText({ text }: { text: string | null }) {
  const t = useTheme();
  return text ? <Text accessibilityRole="alert" style={{ color: t.bad, fontSize: 12.5, marginTop: 5 }}>{text}</Text> : null;
}

const styles = StyleSheet.create({
  chip: { borderWidth: 1, borderRadius: radius.pill, paddingHorizontal: 11, paddingVertical: 6 },
  chips: { flexDirection: 'row', flexWrap: 'wrap', gap: space.xs },
  field: { marginTop: space.md },
  fieldLabel: { fontSize: 13, fontWeight: '500', marginBottom: space.xs },
  hint: { fontSize: 11.5, marginTop: space.xs, lineHeight: 17 },
  input: { borderWidth: 1, borderRadius: radius.field, paddingHorizontal: 14, paddingVertical: 11, fontSize: 15 },
  picker: { flexDirection: 'row', alignItems: 'center' },
  list: { borderWidth: 1, borderRadius: radius.field, marginTop: space.xs, overflow: 'hidden' },
  option: { paddingHorizontal: 14, paddingVertical: 11 },
  button: { borderWidth: 1, alignItems: 'center' },
  armed: { flexDirection: 'row', alignItems: 'center', gap: space.xs },
  armedLabel: { fontSize: 13, fontWeight: '600' },
  cross: { padding: space.xs },
});
