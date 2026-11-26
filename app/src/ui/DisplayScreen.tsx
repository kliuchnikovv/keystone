// The app's preferences: the theme it wears, the units it speaks, and what
// the Home screen shows.
//
// The theme lives here and nowhere else — the picker on the Hub tab is gone.
import { Pressable, ScrollView, StyleSheet, Text, View } from './kit';
import { Glyph } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { MODES, THEMES, useTheme, type ThemeMode } from './theme';

export interface DisplayScreenProps {
  mode: ThemeMode;
  onMode(mode: ThemeMode): void;
  /** The temperature unit the app shows. */
  temperature: 'C' | 'F';
  onTemperature(unit: 'C' | 'F'): void;
  /** The Home screen's options. */
  showScenes: boolean;
  onShowScenes(on: boolean): void;
  groupByRoom: boolean;
  onGroupByRoom(on: boolean): void;
  onBack(): void;
}

/** A row of mutually exclusive choices: the track, and the chosen one on top. */
export function Segmented<T extends string>({
  options,
  value,
  onChange,
  a11y,
}: {
  options: { id: T; label: string }[];
  value: T;
  onChange(v: T): void;
  a11y?: string;
}) {
  const t = useTheme();
  return (
    <View accessibilityLabel={a11y} style={[styles.seg, { backgroundColor: t.fill }]}>
      {options.map((o) => {
        const on = o.id === value;
        return (
          <Pressable
            key={o.id}
            accessibilityRole="button"
            accessibilityLabel={o.label}
            accessibilityState={{ selected: on }}
            onPress={() => onChange(o.id)}
            style={[styles.segItem, on ? { backgroundColor: t.card } : null]}
          >
            <Text style={{ color: on ? t.text : t.muted, fontSize: 12.5, fontWeight: '600' }}>{o.label}</Text>
          </Pressable>
        );
      })}
    </View>
  );
}

/** A switch: a track with a knob. */
export function Switch({ value, onChange, a11y }: { value: boolean; onChange(v: boolean): void; a11y: string }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="switch"
      accessibilityLabel={a11y}
      accessibilityState={{ checked: value }}
      onPress={() => onChange(!value)}
      style={[styles.track, { backgroundColor: value ? t.primary : t.fill }]}
    >
      <View
        style={[
          styles.knob,
          { backgroundColor: value ? t.card : t.muted, alignSelf: value ? 'flex-end' : 'flex-start' },
        ]}
      />
    </Pressable>
  );
}

/** A little picture of a theme: the page, a gold line, a card. */
function ThemePreview({ which }: { which: 'light' | 'dark' }) {
  const theme = THEMES[which];
  return (
    <View style={[styles.preview, { backgroundColor: theme.bg }]}>
      <View style={[styles.previewGold, { backgroundColor: theme.accent }]} />
      <View style={[styles.previewCard, { backgroundColor: theme.card, borderColor: theme.border }]} />
    </View>
  );
}

/** System: half paper, half night — it follows the phone, so show both. */
function SystemPreview() {
  const halves: ('light' | 'dark')[] = ['light', 'dark'];
  return (
    <View style={[styles.preview, styles.previewSplit]}>
      {halves.map((which) => {
        const theme = THEMES[which];
        return (
          <View key={which} style={[styles.previewHalf, { backgroundColor: theme.bg }]}>
            <View style={[styles.previewGold, { backgroundColor: theme.accent }]} />
            <View style={[styles.previewCard, { backgroundColor: theme.card, borderColor: theme.border }]} />
          </View>
        );
      })}
    </View>
  );
}

/** The long name under each theme card. */
const THEME_NAME: Record<ThemeMode, string> = {
  system: 'System · auto',
  light: 'Light · paper',
  dark: 'Dark · warm',
};

export function DisplayScreen({
  mode,
  onMode,
  temperature,
  onTemperature,
  showScenes,
  onShowScenes,
  groupByRoom,
  onGroupByRoom,
  onBack,
}: DisplayScreenProps) {
  const t = useTheme();
  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader
        title="Display"
        left={
          <Pressable accessibilityRole="button" accessibilityLabel="Settings" onPress={onBack} style={styles.back}>
            <Glyph name="back" size={20} color={t.muted} />
          </Pressable>
        }
      />

      <Text style={[styles.label, { color: t.muted }]}>THEME</Text>
      <View style={styles.previews}>
        {MODES.map((m) => {
          const on = m.id === mode;
          return (
            <Pressable
              key={m.id}
              accessibilityRole="button"
              accessibilityLabel={m.name}
              accessibilityState={{ selected: on }}
              onPress={() => onMode(m.id)}
              style={[
                styles.previewCardBox,
                {
                  borderColor: on ? t.accent : t.border,
                  borderWidth: on ? 2 : 1,
                  backgroundColor: on ? t.accentSoft : t.card,
                  padding: on ? 9 : 10,
                },
              ]}
            >
              {m.id === 'system' ? <SystemPreview /> : <ThemePreview which={m.id} />}
              <View style={styles.previewHead}>
                <Text style={[styles.previewName, { color: t.text }]} numberOfLines={1}>
                  {THEME_NAME[m.id]}
                </Text>
                {on ? <Glyph name="check" size={14} color={t.accent} /> : null}
              </View>
            </Pressable>
          );
        })}
      </View>

      <Text style={[styles.label, { color: t.muted }]}>TEMPERATURE</Text>
      <Segmented
        a11y="Temperature"
        options={[
          { id: 'C' as const, label: '°C' },
          { id: 'F' as const, label: '°F' },
        ]}
        value={temperature}
        onChange={onTemperature}
      />

      <Text style={[styles.label, { color: t.muted }]}>HOME SCREEN</Text>
      <View style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}>
        <View style={styles.rowText}>
          <Text style={[styles.rowName, { color: t.text }]}>Show scenes on Home</Text>
          <Text style={[styles.rowHint, { color: t.muted }]}>A quick row above the rooms</Text>
        </View>
        <Switch value={showScenes} onChange={onShowScenes} a11y="Show scenes on Home" />
      </View>
      <View style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}>
        <View style={styles.rowText}>
          <Text style={[styles.rowName, { color: t.text }]}>Group by room</Text>
          <Text style={[styles.rowHint, { color: t.muted }]}>Off = one flat list</Text>
        </View>
        <Switch value={groupByRoom} onChange={onGroupByRoom} a11y="Group by room" />
      </View>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: 40, gap: 8 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
  label: { fontSize: 10.5, fontWeight: '700', letterSpacing: 1.4, marginTop: 14, marginBottom: 2 },
  seg: { flexDirection: 'row', borderRadius: 999, padding: 3 },
  segItem: { flex: 1, alignItems: 'center', paddingVertical: 8, borderRadius: 999 },
  previews: { flexDirection: 'row', gap: 10, marginTop: 4 },
  previewCardBox: { flex: 1, borderWidth: 1, borderRadius: 14, padding: 10 },
  preview: { height: 52, borderRadius: 10, padding: 9, justifyContent: 'center', gap: 5 },
  previewSplit: { flexDirection: 'row', padding: 0, gap: 0, overflow: 'hidden' },
  previewHalf: { flex: 1, padding: 6, justifyContent: 'center', gap: 5 },
  previewGold: { width: '62%', height: 7, borderRadius: 4 },
  previewCard: { width: '84%', height: 7, borderRadius: 4, borderWidth: 1 },
  previewHead: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 6, marginTop: 8 },
  previewName: { flex: 1, fontSize: 11, fontWeight: '600' },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 14, paddingVertical: 12, borderRadius: 16, borderWidth: 1 },
  rowText: { flex: 1, gap: 2, minWidth: 0 },
  rowName: { fontSize: 14, fontWeight: '600' },
  rowHint: { fontSize: 11.5 },
  track: { width: 46, height: 28, borderRadius: 999, padding: 3, justifyContent: 'center' },
  knob: { width: 22, height: 22, borderRadius: 11 },
});
