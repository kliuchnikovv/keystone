import { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { describeWifi, signalText, wifiRequest } from '../model/automation';
import type { WifiNetwork } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import { Button, ErrorText, Input } from './controls';
import { radius, useTheme } from './theme';

/**
 * The hub's Wi-Fi: which network it is on, and a way to move it to another. The hub tries the new network
 * and goes back to the old one if it cannot join in about 30 s, so a mistyped password does not strand it;
 * while it switches, the app's connection drops and returns by itself.
 */
export function WifiCard({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  const [networks, setNetworks] = useState<WifiNetwork[] | null>(null);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [sent, setSent] = useState<string | null>(null);
  const current = home.hub?.wifi ?? null;

  const scan = async () => {
    setScanning(true);
    const res = await commands.request('wifi.scan');
    setScanning(false);
    if (res && Array.isArray(res.networks)) setNetworks(res.networks as WifiNetwork[]);
  };
  const connect = async () => {
    const r = wifiRequest(ssid, password);
    if (!r.ok) return setError(r.error);
    setError(null);
    if ((await commands.request('wifi.set', r.value)) !== null) {
      setSent(ssid);
      setOpen(false);
      setPassword('');
    }
  };

  return (
    <View accessibilityLabel="Wi-Fi" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <Text style={[styles.h2, { color: t.text }]}>Wi-Fi</Text>
      <Text style={{ color: t.muted }}>{describeWifi(home.hub)}</Text>
      {sent && !open && (
        <Text style={{ color: t.muted, fontSize: 12, marginTop: 4 }}>
          Switching to {sent}. The connection drops for a moment. If the hub cannot join in about 30 seconds it returns to
          {current ? ` ${current.ssid}` : ' its old network'}.
        </Text>
      )}
      {open ? (
        <View style={{ gap: 8, marginTop: 8 }}>
          <Button kind="plain" label={scanning ? 'Scanning…' : 'Scan for networks'} onPress={scan} disabled={scanning} />
          {networks?.length === 0 && <Text style={{ color: t.muted }}>No networks found.</Text>}
          {networks?.map((n) => (
            <Pressable
              key={n.ssid}
              accessibilityRole="button"
              accessibilityLabel={`Network ${n.ssid}`}
              onPress={() => {
                setSsid(n.ssid);
                if (!n.secure) setPassword('');
              }}
              style={[styles.net, { borderColor: ssid === n.ssid ? t.accent : t.border }]}
            >
              <Text style={{ color: t.text, fontWeight: '600' }}>{n.ssid}</Text>
              <Text style={{ color: t.muted, fontSize: 12 }}>
                {signalText(n.rssi)} · {n.secure ? 'password' : 'open'}
              </Text>
            </Pressable>
          ))}
          <Input label="Network name" value={ssid} onChangeText={setSsid} placeholder="network name (type it for a hidden one)" />
          <Input label="Wi-Fi password" value={password} onChangeText={setPassword} placeholder="leave empty for an open network" secureTextEntry />
          <ErrorText text={error} />
          <Text style={{ color: t.muted, fontSize: 12 }}>
            The hub keeps the new network only if it gets an address there within about 30 seconds; otherwise it goes back.
          </Text>
          <View style={styles.row}>
            <Button kind="secondary" label="Cancel" onPress={() => setOpen(false)} />
            <Button label="Connect" onPress={connect} />
          </View>
        </View>
      ) : (
        <View style={styles.row}>
          <Button kind="secondary" size="compact" label="Change network" onPress={() => { setError(null); setSent(null); setOpen(true); }} />
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 14, gap: 4 },
  h2: { fontSize: 17, fontWeight: '600' },
  row: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
  net: { borderWidth: 1, borderRadius: radius.field, paddingHorizontal: 14, paddingVertical: 10 },
});
