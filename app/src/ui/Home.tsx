import { useState } from 'react';
import { Pressable, SafeAreaView, StyleSheet, Text, View } from 'react-native';
import { useCommands } from '../hooks/useCommands';
import { useHome, useHubClient, type HubConnection } from '../hooks/useHub';
import type { HubClientOptions } from '../protocol/client';
import type { Settings } from '../settings';
import { Banner } from './Banner';
import { AutomationsScreen } from './AutomationsScreen';
import { DevicesScreen } from './DevicesScreen';
import { Glyph, type GlyphName } from './Glyph';
import { HistoryScreen } from './HistoryScreen';
import { HubScreen } from './HubScreen';
import { useTheme } from './theme';

type Tab = 'devices' | 'automations' | 'history' | 'hub';

const TABS: { id: Tab; label: string; glyph: GlyphName }[] = [
  { id: 'devices', label: 'Devices', glyph: 'home' },
  { id: 'automations', label: 'Automations', glyph: 'automation' },
  { id: 'hub', label: 'Hub', glyph: 'hub' },
  { id: 'history', label: 'History', glyph: 'history' },
];

export interface HomeProps {
  settings: Settings;
  onChangeHub(): void;
  /** For tests: socket factory, timeouts. */
  clientOptions?: Partial<HubClientOptions>;
}

/** The signed-in app: connection, tabs, and the failure toast. */
export function Home({ settings, onChangeHub, clientOptions }: HomeProps) {
  const t = useTheme();
  const client = useHubClient(settings, clientOptions);
  const home = useHome(client);
  const commands = useCommands(client);
  const [tab, setTab] = useState<Tab>('devices');

  const fatal = home.status === 'unauthorized' || home.status === 'incompatible' || home.status === 'untrusted';

  return (
    <SafeAreaView style={[styles.screen, { backgroundColor: t.bg }]}>
      <Banner state={home} url={settings.url} />
      {fatal && (
        <Pressable accessibilityRole="button" accessibilityLabel="Edit connection" onPress={onChangeHub} style={styles.fix}>
          <Text style={{ color: t.accent, fontWeight: '600' }}>Edit connection</Text>
        </Pressable>
      )}
      <View style={styles.body}>
        {tab === 'devices' ? (
          <DevicesScreen home={home} commands={commands} />
        ) : tab === 'automations' ? (
          <AutomationsScreen home={home} commands={commands} />
        ) : tab === 'history' ? (
          <HistoryScreen home={home} client={client} />
        ) : (
          <HubScreen home={home} url={settings.url} onChangeHub={onChangeHub} commands={commands} />
        )}
      </View>
      {commands.message && (
        <Pressable accessibilityRole="alert" accessibilityLabel="Dismiss message" onPress={commands.dismiss} style={[styles.toast, { backgroundColor: t.text }]}>
          <Text style={{ color: t.bg, fontWeight: '600' }}>{commands.message}</Text>
        </Pressable>
      )}
      <View style={[styles.tabs, { backgroundColor: t.glass, borderColor: t.glassBorder }]}>
        {TABS.map((x) => (
          <TabButton key={x.id} label={x.label} glyph={x.glyph} active={tab === x.id} onPress={() => setTab(x.id)} />
        ))}
      </View>
    </SafeAreaView>
  );
}

function TabButton({ label, glyph, active, onPress }: { label: string; glyph: GlyphName; active: boolean; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="tab"
      accessibilityLabel={label}
      accessibilityState={{ selected: active }}
      onPress={onPress}
      style={styles.tab}
    >
      <Glyph name={glyph} size={21} color={active ? t.accent : t.muted} filled={active} />
      <Text style={{ color: active ? t.accent : t.muted, fontWeight: active ? '700' : '500', fontSize: 11 }}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  body: { flex: 1 },
  fix: { alignItems: 'center', paddingVertical: 10 },
  toast: { position: 'absolute', left: 16, right: 16, bottom: 88, padding: 14, borderRadius: 14 },
  tabs: { flexDirection: 'row', borderTopWidth: StyleSheet.hairlineWidth, paddingTop: 8 },
  tab: { flex: 1, alignItems: 'center', gap: 3, paddingVertical: 8 },
});