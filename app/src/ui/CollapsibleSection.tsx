// A section that folds away: the title, how many things are in it, and a
// chevron. The Automations tab is four of these.
import { useState, type ReactNode } from 'react';
import { Pressable, StyleSheet, Text, View } from './kit';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

export interface CollapsibleSectionProps {
  title: string;
  /** How many things are in the section. */
  count?: number;
  children: ReactNode;
  /** Folded away from the start? */
  initiallyOpen?: boolean;
}

export function CollapsibleSection({ title, count, children, initiallyOpen = true }: CollapsibleSectionProps) {
  const t = useTheme();
  const [open, setOpen] = useState(initiallyOpen);
  return (
    <View style={styles.box}>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={`${open ? 'Collapse' : 'Expand'} ${title}`}
        onPress={() => setOpen((o) => !o)}
        style={styles.head}
      >
        <Text style={[styles.title, { color: t.muted }]}>{title.toUpperCase()}</Text>
        {count !== undefined && (
          <Text style={[styles.count, { color: t.accentInk, backgroundColor: t.accentSoft }]}>{count}</Text>
        )}
        <Glyph name="chevron" size={14} color={t.muted} />
      </Pressable>
      {open ? <View style={styles.body}>{children}</View> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  box: { marginTop: 14 },
  head: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4, paddingHorizontal: 2 },
  title: { fontSize: 10.5, fontWeight: '700', letterSpacing: 1.4 },
  count: { fontSize: 10, fontWeight: '700', paddingHorizontal: 7, paddingVertical: 2, borderRadius: 999, overflow: 'hidden' },
  body: { marginTop: 8 },
});
