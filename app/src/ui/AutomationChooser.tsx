// The screen behind «+ → Automation»: what should trigger it?
import { Pressable, ScrollView, StyleSheet, Text, View } from './kit';
import { Glyph, type GlyphName } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { Suggestions } from './Suggestions';
import type { Suggestion } from '../model/suggestions';
import { useTheme } from './theme';

export interface AutomationChooserProps {
  onBack(): void;
  /** A schedule: at a time of day, at sunrise, on a date. */
  onSchedule(): void;
  /** A timer: after a countdown. */
  onTimer(): void;
  /** A rule: when a device does something. */
  onRule(): void;
  /** The automations this home could run right now. */
  suggestions?: Suggestion[];
  /** A suggestion was picked: the rule editor opens with it filled in. */
  onPickSuggestion?(s: Suggestion): void;
}

/** One of the three kinds of automation. */
function KindRow({
  glyph,
  label,
  hint,
  onPress,
}: {
  glyph: GlyphName;
  label: string;
  hint: string;
  onPress(): void;
}) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      onPress={onPress}
      style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}
    >
      <View style={[styles.tile, { backgroundColor: t.fill }]}>
        <Glyph name={glyph} size={19} color={t.muted} />
      </View>
      <View style={styles.text}>
        <Text style={[styles.name, { color: t.text }]}>{label}</Text>
        <Text style={[styles.hint, { color: t.muted }]}>{hint}</Text>
      </View>
      <Glyph name="chevronRight" size={16} color={t.muted} />
    </Pressable>
  );
}

export function AutomationChooser({
  onBack,
  onSchedule,
  onTimer,
  onRule,
  suggestions = [],
  onPickSuggestion,
}: AutomationChooserProps) {
  const t = useTheme();
  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader
        title="New automation"
        left={
          <Pressable accessibilityRole="button" accessibilityLabel="Back" onPress={onBack} style={styles.back}>
            <Glyph name="back" size={20} color={t.muted} />
          </Pressable>
        }
      />

      <View style={[styles.note, { backgroundColor: t.card, borderColor: t.border }]}>
        <Text style={{ color: t.muted, fontSize: 12.5 }}>What should trigger it?</Text>
      </View>

      {suggestions.length > 0 && onPickSuggestion ? (
        <>
          <Text style={[styles.label, { color: t.muted }]}>SUGGESTED FOR YOUR HOME</Text>
          <Suggestions items={suggestions} onPick={onPickSuggestion} />
          <Text style={[styles.label, { color: t.muted }]}>OR START FROM SCRATCH</Text>
        </>
      ) : null}

      <KindRow glyph="schedule" label="Schedule" hint="At a time of day / sunrise / a date" onPress={onSchedule} />
      <KindRow glyph="timer" label="Timer" hint="After a countdown" onPress={onTimer} />
      <KindRow glyph="rule" label="Rule" hint="When a device does something" onPress={onRule} />
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: 40, gap: 8 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
  note: { borderWidth: 1, borderRadius: 16, padding: 13 },
  label: { fontSize: 10.5, fontWeight: '700', letterSpacing: 1.4, marginTop: 14, marginBottom: 2 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 16, borderWidth: 1 },
  tile: { width: 38, height: 38, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  text: { flex: 1, gap: 2, minWidth: 0 },
  name: { fontSize: 14.5, fontWeight: '600' },
  hint: { fontSize: 12 },
});
