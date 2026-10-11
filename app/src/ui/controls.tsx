// Small building blocks shared by the forms.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View, type TextInputProps } from 'react-native';
import { useTheme } from './theme';

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
      <Text style={{ color: selected ? t.onAccent : t.text, fontWeight: selected ? '700' : '500', fontSize: 14 }}>{label}</Text>
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

export function Button({ label, onPress, kind = 'primary', disabled, a11y }: { label: string; onPress(): void; kind?: 'primary' | 'plain' | 'danger'; disabled?: boolean; a11y?: string }) {
  const t = useTheme();
  const bg = kind === 'primary' ? t.accent : 'transparent';
  const fg = kind === 'primary' ? t.onAccent : kind === 'danger' ? t.bad : t.accent;
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={a11y ?? label}
      accessibilityState={{ disabled: !!disabled }}
      disabled={disabled}
      onPress={onPress}
      style={[styles.button, { backgroundColor: bg, borderColor: kind === 'primary' ? bg : t.border, opacity: disabled ? 0.5 : 1 }]}
    >
      <Text style={{ color: fg, fontWeight: '600', fontSize: 15 }}>{label}</Text>
    </Pressable>
  );
}

/** Delete that must be tapped twice (a confirmation that works on every platform). */
export function ConfirmDelete({ onConfirm, a11y, label = 'Delete' }: { onConfirm(): void; a11y: string; label?: string }) {
  const [armed, setArmed] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);
  return (
    <Button
      kind="danger"
      a11y={a11y}
      label={armed ? (label === 'Delete' ? 'Tap again to delete' : `Tap again: ${label.toLowerCase()}`) : label}
      onPress={() => {
        if (armed) {
          onConfirm();
          return;
        }
        setArmed(true);
        timer.current = setTimeout(() => setArmed(false), 3000);
      }}
    />
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
  return text ? <Text accessibilityRole="alert" style={{ color: t.bad, fontSize: 13, marginTop: 6 }}>{text}</Text> : null;
}

const styles = StyleSheet.create({
  chip: { borderWidth: 1, borderRadius: 18, paddingHorizontal: 14, paddingVertical: 8 },
  chips: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  field: { marginTop: 14 },
  fieldLabel: { fontSize: 13, marginBottom: 6 },
  hint: { fontSize: 12, marginTop: 4 },
  input: { borderWidth: 1, borderRadius: 10, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16 },
  picker: { flexDirection: 'row', alignItems: 'center' },
  list: { borderWidth: 1, borderRadius: 10, marginTop: 6, overflow: 'hidden' },
  option: { paddingHorizontal: 12, paddingVertical: 11 },
  button: { borderWidth: 1, borderRadius: 10, paddingVertical: 11, paddingHorizontal: 16, alignItems: 'center' },
});
