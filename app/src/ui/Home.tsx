import { useEffect, useMemo, useState } from 'react';
import { BackHandler, Platform, Pressable, SafeAreaView, StyleSheet, Text, View } from './kit';
import { useCommands } from '../hooks/useCommands';
import { useHome, useHubClient, type HubConnection } from '../hooks/useHub';
import { useStateCache } from '../hooks/useStateCache';
import { createPrefsStore, type Prefs } from '../palette';
import type { HubClientOptions } from '../protocol/client';
import type { Settings } from '../settings';
import { defaultStore } from '../storage';
import { Banner, type BannerAction } from './Banner';
import { AddMenu } from './AddMenu';
import { AutomationEditor } from './AutomationEditor';
import { AutomationsScreen } from './AutomationsScreen';
import { DevicesScreen } from './DevicesScreen';
import { DisplayScreen } from './DisplayScreen';
import { FindDevicesScreen } from './FindDevicesScreen';
import { Glyph, type GlyphName } from './Glyph';
import { HistoryScreen } from './HistoryScreen';
import { HubScreen } from './HubScreen';
import { FirmwareScreen, LocationScreen, RemoteAccessScreen, WifiScreen } from './HubScreens';
import { PluginsScreen } from './PluginsScreen';
import { SettingsScreen } from './SettingsScreen';
import { useTheme, type ThemeMode } from './theme';

/** The three tabs of the dock. Hub and History live inside Settings. */
export type Tab = 'home' | 'automations' | 'settings';

const TABS: { id: Tab; label: string; glyph: GlyphName }[] = [
  { id: 'home', label: 'Home', glyph: 'home' },
  { id: 'automations', label: 'Automations', glyph: 'automation' },
  { id: 'settings', label: 'Settings', glyph: 'settings' },
];

/** The screen under the dock: the tab, or one of the sub-screens Settings opens.
 *  Each one is a door of its own — the hub's affairs do not share a screen. */
export type SubScreen =
  | 'none'
  | 'hub'
  | 'history'
  | 'wifi'
  | 'remoteAccess'
  | 'firmware'
  | 'location'
  | 'display'
  | 'plugins';

export interface HomeProps {
  settings: Settings;
  onChangeHub(): void;
  /** The hub issued a new token: the app stores it and reconnects with it. */
  onTokenChange?(token: string): Promise<void>;
  /** The hub has a relay: connect through it (the address changes). */
  onUseRelay?(url: string): void;
  /** The theme mode the app wears. */
  mode?: ThemeMode;
  onMode?(mode: ThemeMode): void;
  /** For tests: socket factory, timeouts. */
  clientOptions?: Partial<HubClientOptions>;
  /** A connection made elsewhere (the Connect screen hands its client over). */
  client?: HubConnection;
}

/** The signed-in app: connection, tabs, and the toast. */
export function Home({ settings, onChangeHub, onTokenChange, onUseRelay, mode, onMode, clientOptions, client: given }: HomeProps) {
  const owned = useHubClient(settings, clientOptions, given === undefined);
  const client = given ?? owned;
  return (
    <HomeBody
      settings={settings}
      client={client}
      onChangeHub={onChangeHub}
      mode={mode}
      onMode={onMode}
      {...(onTokenChange ? { onTokenChange } : {})}
      {...(onUseRelay ? { onUseRelay } : {})}
    />
  );
}

/** The screens themselves, with a connection that already exists. */
export function HomeBody({
  settings,
  client,
  onChangeHub,
  onTokenChange,
  onUseRelay,
  mode,
  onMode,
}: {
  settings: Settings;
  client: HubConnection;
  onChangeHub(): void;
  onTokenChange?(token: string): Promise<void>;
  onUseRelay?(url: string): void;
  mode?: ThemeMode;
  onMode?(mode: ThemeMode): void;
}) {
  const t = useTheme();
  const ios = Platform.OS === 'ios';
  const home = useHome(client);
  const commands = useCommands(client);
  // What the hub said last time, kept on the phone: shown while it is away.
  useStateCache(client, settings.url);
  const [tab, setTab] = useState<Tab>('home');
  // A sub-screen inside Settings (Hub & health, History, Display): it covers the tab.
  const [sub, setSub] = useState<SubScreen>('none');
  // "Find devices" is a screen of its own: it covers the tabs while it is open.
  const [finding, setFinding] = useState(false);
  // The «+» menu: what can be added.
  const [adding, setAdding] = useState(false);
  // What the «+» menu opened: a device search, a new scene, a new automation.
  const [branch, setBranch] = useState<'none' | 'device' | 'scene' | 'automation'>('none');
  // The app's own preferences: the units it speaks and what Home shows.
  const prefsStore = useMemo(() => createPrefsStore(defaultStore), []);
  const [prefs, setPrefs] = useState<Prefs>({ temperature: 'C', showScenes: true, groupByRoom: true });
  useEffect(() => {
    void prefsStore.load().then(setPrefs);
  }, [prefsStore]);
  const setPrefsAndSave = (next: Partial<Prefs>) => {
    setPrefs((cur) => {
      const merged = { ...cur, ...next };
      void prefsStore.save(merged);
      return merged;
    });
  };

  // Android has no back button in the app: the hardware one must close the screen.
  useEffect(() => {
    if (!finding && sub === 'none' && branch === 'none') return;
    if (Platform.OS !== 'android') return;
    const sub2 = BackHandler.addEventListener('hardwareBackPress', () => {
      if (finding) setFinding(false);
      else if (branch !== 'none') setBranch('none');
      else setSub('none');
      return true;
    });
    return () => sub2.remove();
  }, [finding, sub, branch]);

  // What the banner offers: a retry when the socket is gone, a way back to Connect when
  // the token or the certificate is the problem.
  const action: BannerAction | null =
    home.status === 'closed'
      ? { label: 'Retry', onPress: () => { client.close(); client.connect(); } }
      : home.status === 'unauthorized' || home.status === 'untrusted' || home.status === 'incompatible'
        ? { label: 'Go to Connect', onPress: onChangeHub }
        : null;

  const hubScreen = (
    <HubScreen
      home={home}
      url={settings.url}
      onChangeHub={onChangeHub}
      commands={commands}
      onBack={() => setSub('none')}
    />
  );

  const content =
    branch === 'device' ? (
      <FindDevicesScreen home={home} commands={commands} onClose={() => setBranch('none')} />
    ) : branch === 'scene' ? (
      <AutomationEditor home={home} commands={commands} initial="scene" onClose={() => setBranch('none')} />
    ) : branch === 'automation' ? (
      <AutomationEditor home={home} commands={commands} onClose={() => setBranch('none')} />
    ) : finding ? (
      <FindDevicesScreen home={home} commands={commands} onClose={() => setFinding(false)} />
    ) : sub === 'hub' ? (
      hubScreen
    ) : sub === 'history' ? (
      <HistoryScreen home={home} client={client} onBack={() => setSub('none')} />
    ) : sub === 'wifi' ? (
      <WifiScreen home={home} commands={commands} onBack={() => setSub('none')} />
    ) : sub === 'remoteAccess' ? (
      <RemoteAccessScreen
        client={client}
        commands={commands}
        onBack={() => setSub('none')}
        {...(onTokenChange ? { onTokenChange } : {})}
        {...(onUseRelay ? { onUseRelay } : {})}
      />
    ) : sub === 'firmware' ? (
      <FirmwareScreen home={home} client={client} commands={commands} onBack={() => setSub('none')} />
    ) : sub === 'location' ? (
      <LocationScreen home={home} commands={commands} onBack={() => setSub('none')} />
    ) : sub === 'display' ? (
      <DisplayScreen
        mode={mode ?? 'system'}
        onMode={(m) => onMode?.(m)}
        temperature={prefs.temperature}
        onTemperature={(u) => setPrefsAndSave({ temperature: u })}
        showScenes={prefs.showScenes}
        onShowScenes={(v) => setPrefsAndSave({ showScenes: v })}
        groupByRoom={prefs.groupByRoom}
        onGroupByRoom={(v) => setPrefsAndSave({ groupByRoom: v })}
        onBack={() => setSub('none')}
      />
    ) : sub === 'plugins' ? (
      <PluginsScreen onBack={() => setSub('none')} />
    ) : tab === 'home' ? (
      <DevicesScreen home={home} commands={commands} onFindDevices={() => setFinding(true)} showScenes={prefs.showScenes} />
    ) : tab === 'automations' ? (
      <AutomationsScreen home={home} commands={commands} />
    ) : (
      <SettingsScreen
        home={home}
        mode={mode}
        onOpenHub={() => setSub('hub')}
        onOpenHistory={() => setSub('history')}
        onOpenWifi={() => setSub('wifi')}
        onOpenRemoteAccess={() => setSub('remoteAccess')}
        onOpenFirmware={() => setSub('firmware')}
        onOpenLocation={() => setSub('location')}
        onOpenDisplay={() => setSub('display')}
        onOpenPlugins={() => setSub('plugins')}
      />
    );

  return (
    <SafeAreaView style={[styles.screen, { backgroundColor: t.bg }]}>
      <Banner state={home} url={settings.url} action={action} />
      <View style={styles.body}>{content}</View>
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
      {!finding && sub === 'none' && branch === 'none' && (
        <Dock tab={tab} onTab={setTab} onAdd={() => setAdding(true)} />
      )}
      <AddMenu
        visible={adding}
        onClose={() => setAdding(false)}
        onDevice={() => setBranch('device')}
        onScene={() => setBranch('scene')}
        onAutomation={() => setBranch('automation')}
      />
    </SafeAreaView>
  );
}

/** The dock: three tabs in a floating pill, and the round ink button beside it. */
export function Dock({ tab, onTab, onAdd }: { tab: Tab; onTab(t: Tab): void; onAdd(): void }) {
  const t = useTheme();
  return (
    <View style={styles.dock} pointerEvents="box-none">
      <View
        accessibilityRole="tablist"
        style={[styles.pill, { backgroundColor: t.card, borderColor: t.border }]}
      >
        {TABS.map((x) => (
          <TabButton key={x.id} label={x.label} glyph={x.glyph} active={tab === x.id} onPress={() => onTab(x.id)} />
        ))}
      </View>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel="Add"
        onPress={onAdd}
        style={[styles.fab, { backgroundColor: t.primary }]}
      >
        <Glyph name="plus" size={24} color={t.onPrimary} />
      </Pressable>
    </View>
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
      <Text testID={`tab-${label}`} style={{ color: active ? t.accent : t.muted, fontWeight: active ? '600' : '500', fontSize: 10 }}>
        {label}
      </Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  body: { flex: 1 },
  toast: { position: 'absolute', left: 16, right: 16, padding: 14, borderRadius: 14 },
  // The dock: the pill takes the room, the ink button stands beside it.
  dock: {
    position: 'absolute',
    left: 14,
    right: 14,
    bottom: 16,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 9,
  },
  pill: {
    flex: 1,
    height: 58,
    flexDirection: 'row',
    alignItems: 'center',
    borderRadius: 999,
    borderWidth: StyleSheet.hairlineWidth,
    paddingHorizontal: 8,
  },
  fab: {
    width: 54,
    height: 54,
    borderRadius: 27,
    alignItems: 'center',
    justifyContent: 'center',
  },
  tab: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 3, minHeight: 44 },
});
