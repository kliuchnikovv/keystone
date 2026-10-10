import { StyleSheet, Text, View } from 'react-native';
import type { HomeState } from '../protocol/store';
import { useTheme } from './theme';

/** A strip explaining why the data on screen may be missing or old. null when all is well. */
export function connectionNotice(s: HomeState): { text: string; tone: 'warn' | 'bad' } | null {
  switch (s.status) {
    case 'ready':
      return null;
    case 'idle':
    case 'connecting':
    case 'authenticating':
      return { text: 'Connecting to the hub…', tone: 'warn' };
    case 'reconnecting':
      return { text: s.error === 'busy' ? 'The hub has too many clients; retrying…' : 'Connection lost; reconnecting…', tone: 'warn' };
    case 'unauthorized':
      return { text: 'The hub rejected the token', tone: 'bad' };
    case 'incompatible':
      return { text: 'This app and the hub speak different protocol versions', tone: 'bad' };
    case 'untrusted':
      return {
        text:
          s.error === 'pin_unsupported'
            ? "This build can't verify the hub's certificate fingerprint. Clear the fingerprint, or use a build with certificate pinning."
            : "The hub's certificate is not the one you trusted. Someone may be impersonating the hub, or its certificate was replaced.",
        tone: 'bad',
      };
    case 'closed':
      return { text: 'Disconnected', tone: 'bad' };
  }
}

/** For a wss:// hub: what to try when it will not connect (a browser hides why TLS failed). */
export function secureHint(url: string, s: HomeState): string | null {
  if (!/^wss:\/\//i.test(url)) return null;
  if (s.status !== 'connecting' && s.status !== 'reconnecting' && s.status !== 'authenticating') return null;
  const host = url.replace(/^wss:\/\//i, '').replace(/\/.*$/, '');
  return `Not connecting? In a browser, open https://${host}/ once and accept the hub's certificate, then come back.`;
}

export function Banner({ state, url }: { state: HomeState; url?: string }) {
  const t = useTheme();
  const n = connectionNotice(state);
  if (!n) return null;
  const hint = url ? secureHint(url, state) : null;
  return (
    <View accessibilityRole="alert" style={[styles.banner, { backgroundColor: n.tone === 'bad' ? t.bad : t.warn }]}>
      <Text style={styles.text}>{n.text}</Text>
      {hint ? <Text style={styles.hint}>{hint}</Text> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  banner: { paddingVertical: 8, paddingHorizontal: 14 },
  text: { color: '#fff', fontWeight: '600', textAlign: 'center' },
  hint: { color: '#fff', textAlign: 'center', fontSize: 12, marginTop: 3 },
});
