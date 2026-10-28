import { useState } from 'react';
import { Platform, Pressable, SafeAreaView, StyleSheet, Text, View } from 'react-native';
import { useCommands } from '../hooks/useCommands';
import { useHome, useHubClient, type HubConnection } from '../hooks/useHub';
import type { HubClientOptions } from '../protocol/client';
import type { Settings } from '../settings';
import { Banner, type BannerAction } from './Banner';
import { AutomationsScreen } from './AutomationsScreen';
import { DevicesScreen } from './DevicesScreen';
import { Glyph, type GlyphName } from './Glyph';
import { HistoryScreen } from './HistoryScreen';
import { HubScreen } from './HubScreen';
import { useTheme, type PaletteId } from './theme';

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
  /** The hub issued a new token: the app stores it and reconnects with it. */
  onTokenChange?(token: string): Promise<void>;
  /** The palette the app wears. */
  palette?: PaletteId;
  onPalette?(id: PaletteId): void;
  /** For tests: socket factory, timeouts. */
  clientOptions?: Partial<HubClientOptions>;
  /** A connection made elsewhere (the Connect screen hands its client over). */
  client?: HubConnection;
}

/** The signed-in app: connection, tabs, and the toast. */
export function Home({ settings, onChangeHub, onTokenChange, palette, onPalette, clientOptions, client: given }: HomeProps) {
  const owned = useHubClient(settings, clientOptions, given === undefined);
  const client = given ?? owned;
  return (
    <HomeBody
      settings={settings}
      client={client}
      onChangeHub={onChangeHub}
      palette={palette}
      onPalette={onPalette}
      {...(onTokenChange ? { onTokenChange } : {})}
    />
  );
}

/** The screens themselves, with a connection that already exists. */
export function HomeBody({
  settings,
  client,
  onChangeHub,
  onTokenChange,
  palette,
  onPalette,
}: {
  settings: Settings;
  client: HubConnection;
  onChangeHub(): void;
  onTokenChange?(token: string): Promise<void>;
  palette?: PaletteId;
  onPalette?(id: PaletteId): void;
}) {
  const t = useTheme();
  const ios = Platform.OS === 'ios';
  const home = useHome(client);
  const commands = useCommands(client);
  const [tab, setTab] = useState<Tab>('devices');

  // What the banner offers: a retry when the socket is gone, a way back to Connect when
  // the token or the certificate is the problem.
  const action: BannerAction | null =
    home.status === 'closed'
      ? { label: 'Retry', onPress: () => { client.close(); client.connect(); } }
      : home.status === 'unauthorized' || home.status === 'untrusted' || home.status === 'incompatible'
        ? { label: 'Go to Connect', onPress: onChangeHub }
        : null;

  return (
    <SafeAreaView style={[styles.screen, { backgroundColor: t.bg }]}>
      <Banner state={home} url={settings.url} action={action} />
      <View style={styles.body}>
        {tab === 'devices' ? (
          <DevicesScreen home={home} commands={commands} />
        ) : tab === 'automations' ? (
          <AutomationsScreen home={home} commands={commands} />
        ) : tab === 'history' ? (
          <HistoryScreen home={home} client={client} />
        ) : (
          <HubScreen
            home={home}
            url={settings.url}
            onChangeHub={onChangeHub}
            commands={commands}
            client={client}
            palette={palette}
            onPalette={onPalette}
            {...(onTokenChange ? { onTokenChange } : {})}
          />
        )}
      </View>
      {commands.message && (
        <Pressable
          accessibilityRole="alert"
          accessibilityLabel="Dismiss message"
          onPress={commands.dismiss}
          style={[styles.toast, { backgroundColor: commands.tone === 'bad' ? t.bad : t.text, bottom: ios ? 88 : 96 }]}
        >
          <Text style={{ color: t.bg, fontWeight: '600' }}>{commands.message}</Text>
        </Pressable>
      )}
      <View
        style={[
          styles.tabs,
          ios ? styles.tabsPill : styles.tabsPanel,
          { backgroundColor: ios ? t.card : t.tabBar, borderColor: ios ? t.glassBorder : t.border },
        ]}
      >
        {TABS.map((x) => (
          <TabButton key={x.id} label={x.label} glyph={x.glyph} ios={ios} active={tab === x.id} onPress={() => setTab(x.id)} />
        ))}
      </View>
    </SafeAreaView>
  );
}

function TabButton({ label, glyph, ios, active, onPress }: { label: string; glyph: GlyphName; ios: boolean; active: boolean; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="tab"
      accessibilityLabel={label}
      accessibilityState={{ selected: active }}
      onPress={onPress}
      style={styles.tab}
    >
      <Glyph name={glyph} size={ios ? 21 : 22} color={active ? t.accent : t.muted} filled={active} />
      <Text style={{ color: active ? t.accent : t.muted, fontWeight: active ? '600' : '500', fontSize: 10 }}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  body: { flex: 1 },
  toast: { position: 'absolute', left: 16, right: 16, padding: 14, borderRadius: 14 },
  tabs: {
    position: 'absolute',
    flexDirection: 'row',
    alignItems: 'center',
  },
  // iOS: a floating pill, the way the platform has done it since 26.
  tabsPill: {
    left: 16,
    right: 16,
    bottom: 16,
    height: 60,
    borderRadius: 999,
    borderWidth: StyleSheet.hairlineWidth,
    overflow: 'hidden',
    paddingHorizontal: 8,
  },
  // Android and the web: the room mock's full-width bar.
  tabsPanel: {
    left: 0,
    right: 0,
    bottom: 0,
    height: 82,
    paddingTop: 10,
    paddingBottom: 20,
    paddingHorizontal: 8,
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  tab: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 3, minHeight: 44 },
});
