// The way in: find the hub, prove it is yours, remember it.
// Same language as the room mock — panes of glass, warm accents, quiet labels.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import {
  ActivityIndicator,
  KeyboardAvoidingView,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import type { Discovery } from '../hooks/useDiscovery';
import { hubUrl } from '../native/discovery';
import { normalizeFingerprint } from '../protocol/fingerprint';
import {
  cloudUrl,
  isCloudUrl,
  isPlaintext,
  isRelayUrl,
  isSecure,
  normalizeUrl,
  validateSettings,
  type Settings,
} from '../settings';
import { Chip, ChipRow } from './controls';
import { Glyph, type GlyphName } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { radius, space, useTheme } from './theme';

type Mode = 'local' | 'relay' | 'cloud';

/** What stopped the connection. The screen turns each into words under the form. */
export type ConnectFailure = 'token' | 'certificate' | 'pin' | 'unreachable' | 'version';

const FAILURE_TEXT: Record<ConnectFailure, string> = {
  token: 'The hub rejected the token.',
  certificate: "This hub's certificate changed. Someone may be intercepting the connection.",
  pin: "This browser can't pin a certificate. Use the mobile app.",
  unreachable: "Can't reach the hub. Check Wi-Fi.",
  version: 'This app and the hub speak different protocol versions.',
};

function initialMode(url: string): Mode {
  if (isCloudUrl(url)) return 'cloud';
  if (isRelayUrl(url)) return 'relay';
  return 'local';
}

export interface ConnectScreenProps {
  initial?: Settings | null;
  notice?: string | null;
  /** The app is trying to reach the hub: the form is locked and the button spins. */
  connecting?: boolean;
  /** What went wrong; null while nothing is wrong. */
  failure?: ConnectFailure | null;
  /** Try the same values again (the hub was unreachable — maybe the Wi-Fi came back). */
  onRetry?(): void;
  /** Leave the screen (there was a hub before: the person may want to go back to it). */
  onCancel?(): void;
  onSave(s: Settings): void;
  discovery?: Discovery;
  onSetup?(): void;
}

export function ConnectScreen({
  initial,
  notice,
  connecting = false,
  failure = null,
  onRetry,
  onCancel,
  onSave,
  discovery,
  onSetup,
}: ConnectScreenProps) {
  const t = useTheme();
  const [url, setUrl] = useState(initial?.url ?? '');
  const [token, setToken] = useState(initial?.token ?? '');
  const [fingerprint, setFingerprint] = useState(initial?.fingerprint ?? '');
  const [touched, setTouched] = useState(false);
  const [showToken, setShowToken] = useState(false);
  const [mode, setMode] = useState<Mode>(() => initialMode(initial?.url ?? ''));
  const tokenRef = useRef<TextInput>(null);

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
    ? (!isCloudUrl(candidate.url) ? ('url' as const) : candidate.token.length < 8 ? ('token' as const) : null)
    : validateSettings(candidate);
  // The Connect button is alive only when there is something to connect to.
  const ready = problem === null;
  const submit = () => {
    setTouched(true);
    if (ready && !connecting) onSave(candidate);
  };

  // A rejected token: put the cursor where the fix belongs.
  useEffect(() => {
    if (failure === 'token') tokenRef.current?.focus();
  }, [failure]);

  const input = [styles.input, { color: t.text, backgroundColor: t.bg, borderColor: t.border }];
  const muted = connecting ? 0.5 : 1;

  return (
    <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : undefined} style={[styles.screen, { backgroundColor: t.bg }]}>
      <ScreenHeader
        title="Connect"
        left={
          onCancel ? (
            <Pressable accessibilityRole="button" accessibilityLabel="Back" onPress={onCancel} style={styles.back} hitSlop={8}>
              <Glyph name="back" size={22} color={t.accent} />
            </Pressable>
          ) : undefined
        }
      />
      <ScrollView contentContainerStyle={styles.page} keyboardShouldPersistTaps="handled" showsVerticalScrollIndicator={false}>
        <View style={[styles.form, { opacity: muted }]}>
          <Text style={[styles.hero, { color: t.text }]}>Connect to your hub</Text>
          <Text style={[styles.lead, { color: t.muted }]}>
            Enter the hub's address and the token you set on its console with <Text style={styles.mono}>webtoken</Text>.
          </Text>
          {notice ? <Text style={[styles.notice, { color: t.bad }]}>{notice}</Text> : null}

          {mode === 'local' && discovery?.available && (
            <Card title="Hubs on this network" icon="home" a11y="Hubs on this network">
              {discovery.hubs.length === 0 && (
                <View style={styles.searching}>
                  <ActivityIndicator size="small" color={t.accent} />
                  <Text style={[styles.hint, { color: t.muted }]}>Searching your network…</Text>
                </View>
              )}
              {discovery.hubs.map((h) => (
                <Pressable
                  key={h.instance}
                  accessibilityRole="button"
                  accessibilityLabel={`Use hub ${h.name ?? h.instance}`}
                  onPress={() => {
                    setUrl(hubUrl(h));
                    if (!h.tls) setFingerprint('');
                  }}
                  style={[styles.hub, { backgroundColor: t.fill, borderColor: t.border }]}
                >
                  <Glyph name="hub" size={20} color={t.accent} />
                  <View style={styles.hubBody}>
                    <Text style={[styles.hubName, { color: t.text }]} numberOfLines={1}>
                      {h.name ?? h.instance}
                    </Text>
                    <Text style={[styles.hubMeta, { color: t.muted }]} numberOfLines={1}>
                      {h.address ?? h.host}:{h.port} · {h.tls ? 'encrypted' : 'not encrypted'}
                    </Text>
                  </View>
                </Pressable>
              ))}
              <Text style={[styles.hint, { color: t.muted }]}>
                Finding a hub does not prove it is yours: for an encrypted hub, still enter the fingerprint the hub console prints.
              </Text>
            </Card>
          )}

          <Card title="Connection" icon="hub">
            <ChipRow>
              {(['local', 'cloud', 'relay'] as const).map((m) => (
                <Chip
                  key={m}
                  label={m === 'local' ? 'Local' : m === 'cloud' ? 'Cloud' : 'Relay'}
                  selected={mode === m}
                  onPress={() => setMode(m)}
                />
              ))}
            </ChipRow>

            {isCloud ? (
              <>
                <Field label="Cloud server">
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
                    editable={!connecting}
                  />
                </Field>
                <Field
                  label="Hub ID"
                  error={problem === 'url' && (touched || cloudServer !== '') ? 'Enter a cloud server address and hub ID' : null}
                >
                  <TextInput
                    accessibilityLabel="Hub ID"
                    style={input}
                    value={hubId}
                    onChangeText={setHubId}
                    placeholder="e.g. home-5ef894"
                    placeholderTextColor={t.muted}
                    autoCapitalize="none"
                    autoCorrect={false}
                    editable={!connecting}
                  />
                </Field>
                <Text style={[styles.hint, { color: t.muted }]}>
                  The hub connects to this cloud server automatically. Set it on the hub console with{' '}
                  <Text style={styles.mono}>relay {'<url> <secret>'}</Text>.{'\n'}
                  The Hub ID is shown by <Text style={styles.mono}>relay</Text> on the console.
                </Text>
              </>
            ) : (
              <Field
                label={isRelay ? 'Relay URL' : 'Hub address'}
                error={
                  problem === 'url' && (touched || url !== '')
                    ? isRelay
                      ? 'Enter the relay URL like wss://relay.example.com/client/my-hub'
                      : 'Enter an address like 192.168.0.202 or ws://hub:8080/ws'
                    : null
                }
              >
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
                  editable={!connecting}
                />
                {isRelay && (
                  <Text style={[styles.hint, { color: t.muted }]}>
                    Run a relay server on a VPS, then set it on the hub console with{' '}
                    <Text style={styles.mono}>relay {'<url> <secret>'}</Text>.{'\n'}
                    The relay forwards traffic; authentication still uses the hub's token.
                  </Text>
                )}
              </Field>
            )}
          </Card>

          <Card title="Token" icon="chip">
            <View style={[styles.secretBox, { backgroundColor: t.bg, borderColor: t.border }]}>
              <TextInput
                ref={tokenRef}
                accessibilityLabel="Token"
                style={[styles.secret, { color: t.text }]}
                value={token}
                onChangeText={setToken}
                placeholder="at least 8 characters"
                placeholderTextColor={t.muted}
                autoCapitalize="none"
                autoCorrect={false}
                secureTextEntry={!showToken}
                editable={!connecting}
                onSubmitEditing={submit}
              />
              <Pressable
                accessibilityRole="button"
                accessibilityLabel={showToken ? 'Hide token' : 'Show token'}
                onPress={() => setShowToken((s) => !s)}
                style={styles.eye}
                hitSlop={8}
              >
                <Glyph name={showToken ? 'eyeOff' : 'eye'} size={18} color={t.muted} />
              </Pressable>
            </View>

            {problem === 'token' && (touched || token !== '') && (
              <Text accessibilityRole="alert" style={[styles.err, { color: t.bad }]}>
                The token is at least 8 characters
              </Text>
            )}
          </Card>

          {secure && mode === 'local' && (
            <Card title="Certificate fingerprint (SHA-256)" icon="signal">
              <TextInput
                accessibilityLabel="Certificate fingerprint"
                style={input}
                value={fingerprint}
                onChangeText={setFingerprint}
                placeholder="AA:BB:CC:… as shown by `webtoken` on the hub"
                placeholderTextColor={t.muted}
                autoCapitalize="characters"
                autoCorrect={false}
                editable={!connecting}
              />
              {problem === 'fingerprint' && (touched || fingerprint !== '') && (
                <Text accessibilityRole="alert" style={[styles.err, { color: t.bad }]}>
                  That is not a SHA-256 fingerprint (32 bytes of hex)
                </Text>
              )}
              <Text style={[styles.hint, { color: t.muted }]}>
                Needed when the hub uses its own certificate. Compare it with what the hub console prints. On the web, leave it empty and accept the certificate once in the browser instead.
              </Text>
            </Card>
          )}

          {candidate.url !== '' && isPlaintext(candidate.url) && mode === 'local' && (
            <Text style={[styles.warn, { color: t.warn }]}>
              This connection is not encrypted: use it on your own network only.
            </Text>
          )}

          {failure ? (
            <View style={[styles.alert, { backgroundColor: t.bad }]} accessibilityRole="alert">
              <Text style={styles.alertText}>{FAILURE_TEXT[failure]}</Text>
              {failure === 'unreachable' && onRetry ? (
                <Pressable accessibilityRole="button" accessibilityLabel="Retry" onPress={onRetry} style={styles.alertButton}>
                  <Text style={styles.alertButtonText}>Retry</Text>
                </Pressable>
              ) : null}
            </View>
          ) : null}
        </View>

        <Pressable
          accessibilityRole="button"
          accessibilityLabel="Connect"
          accessibilityState={{ disabled: !ready || connecting }}
          disabled={!ready || connecting}
          onPress={submit}
          style={[styles.button, { backgroundColor: t.accent, opacity: ready && !connecting ? 1 : 0.5 }]}
        >
          {connecting ? <ActivityIndicator size="small" color={t.onAccent} /> : null}
          <Text style={[styles.buttonText, { color: t.onAccent }]}>{connecting ? 'Connecting…' : 'Connect'}</Text>
        </Pressable>

        {onSetup && (
          <Pressable accessibilityRole="button" accessibilityLabel="Set up a new hub" onPress={onSetup} style={styles.setup}>
            <Text style={[styles.setupText, { color: t.accent }]}>Set up a new hub</Text>
            <Text style={[styles.setupHint, { color: t.muted }]}>For a hub that is not on your Wi-Fi yet</Text>
          </Pressable>
        )}
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

/** A pane of glass with a small title — the room mock's card. */
function Card({ title, icon, children, a11y }: { title: string; icon: GlyphName; children: ReactNode; a11y?: string }) {
  const t = useTheme();
  return (
    <View
      accessibilityLabel={a11y}
      style={[styles.card, { backgroundColor: t.glass, borderColor: t.glassBorder, borderRadius: t.cardRadius }]}
    >
      <View style={styles.cardHead}>
        <Glyph name={icon} size={15} color={t.muted} />
        <Text style={[styles.cardTitle, { color: t.muted }]}>{title}</Text>
      </View>
      {children}
    </View>
  );
}

/** A labelled field: the name in small capitals, the box, and what went wrong. */
function Field({ label, error, children }: { label: string; error?: string | null; children: ReactNode }) {
  const t = useTheme();
  return (
    <View style={styles.field}>
      <Text style={[styles.fieldLabel, { color: t.muted }]}>{label}</Text>
      {children}
      {error ? (
        <Text accessibilityRole="alert" style={[styles.err, { color: t.bad }]}>
          {error}
        </Text>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  page: { padding: 16, paddingBottom: 40, maxWidth: 520, width: '100%', alignSelf: 'center' },
  form: { opacity: 1 },
  hero: { fontSize: 22, fontWeight: '700', letterSpacing: -0.02 },
  lead: { fontSize: 12.5, marginTop: 6, lineHeight: 18 },
  mono: { fontFamily: Platform.select({ ios: 'Menlo', android: 'monospace', default: 'monospace' }) },
  card: { borderWidth: 1, padding: 16, marginTop: space.md },
  cardHead: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: space.md },
  cardTitle: { fontSize: 11, fontWeight: '700', letterSpacing: 1, textTransform: 'uppercase', flexShrink: 1 },
  field: { marginTop: space.sm },
  fieldLabel: { fontSize: 10.5, fontWeight: '700', letterSpacing: 1, textTransform: 'uppercase', marginBottom: 5 },
  input: { borderWidth: 1, borderRadius: radius.field, paddingHorizontal: 14, paddingVertical: 11, fontSize: 12.5 },
  secretBox: { flexDirection: 'row', alignItems: 'center', borderWidth: 1, borderRadius: radius.field, paddingRight: 6 },
  secret: { flex: 1, paddingHorizontal: 14, paddingVertical: 11, fontSize: 12.5 },
  eye: { padding: 6 },
  err: { fontSize: 12, marginTop: 5 },
  warn: { fontSize: 12.5, marginTop: space.md },
  hint: { fontSize: 11.5, marginTop: 6, lineHeight: 17 },
  searching: { flexDirection: 'row', alignItems: 'center', gap: 8, marginVertical: 4 },
  hub: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    borderWidth: 1,
    borderRadius: 14,
    paddingHorizontal: 14,
    paddingVertical: 12,
    marginBottom: space.xs,
  },
  hubBody: { flex: 1 },
  hubName: { fontSize: 16, fontWeight: '600' },
  hubMeta: { fontSize: 12, fontWeight: '500', marginTop: 2 },
  notice: { fontSize: 12.5, marginTop: space.sm, fontWeight: '600' },
  alert: { borderRadius: 14, paddingVertical: 12, paddingHorizontal: 14, marginTop: space.md },
  alertText: { color: '#fff', fontSize: 13, fontWeight: '600', lineHeight: 18 },
  alertButton: {
    alignSelf: 'flex-start',
    marginTop: 8,
    paddingHorizontal: 12,
    paddingVertical: 5,
    borderRadius: radius.pill,
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.75)',
  },
  alertButtonText: { color: '#fff', fontWeight: '700', fontSize: 12.5 },
  button: {
    marginTop: space.lg,
    borderRadius: 14,
    paddingVertical: 14,
    paddingHorizontal: 18,
    alignItems: 'center',
    flexDirection: 'row',
    justifyContent: 'center',
    gap: 8,
  },
  buttonText: { fontSize: 15, fontWeight: '600' },
  setup: { marginTop: space.lg, alignItems: 'center', paddingVertical: 8 },
  setupText: { fontSize: 13, fontWeight: '600' },
  setupHint: { fontSize: 11, marginTop: 2 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center', borderRadius: radius.field },
});
