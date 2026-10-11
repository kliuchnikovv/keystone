import { StatusBar } from 'expo-status-bar';
import { useEffect, useMemo, useState, type ComponentProps } from 'react';
import { ActivityIndicator, View } from 'react-native';
import { useDiscovery } from './src/hooks/useDiscovery';
import { createSettingsStore, type Settings } from './src/settings';
import { defaultStore } from './src/storage';
import { ConnectScreen } from './src/ui/ConnectScreen';
import { Home } from './src/ui/Home';

// mDNS browsing runs only while this screen is up.
function ConnectWithDiscovery(props: Omit<ComponentProps<typeof ConnectScreen>, 'discovery'>) {
  return <ConnectScreen {...props} discovery={useDiscovery()} />;
}

export default function App() {
  const store = useMemo(() => createSettingsStore(defaultStore), []);
  // undefined = still reading the saved settings, null = none (ask for them).
  const [settings, setSettings] = useState<Settings | null | undefined>(undefined);
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    void store.load().then(setSettings);
  }, [store]);

  const content =
    settings === undefined ? (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
        <ActivityIndicator />
      </View>
    ) : settings === null ? (
      <ConnectWithDiscovery
        notice={notice}
        onSave={(s) => {
          setNotice(null);
          void store.save(s).then(() => setSettings(s));
        }}
      />
    ) : (
      <Home
        key={`${settings.url}|${settings.token}|${settings.fingerprint ?? ''}`}
        settings={settings}
        onChangeHub={() => {
          setNotice(null);
          setSettings(null);
        }}
      />
    );

  return (
    <>
      {content}
      <StatusBar style="auto" />
    </>
  );
}
