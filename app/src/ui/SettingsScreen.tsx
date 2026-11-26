// Settings: the app's own look and the hub's affairs, in one list.
//
// The mockup puts the hub's summary on top and then a short list of rows,
// each opening its own screen. The theme lives under App → Display, not here.
import { Pressable, ScrollView, StyleSheet, Text, View } from './kit';
import type { HomeState } from '../protocol/store';
import { Glyph, type GlyphName } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { useTheme, type ThemeMode } from './theme';

export interface SettingsScreenProps {
  home: HomeState;
  /** The theme the app wears — the Display screen changes it. */
  mode?: ThemeMode;
  /** The sub-screens Settings opens — one door per thing about the hub. */
  onOpenHub(): void;
  onOpenHistory(): void;
  onOpenWifi(): void;
  onOpenRemoteAccess(): void;
  onOpenFirmware(): void;
  onOpenLocation(): void;
  onOpenDisplay?(): void;
  onOpenPlugins?(): void;
}

/** A row of the settings list: an icon, a name, maybe a value, and a chevron. */
export function SettingsRow({
  glyph,
  label,
  value,
  onPress,
  a11y,
}: {
  glyph?: GlyphName;
  label: string;
  value?: string | null;
  onPress?(): void;
  a11y?: string;
}) {
  const t = useTheme();
  const body = (
    <>
      {glyph ? <Glyph name={glyph} size={18} color={t.muted} /> : <View style={styles.dot} />}
      <Text style={[styles.rowName, { color: t.text }]} numberOfLines={1}>
        {label}
      </Text>
      {value ? (
        <Text style={[styles.rowValue, { color: t.muted }]} numberOfLines={1}>
          {value}
        </Text>
      ) : null}
      {onPress ? <Glyph name="chevronRight" size={16} color={t.muted} /> : null}
    </>
  );
  if (!onPress) {
    return (
      <View accessibilityLabel={a11y ?? label} style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}>
        {body}
      </View>
    );
  }
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={a11y ?? label}
      onPress={onPress}
      style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}
    >
      {body}
    </Pressable>
  );
}

/** The name of the theme the app wears, in words. */
function modeName(mode: ThemeMode | undefined): string {
  if (mode === 'light') return 'Light';
  if (mode === 'dark') return 'Dark';
  return 'System';
}

export function SettingsScreen({
  home,
  mode,
  onOpenHub,
  onOpenHistory,
  onOpenWifi,
  onOpenRemoteAccess,
  onOpenFirmware,
  onOpenLocation,
  onOpenDisplay,
  onOpenPlugins,
}: SettingsScreenProps) {
  const t = useTheme();
  const hub = home.hub;
  // The hub is healthy when it answers and no chip is quiet.
  const chipsOk = home.chips.filter((c) => c.status === 'ready').length;
  const chipsAll = home.chips.length;
  const chipValue = chipsAll === 0 ? null : `${chipsOk} of ${chipsAll} ok`;
  const wifi = hub?.wifi?.ssid ?? null;

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader title="Settings" />

      <View style={[styles.summary, { backgroundColor: t.card, borderColor: t.border }]}>
        <View style={[styles.tile, { backgroundColor: t.accent }]}>
          <Glyph name="hub" size={20} color={t.onAccent} />
        </View>
        <View style={styles.summaryText}>
          <Text style={[styles.summaryName, { color: t.text }]} numberOfLines={1}>
            {hub?.name ?? 'Hub'}
          </Text>
          <View style={styles.summaryLine}>
            <View
              style={[
                styles.dot,
                { backgroundColor: home.status === 'ready' ? t.good : t.bad },
              ]}
            />
            <Text style={[styles.summarySub, { color: t.muted }]} numberOfLines={1}>
              {home.status === 'ready' ? `Connected · firmware ${hub?.fw ?? '—'}` : 'Not connected'}
            </Text>
          </View>
        </View>
      </View>

      <Text style={[styles.label, { color: t.muted }]}>APP</Text>
      <SettingsRow
        glyph="settings"
        label="Display"
        value={`${modeName(mode)} · °C`}
        onPress={onOpenDisplay}
        a11y="Display"
      />

      <Text style={[styles.label, { color: t.muted }]}>HUB</Text>
      <SettingsRow glyph="hub" label="Hub & health" value={chipValue} onPress={onOpenHub} a11y="Hub & health" />
      <SettingsRow glyph="history" label="History" onPress={onOpenHistory} a11y="History" />
      <SettingsRow glyph="wifi" label="Wi-Fi" value={wifi} onPress={onOpenWifi} a11y="Wi-Fi" />
      <SettingsRow glyph="signal" label="Remote access" onPress={onOpenRemoteAccess} a11y="Remote access" />

      <Text style={[styles.label, { color: t.muted }]}>DEVICE</Text>
      <SettingsRow glyph="bolt" label="Firmware updates" onPress={onOpenFirmware} a11y="Firmware updates" />
      <SettingsRow glyph="room" label="Location" onPress={onOpenLocation} a11y="Location" />

      {onOpenPlugins ? (
        <>
          <Text style={[styles.label, { color: t.muted }]}>MORE</Text>
          <SettingsRow glyph="chip" label="Plugins" onPress={onOpenPlugins} a11y="Plugins" />
        </>
      ) : null}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: 110, gap: 8 },
  summary: { flexDirection: 'row', alignItems: 'center', gap: 12, padding: 13, borderRadius: 22, borderWidth: 1 },
  summaryText: { flex: 1, gap: 2, minWidth: 0 },
  summaryName: { fontSize: 15, fontWeight: '600' },
  summaryLine: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  summarySub: { fontSize: 12, flex: 1 },
  tile: { width: 40, height: 40, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  label: { fontSize: 10.5, fontWeight: '700', letterSpacing: 1.4, marginTop: 14, marginBottom: 2 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 14, paddingVertical: 12, borderRadius: 16, borderWidth: 1 },
  rowName: { fontSize: 14, fontWeight: '500', flexShrink: 1 },
  rowValue: { fontSize: 12, fontWeight: '600', marginLeft: 'auto' },
  dot: { width: 8, height: 8, borderRadius: 4, marginHorizontal: 5 },
});
