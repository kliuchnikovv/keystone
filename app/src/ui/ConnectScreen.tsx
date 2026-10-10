import { useState } from 'react';
import { KeyboardAvoidingView, Platform, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import { normalizeFingerprint } from '../protocol/fingerprint';
import { isPlaintext, isSecure, normalizeUrl, validateSettings, type Settings } from '../settings';
import { useTheme } from './theme';

export interface ConnectScreenProps {
  initial?: Settings | null;
  /** Shown above the form, e.g. "The hub rejected the token". */
  notice?: string | null;
  onSave(s: Settings): void;
}

export function ConnectScreen({ initial, notice, onSave }: ConnectScreenProps) {
  const t = useTheme();
  const [url, setUrl] = useState(initial?.url ?? '');
  const [token, setToken] = useState(initial?.token ?? '');
  const [fingerprint, setFingerprint] = useState(initial?.fingerprint ?? '');
  const [touched, setTouched] = useState(false);

  const secure = isSecure(url);
  const candidate: Settings = {
    url: url.trim() === '' ? '' : normalizeUrl(url),
    token,
    // Only a wss:// hub has a certificate to pin; an empty field means "rely on the platform's trust".
    ...(secure && fingerprint.trim() !== '' ? { fingerprint: normalizeFingerprint(fingerprint) ?? fingerprint } : {}),
  };
  const problem = validateSettings(candidate);
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

        <Text style={[styles.label, { color: t.muted }]}>Hub address</Text>
        <TextInput
          accessibilityLabel="Hub address"
          style={input}
          value={url}
          onChangeText={setUrl}
          placeholder="192.168.0.202 or ws://hub:8080/ws"
          placeholderTextColor={t.muted}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
        />
        {touched && problem === 'url' && <Text style={[styles.err, { color: t.bad }]}>Enter an address like 192.168.0.202 or ws://hub:8080/ws</Text>}

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

        {secure && (
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

        {candidate.url !== '' && isPlaintext(candidate.url) && (
          <Text style={[styles.warn, { color: t.warn }]}>
            This connection is not encrypted: use it on your own network only.
          </Text>
        )}

        <Pressable accessibilityRole="button" accessibilityLabel="Connect" onPress={submit} style={[styles.button, { backgroundColor: t.accent }]}>
          <Text style={[styles.buttonText, { color: t.onAccent }]}>Connect</Text>
        </Pressable>
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
  notice: { fontSize: 14, marginBottom: 4, fontWeight: '600' },
  button: { marginTop: 22, borderRadius: 10, paddingVertical: 13, alignItems: 'center' },
  buttonText: { fontSize: 16, fontWeight: '600' },
});
