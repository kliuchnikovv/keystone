import { useState } from 'react';
import { Pressable, SafeAreaView, StyleSheet, Text, View } from 'react-native';
import { useCommands } from '../hooks/useCommands';
import { useHome, useHubClient } from '../hooks/useHub';
import type { HubClientOptions } from '../protocol/client';
import type { Settings } from '../settings';
import { Banner } from './Banner';
import { AutomationsScreen } from './AutomationsScreen';
import { DevicesScreen } from './DevicesScreen';
import { HubScreen } from './HubScreen';
import { useTheme } from './theme';

type Tab = 'devices' | 'automations' | 'hub';

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

  const fatal = home.status === 'unauthorized' || home.status === 'incompatible';

  return (
    <SafeAreaView style={[styles.screen, { backgroundColor: t.bg }]}>
      <Banner state={home} />
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
        ) : (
          <HubScreen home={home} url={settings.url} onChangeHub={onChangeHub} />
        )}
      </View>
      {commands.message && (
        <Pressable accessibilityRole="alert" accessibilityLabel="Dismiss message" onPress={commands.dismiss} style={[styles.toast, { backgroundColor: t.text }]}>
          <Text style={{ color: t.bg, fontWeight: '600' }}>{commands.message}</Text>
        </Pressable>
      )}
      <View style={[styles.tabs, { backgroundColor: t.card, borderTopColor: t.border }]}>
        <TabButton label="Devices" active={tab === 'devices'} onPress={() => setTab('devices')} />
        <TabButton label="Automations" active={tab === 'automations'} onPress={() => setTab('automations')} />
        <TabButton label="Hub" active={tab === 'hub'} onPress={() => setTab('hub')} />
      </View>
    </SafeAreaView>
  );
}

function TabButton({ label, active, onPress }: { label: string; active: boolean; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable accessibilityRole="tab" accessibilityLabel={label} accessibilityState={{ selected: active }} onPress={onPress} style={styles.tab}>
      <Text style={{ color: active ? t.accent : t.muted, fontWeight: active ? '700' : '500', fontSize: 15 }}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  body: { flex: 1 },
  fix: { alignItems: 'center', paddingVertical: 10 },
  toast: { position: 'absolute', left: 16, right: 16, bottom: 72, padding: 14, borderRadius: 12 },
  tabs: { flexDirection: 'row', borderTopWidth: StyleSheet.hairlineWidth },
  tab: { flex: 1, alignItems: 'center', paddingVertical: 14 },
});
