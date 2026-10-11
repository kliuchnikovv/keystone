import { useEffect, useRef, useState } from 'react';
import { ActivityIndicator, KeyboardAvoidingView, Platform, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useDiscovery } from '../hooks/useDiscovery';
import { describeSetupFailure, generateToken, normalizeSetupCode, setupCodeProblem } from '../model/setup';
import { signalText, wifiRequest } from '../model/automation';
import { hubUrl } from '../native/discovery';
import { keystoneNet } from '../native/module';
import { chooseSocketFactory } from '../native/platform';
import type { SocketFactory } from '../protocol/client';
import type { WifiNetwork } from '../protocol/messages';
import { SETUP_URL, SetupClient, SetupError } from '../protocol/setup';
import type { Settings } from '../settings';
import { Button, ErrorText, Input } from './controls';
import { useTheme } from './theme';

/** Where the hub will be once it is on the home network, if discovery does not find it first. */
const FALLBACK_URL = 'wss://keystone.local:8443/ws';

export interface SetupScreenProps {
  /** Back to the connect screen. */
  onCancel(): void;
  /** The hub is on its way onto the home network: connect to it with these settings. */
  onDone(settings: Settings): void;
  /** Tests give a socket that needs no native module. */
  socketFactory?: SocketFactory;
  /** Where the hub's access point puts it (default 192.168.4.1). */
  url?: string;
}

type Step = 'code' | 'connecting' | 'network' | 'applying' | 'finish';

/**
 * First-time setup of a hub that has no network. The phone joins the hub's own Wi-Fi (`Keystone-XXXX`, password = the
 * setup code on its label), this screen signs in with the same code, lets the person pick the home network and sends
 * it, together with a token the app invents for the hub. Then the phone goes back to the home Wi-Fi and finds the hub.
 */
export function SetupScreen({ onCancel, onDone, socketFactory, url }: SetupScreenProps) {
  const t = useTheme();
  const [step, setStep] = useState<Step>('code');
  const [code, setCode] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [networks, setNetworks] = useState<WifiNetwork[] | null>(null);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [result, setResult] = useState<{ ssid: string; settings: Settings } | null>(null);
  const client = useRef<SetupClient | null>(null);

  useEffect(() => () => client.current?.close(), []);

  const scan = async () => {
    const c = client.current;
    if (!c) return;
    setScanning(true);
    try {
      setNetworks(await c.scan());
    } catch (e) {
      setError(e instanceof SetupError ? describeSetupFailure(e.kind, e.message) : 'The scan failed.');
    } finally {
      setScanning(false);
    }
  };

  const connect = async () => {
    const problem = setupCodeProblem(code);
    if (problem) return setError(problem);
    setError(null);
    setStep('connecting');
    client.current?.close();
    const c = new SetupClient({ url: url ?? SETUP_URL, code: normalizeSetupCode(code)!, socketFactory: socketFactory ?? chooseSocketFactory(keystoneNet()) });
    client.current = c;
    try {
      await c.connect();
    } catch (e) {
      setStep('code');
      return setError(e instanceof SetupError ? describeSetupFailure(e.kind, e.message) : 'Could not connect to the hub.');
    }
    setStep('network');
    void scan();
  };

  const apply = async () => {
    const r = wifiRequest(ssid, password);
    if (!r.ok) return setError(r.error);
    const c = client.current;
    if (!c) return;
    setError(null);
    setStep('applying');
    try {
      const token = await generateToken();
      await c.apply(ssid, password, token);
      // The hub is now switching networks: remember what is needed to find it again, and stop talking.
      const settings: Settings = { url: FALLBACK_URL, token, ...(c.fingerprint ? { fingerprint: c.fingerprint } : {}) };
      c.close();
      setResult({ ssid, settings });
      setStep('finish');
    } catch (e) {
      setStep('network');
      setError(e instanceof SetupError ? describeSetupFailure(e.kind, e.message) : 'The hub did not take the settings.');
    }
  };

  return (
    <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : undefined} style={[styles.screen, { backgroundColor: t.bg }]}>
      <ScrollView contentContainerStyle={styles.form} keyboardShouldPersistTaps="handled">
        <Text style={[styles.h1, { color: t.text }]}>Set up a new hub</Text>

        {step === 'code' && (
          <>
            <Text style={[styles.p, { color: t.muted }]}>
              1. Plug the hub in. After a few seconds it opens its own Wi-Fi network, <Text style={styles.mono}>Keystone-…</Text>.{'\n'}
              2. In your phone’s Wi-Fi settings, join it. Its password is the setup code on the hub’s label.{'\n'}
              3. Come back here and type the same code. If the phone says the network has no internet, stay on it.
            </Text>
            <Input label="Setup code" value={code} onChangeText={setCode} placeholder="XXXX-XXXX-XXXX" autoCapitalize="characters" onSubmitEditing={connect} />
            <ErrorText text={error} />
            <View style={styles.row}>
              <Button kind="plain" label="Back" onPress={onCancel} />
              <Button label="Connect to the hub" onPress={connect} />
            </View>
          </>
        )}

        {step === 'connecting' && <Busy text="Connecting to the hub…" />}

        {step === 'network' && (
          <>
            <Text style={[styles.p, { color: t.muted }]}>
              Connected. Choose your home Wi-Fi: the hub will join it and use it from now on.
            </Text>
            <Button kind="plain" label={scanning ? 'Scanning…' : 'Scan again'} onPress={scan} disabled={scanning} />
            {networks?.length === 0 && <Text style={{ color: t.muted }}>No networks found. Type the name below.</Text>}
            <View style={{ gap: 8, marginTop: 6 }}>
              {networks?.map((n) => (
                <Pressable
                  key={n.ssid}
                  accessibilityRole="button"
                  accessibilityLabel={`Network ${n.ssid}`}
                  onPress={() => {
                    setSsid(n.ssid);
                    if (!n.secure) setPassword('');
                  }}
                  style={[styles.net, { borderColor: ssid === n.ssid ? t.accent : t.border, backgroundColor: t.card }]}
                >
                  <Text style={{ color: t.text, fontWeight: '600' }}>{n.ssid}</Text>
                  <Text style={{ color: t.muted, fontSize: 12 }}>
                    {signalText(n.rssi)} · {n.secure ? 'password' : 'open'}
                  </Text>
                </Pressable>
              ))}
            </View>
            <View style={{ gap: 8, marginTop: 12 }}>
              <Input label="Network name" value={ssid} onChangeText={setSsid} placeholder="network name (type it for a hidden one)" />
              <Input label="Wi-Fi password" value={password} onChangeText={setPassword} placeholder="leave empty for an open network" secureTextEntry />
            </View>
            <ErrorText text={error} />
            <Text style={[styles.hint, { color: t.muted }]}>
              The hub cannot tell whether the password is right until it tries, a moment after you tap. Check it carefully.
            </Text>
            <View style={styles.row}>
              <Button kind="plain" label="Back" onPress={() => { client.current?.close(); setStep('code'); setError(null); }} />
              <Button label="Set up the hub" onPress={apply} />
            </View>
          </>
        )}

        {step === 'applying' && <Busy text="Sending the settings to the hub…" />}

        {step === 'finish' && result && <Finish ssid={result.ssid} settings={result.settings} onDone={onDone} onBack={onCancel} />}
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

function Busy({ text }: { text: string }) {
  const t = useTheme();
  return (
    <View accessibilityLabel={text} style={{ alignItems: 'center', gap: 12, marginTop: 24 }}>
      <ActivityIndicator />
      <Text style={{ color: t.muted }}>{text}</Text>
    </View>
  );
}

/** The hub is joining the home network; the phone has to go back to it too. Browsing finds the hub once both are there. */
function Finish({ ssid, settings, onDone, onBack }: { ssid: string; settings: Settings; onDone(s: Settings): void; onBack(): void }) {
  const t = useTheme();
  const discovery = useDiscovery();
  const found = discovery.hubs.find((h) => h.tls) ?? null;
  const go = () => onDone({ ...settings, url: found ? hubUrl(found) : settings.url });
  return (
    <>
      <Text style={[styles.p, { color: t.text, fontWeight: '600' }]}>The hub is joining {ssid}.</Text>
      <Text style={[styles.p, { color: t.muted }]}>
        Now put your phone back on your home Wi-Fi (leave <Text style={styles.mono}>Keystone-…</Text>). The hub needs about half a minute.
        {discovery.available ? (found ? ' The hub has been found on your network.' : ' Looking for it…') : ''}
      </Text>
      <Button label={found ? 'Connect to the hub' : 'Connect'} onPress={go} />
      <Text style={[styles.hint, { color: t.muted }]}>
        If it does not appear, the Wi-Fi password may have been wrong. The hub then opens its Wi-Fi again: start over from the connect screen.
      </Text>
      <View style={styles.row}>
        <Button kind="plain" label="Back to the connect screen" onPress={onBack} />
      </View>
    </>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  form: { padding: 24, maxWidth: 480, width: '100%', alignSelf: 'center', paddingTop: 48 },
  h1: { fontSize: 26, fontWeight: '700', marginBottom: 8 },
  p: { fontSize: 15, marginBottom: 16, lineHeight: 22 },
  mono: { fontFamily: Platform.select({ ios: 'Menlo', android: 'monospace', default: 'monospace' }) },
  hint: { fontSize: 12, marginTop: 10, lineHeight: 17 },
  row: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 18 },
  net: { borderWidth: 1, borderRadius: 10, paddingHorizontal: 12, paddingVertical: 9 },
});
