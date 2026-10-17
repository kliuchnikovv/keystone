import { useState } from 'react';
import { KeyboardAvoidingView, Platform, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import type { Discovery } from '../hooks/useDiscovery';
import { hubUrl } from '../native/discovery';
import { normalizeFingerprint } from '../protocol/fingerprint';
import { cloudUrl, isCloudUrl, isPlaintext, isRelayUrl, isSecure, normalizeUrl, validateSettings, type Settings } from '../settings';
import { useTheme } from './theme';

type Mode = 'local' | 'relay' | 'cloud';

function initialMode(url: string): Mode {
  if (isCloudUrl(url)) return 'cloud';
  if (isRelayUrl(url)) return 'relay';
  return 'local';
}

export interface ConnectScreenProps {
  initial?: Settings | null;
  notice?: string | null;
  onSave(s: Settings): void;
  discovery?: Discovery;
  onSetup?(): void;
}

export function ConnectScreen({ initial, notice, onSave, discovery, onSetup }: ConnectScreenProps) {
  const t = useTheme();
  const [url, setUrl] = useState(initial?.url ?? '');
  const [token, setToken] = useState(initial?.token ?? '');
  const [fingerprint, setFingerprint] = useState(initial?.fingerprint ?? '');
  const [touched, setTouched] = useState(false);
  const [mode, setMode] = useState<Mode>(() => initialMode(initial?.url ?? ''));

  // Cloud mode fields
  const [cloudServer, setCloudServer] = useState('');
  const [hubId, setHubId] = useState('');

  const secure = isSecure(url);
  const isCloud = mode === 'cloud';
  const isRelay = mode === 'relay';

  const candidate: Settings = isCloud
    ? { url: hubId.trim() && cloudServer.trim() ? cloudUrl(cloudServer, hubId) : '', token }
    : {
        url: url.trim() === '' ? '' : normalizeUrl(url),
        token,
        ...(secure && !isRelay && fingerprint.trim() !== '' ? { fingerprint: normalizeFingerprint(fingerprint) ?? fingerprint } : {}),
      };
  const problem = isCloud
    ? (!isCloudUrl(candidate.url) ? 'url' as const : candidate.token.length < 8 ? 'token' as const : null)
    : validateSettings(candidate);
  const submit = () => {
    setTouched(true);
    if (!problem) onSave(candidate);
  };
  const input = [styles.input, { color: t.text, backgroundColor: t.card, borderColor: t.border }];

  return (
    <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : undefined} style={[styles.screen, { backgroundColor: t.bg }]}>
      <View style={styles.form}>
        <Text style={[styles.h1, { color: t.text }]}>Connect to your hub</Text>
        <Text style={[styles.p, { color: t.muted }]}>
          Enter the hub's address and the token you set on its console with <Text style={styles.mono}>webtoken</Text>.
        </Text>
        {notice ? <Text style={[styles.notice, { color: t.bad }]}>{notice}</Text> : null}

        {mode === 'local' && discovery?.available && (
          <View accessibilityLabel="Hubs on this network" style={styles.found}>
            <Text style={[styles.label, { color: t.muted }]}>Hubs on this network</Text>
            {discovery.hubs.length === 0 && <Text style={[styles.hint, { color: t.muted }]}>Searching…</Text>}
            {discovery.hubs.map((h) => (
              <Pressable
                key={h.instance}
                accessibilityRole="button"
                accessibilityLabel={`Use hub ${h.name ?? h.instance}`}
                onPress={() => {
                  setUrl(hubUrl(h));
                  if (!h.tls) setFingerprint('');
                }}
                style={[styles.hub, { backgroundColor: t.card, borderColor: t.border }]}
              >
                <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }}>{h.name ?? h.instance}</Text>
                <Text style={{ color: t.muted, fontSize: 12 }}>
                  {h.address ?? h.host}:{h.port} · {h.tls ? 'encrypted' : 'not encrypted'}
                </Text>
              </Pressable>
            ))}
            <Text style={[styles.hint, { color: t.muted }]}>
              Finding a hub does not prove it is yours: for an encrypted hub, still enter the fingerprint the hub console prints.
            </Text>
          </View>
        )}

        <View style={styles.modeRow}>
          {(['local', 'cloud', 'relay'] as const).map((m) => (
            <Pressable key={m} accessibilityRole="button" onPress={() => setMode(m)} style={[styles.modeChip, mode === m && { backgroundColor: t.accent }]}>
              <Text style={[styles.modeText, { color: mode === m ? t.onAccent : t.muted }]}>
                {m === 'local' ? 'Local' : m === 'cloud' ? 'Cloud' : 'Relay'}
              </Text>
            </Pressable>
          ))}
        </View>

        {isCloud ? (
          <>
            <Text style={[styles.label, { color: t.muted }]}>Cloud server</Text>
            <TextInput
              accessibilityLabel="Cloud server"
              style={input}
              value={cloudServer}
              onChangeText={setCloudServer}
              placeholder="relay.example.com"
              placeholderTextColor={t.muted}
              autoCapitalize="none"
              autoCorrect={false}
              keyboardType="url"
            />
            <Text style={[styles.label, { color: t.muted }]}>Hub ID</Text>
            <TextInput
              accessibilityLabel="Hub ID"
              style={input}
              value={hubId}
              onChangeText={setHubId}
              placeholder="e.g. home-5ef894"
              placeholderTextColor={t.muted}
              autoCapitalize="none"
              autoCorrect={false}
            />
            {touched && problem === 'url' && (
              <Text style={[styles.err, { color: t.bad }]}>Enter a cloud server address and hub ID</Text>
            )}
            <Text style={[styles.hint, { color: t.muted }]}>
              The hub connects to this cloud server automatically. Set it on the hub console with{' '}
              <Text style={styles.mono}>relay {'<url> <secret>'}</Text>.{'\n'}
              The Hub ID is shown by <Text style={styles.mono}>relay</Text> on the console.
            </Text>
          </>
        ) : (
          <>
            <Text style={[styles.label, { color: t.muted }]}>{isRelay ? 'Relay URL' : 'Hub address'}</Text>
            <TextInput
              accessibilityLabel={isRelay ? 'Relay URL' : 'Hub address'}
              style={input}
              value={url}
              onChangeText={setUrl}
              placeholder={isRelay ? 'wss://relay.example.com/client/my-hub' : '192.168.0.202 or ws://hub:8080/ws'}
              placeholderTextColor={t.muted}
              autoCapitalize="none"
              autoCorrect={false}
              keyboardType="url"
            />
            {touched && problem === 'url' && (
              <Text style={[styles.err, { color: t.bad }]}>
                {isRelay ? 'Enter the relay URL like wss://relay.example.com/client/my-hub' : 'Enter an address like 192.168.0.202 or ws://hub:8080/ws'}
              </Text>
            )}
            {isRelay && (
              <Text style={[styles.hint, { color: t.muted }]}>
                Run a relay server on a VPS, then set it on the hub console with{' '}
                <Text style={styles.mono}>relay {'<url> <secret>'}</Text>.{'\n'}
                The relay forwards traffic; authentication still uses the hub's token.
              </Text>
            )}
          </>
        )}

        <Text style={[styles.label, { color: t.muted }]}>Token</Text>
        <TextInput
          accessibilityLabel="Token"
          style={input}
          value={token}
          onChangeText={setToken}
          placeholder="at least 8 characters"
          placeholderTextColor={t.muted}
          autoCapitalize="none"
          autoCorrect={false}
          secureTextEntry
          onSubmitEditing={submit}
        />
        {touched && problem === 'token' && <Text style={[styles.err, { color: t.bad }]}>The token is at least 8 characters</Text>}

        {secure && mode === 'local' && (
          <>
            <Text style={[styles.label, { color: t.muted }]}>Certificate fingerprint (SHA-256)</Text>
            <TextInput
              accessibilityLabel="Certificate fingerprint"
              style={input}
              value={fingerprint}
              onChangeText={setFingerprint}
              placeholder="AA:BB:CC:… as shown by `webtoken` on the hub"
              placeholderTextColor={t.muted}
              autoCapitalize="characters"
              autoCorrect={false}
            />
            {touched && problem === 'fingerprint' && <Text style={[styles.err, { color: t.bad }]}>That is not a SHA-256 fingerprint (32 bytes of hex)</Text>}
            <Text style={[styles.hint, { color: t.muted }]}>
              Needed when the hub uses its own certificate. Compare it with what the hub console prints. On the web, leave it empty and accept the certificate once in the browser instead.
            </Text>
          </>
        )}

        {candidate.url !== '' && isPlaintext(candidate.url) && mode === 'local' && (
          <Text style={[styles.warn, { color: t.warn }]}>
            This connection is not encrypted: use it on your own network only.
          </Text>
        )}

        <Pressable accessibilityRole="button" accessibilityLabel="Connect" onPress={submit} style={[styles.button, { backgroundColor: t.accent }]}>
          <Text style={[styles.buttonText, { color: t.onAccent }]}>Connect</Text>
        </Pressable>

        {onSetup && (
          <Pressable accessibilityRole="button" accessibilityLabel="Set up a new hub" onPress={onSetup} style={styles.setup}>
            <Text style={{ color: t.accent, fontSize: 15, fontWeight: '600' }}>Set up a new hub</Text>
            <Text style={{ color: t.muted, fontSize: 12, marginTop: 2 }}>For a hub that is not on your Wi-Fi yet</Text>
          </Pressable>
        )}
      </View>
    </KeyboardAvoidingView>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, justifyContent: 'center' },
  form: { padding: 24, maxWidth: 480, width: '100%', alignSelf: 'center' },
  h1: { fontSize: 26, fontWeight: '700', marginBottom: 8 },
  p: { fontSize: 15, marginBottom: 16, lineHeight: 21 },
  mono: { fontFamily: Platform.select({ ios: 'Menlo', android: 'monospace', default: 'monospace' }) },
  label: { fontSize: 13, marginTop: 12, marginBottom: 6 },
  input: { borderWidth: 1, borderRadius: 10, paddingHorizontal: 12, paddingVertical: 11, fontSize: 16 },
  err: { fontSize: 13, marginTop: 6 },
  warn: { fontSize: 13, marginTop: 12 },
  hint: { fontSize: 12, marginTop: 6, lineHeight: 17 },
  found: { marginTop: 4 },
  hub: { borderWidth: 1, borderRadius: 10, paddingHorizontal: 12, paddingVertical: 9, marginBottom: 6 },
  notice: { fontSize: 14, marginBottom: 4, fontWeight: '600' },
  modeRow: { flexDirection: 'row', gap: 8, marginBottom: 4, marginTop: 4 },
  modeChip: { paddingHorizontal: 14, paddingVertical: 7, borderRadius: 8 },
  modeText: { fontSize: 14, fontWeight: '600' },
  button: { marginTop: 22, borderRadius: 10, paddingVertical: 13, alignItems: 'center' },
  buttonText: { fontSize: 16, fontWeight: '600' },
  setup: { marginTop: 18, alignItems: 'center', paddingVertical: 8 },
});
