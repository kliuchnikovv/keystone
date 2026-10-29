import { StatusBar } from 'expo-status-bar';
import { useCallback, useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, View } from 'react-native';
import { useDiscovery, type Discovery } from './src/hooks/useDiscovery';
import { useHome, useHubClient, type HubConnection } from './src/hooks/useHub';
import { createPaletteStore } from './src/palette';
import { createSettingsStore, type Settings } from './src/settings';
import { defaultStore } from './src/storage';
import { ConnectScreen, type ConnectFailure } from './src/ui/ConnectScreen';
import { Home, HomeBody } from './src/ui/Home';
import { SetupScreen } from './src/ui/SetupScreen';
import { ThemeProvider, type PaletteId } from './src/ui/theme';

/** A connection that is being tried right now: the form stays up, the button spins. */
function Trying({
  settings,
  discovery,
  onReady,
  onTry,
  onSetup,
}: {
  settings: Settings;
  discovery: Discovery;
  onReady(settings: Settings, client: HubConnection): void;
  onTry(settings: Settings): void;
  onSetup(): void;
}) {
  const client = useHubClient(settings);
  const home = useHome(client);
  const [failure, setFailure] = useState<ConnectFailure | null>(null);

  // New values: the old verdict is about the old values.
  useEffect(() => setFailure(null), [settings.url, settings.token]);

  useEffect(() => {
    if (home.status === 'ready') {
      // The connection is ours no more: the app takes it, and it must not be closed
      // when this screen goes away.
      client.handOver();
      onReady(settings, client);
      return;
    }
    if (home.status === 'unauthorized') setFailure('token');
    else if (home.status === 'untrusted') setFailure(home.error === 'pin_unsupported' ? 'pin' : 'certificate');
    else if (home.status === 'incompatible') setFailure('version');
    else if (home.status === 'reconnecting') setFailure('unreachable');
  }, [home.status, home.error, settings, client, onReady]);

  return (
    <ConnectScreen
      initial={settings}
      discovery={discovery}
      connecting={failure === null}
      failure={failure}
      onRetry={() => {
        client.close();
        client.connect();
      }}
      onSave={onTry}
      onSetup={onSetup}
    />
  );
}

/** The Connect screen, and the attempt to reach the hub that follows it.
 * mDNS browsing runs only while this screen is up. */
function ConnectFlow({
  initial,
  pending,
  onSetup,
  onConnected,
  onTry,
  onCancel,
}: {
  initial?: Settings | null;
  pending: Settings | null;
  onSetup(): void;
  onConnected(settings: Settings, client: HubConnection): void;
  onTry(settings: Settings): void;
  onCancel?(): void;
}) {
  const discovery = useDiscovery();
  if (pending) {
    return <Trying settings={pending} discovery={discovery} onReady={onConnected} onTry={onTry} onSetup={onSetup} />;
  }
  return (
    <ConnectScreen
      {...(initial ? { initial } : {})}
      discovery={discovery}
      onSetup={onSetup}
      onSave={onTry}
      onCancel={onCancel}
    />
  );
}

export default function App() {
  const store = useMemo(() => createSettingsStore(defaultStore), []);
  const palettes = useMemo(() => createPaletteStore(defaultStore), []);
  // undefined = still reading the saved settings, null = none (ask for them).
  const [settings, setSettings] = useState<Settings | null | undefined>(undefined);
  const [session, setSession] = useState<{ settings: Settings; client: HubConnection } | null>(null);
  const [pending, setPending] = useState<Settings | null>(null);
  const [editing, setEditing] = useState(false);
  const [settingUp, setSettingUp] = useState(false);
  // The palette is the app's own look — a setting of the phone.
  const [palette, setPaletteState] = useState<PaletteId>('sandstone');

  useEffect(() => {
    void store.load().then(setSettings);
    void palettes.load().then(setPaletteState);
  }, [store, palettes]);

  const setPalette = useCallback(
    (id: PaletteId) => {
      setPaletteState(id);
      void palettes.save(id);
    },
    [palettes],
  );

  const onConnected = useCallback(
    (s: Settings, client: HubConnection) => {
      setPending(null);
      setEditing(false);
      setSettings(s);
      setSession({ settings: s, client });
      void store.save(s);
    },
    [store],
  );

  const onChangeHub = useCallback(() => {
    // Back to the Connect screen, with the values we had.
    setSession(null);
    setPending(null);
    setEditing(true);
  }, []);

  const onUseRelay = useCallback(
    (url: string) => {
      // The hub has a relay now: the app talks to the hub through it from now on.
      const base = session?.settings ?? (settings as Settings);
      const next = { ...base, url };
      setSession(null);
      setSettings(next);
      void store.save(next);
    },
    [session, settings, store],
  );

  const onTokenChange = useCallback(
    async (token: string) => {
      // The hub restarted its server with the new token: store it and reconnect.
      const base = session?.settings ?? (settings as Settings);
      const next = { ...base, token };
      await store.save(next);
      setSession(null);
      setSettings(next);
    },
    [session, settings, store],
  );

  const content =
    settings === undefined ? (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
        <ActivityIndicator />
      </View>
    ) : settings === null && settingUp ? (
      <SetupScreen
        onCancel={() => setSettingUp(false)}
        onDone={(s) => {
          setSettingUp(false);
          void store.save(s).then(() => setSettings(s));
        }}
      />
    ) : settings === null || editing || pending !== null ? (
      <ConnectFlow
        initial={settings}
        pending={pending}
        onSetup={() => setSettingUp(true)}
        onConnected={onConnected}
        onTry={(s) => setPending(s)}
        onCancel={editing ? () => setEditing(false) : undefined}
      />
    ) : session ? (
      <HomeBody settings={session.settings} client={session.client} onChangeHub={onChangeHub} onTokenChange={onTokenChange} onUseRelay={onUseRelay} palette={palette} onPalette={setPalette} />
    ) : (
      <Home
        key={`${settings.url}|${settings.token}|${settings.fingerprint ?? ''}`}
        settings={settings}
        onChangeHub={onChangeHub}
        onTokenChange={onTokenChange}
        onUseRelay={onUseRelay}
        palette={palette}
        onPalette={setPalette}
      />
    );

  return (
    <ThemeProvider palette={palette}>
      {content}
      <StatusBar style="auto" />
    </ThemeProvider>
  );
}
