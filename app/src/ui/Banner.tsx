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
    case 'closed':
      return { text: 'Disconnected', tone: 'bad' };
  }
}

export function Banner({ state }: { state: HomeState }) {
  const t = useTheme();
  const n = connectionNotice(state);
  if (!n) return null;
  return (
    <View accessibilityRole="alert" style={[styles.banner, { backgroundColor: n.tone === 'bad' ? t.bad : t.warn }]}>
      <Text style={styles.text}>{n.text}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  banner: { paddingVertical: 8, paddingHorizontal: 14 },
  text: { color: '#fff', fontWeight: '600', textAlign: 'center' },
});
