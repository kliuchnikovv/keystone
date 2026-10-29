import { Platform, Pressable, StyleSheet, Text, View } from 'react-native';
import { agoText } from '../model/devices';
import type { HomeState } from '../protocol/store';
import { isRelayUrl } from '../settings';
import { useTheme } from './theme';

/** What the banner offers: retry the connection, or go back to the Connect screen. */
export interface BannerAction {
  label: string;
  onPress(): void;
}

/** A strip explaining why the data on screen may be missing or old. null when all is well. */
export function connectionNotice(s: HomeState): { text: string; tone: 'warn' | 'bad' } | null {
  switch (s.status) {
    case 'ready':
      return null;
    case 'idle':
    case 'connecting':
    case 'authenticating':
      return { text: `Connecting…${staleNote(s)}`, tone: 'warn' };
    case 'reconnecting':
      return {
        text: `${s.error === 'busy' ? 'The hub has too many clients; retrying…' : 'Reconnecting after a drop'}${staleNote(s)}`,
        tone: 'warn',
      };
    case 'unauthorized':
      return { text: 'Authentication failed', tone: 'bad' };
    case 'incompatible':
      return { text: 'This app and the hub speak different protocol versions', tone: 'bad' };
    case 'untrusted':
      return {
        text:
          s.error === 'pin_unsupported'
            ? Platform.OS === 'web'
              ? "This browser can't pin a certificate. Use the mobile app."
              : "This build can't verify the hub's certificate fingerprint. Clear the fingerprint, or use a build with certificate pinning."
            : "This hub's certificate changed. Someone may be intercepting the connection.",
        tone: 'bad',
      };
    case 'closed':
      return { text: `Not connected${staleNote(s)}`, tone: 'bad' };
  }
}

/**
 * What the screen is showing when the hub is not answering: the last state we know,
 * and how old it is. Without this the numbers would look live, and they are not.
 */
function staleNote(s: HomeState): string {
  if (!s.stale) return '';
  const age = s.savedAt !== null ? agoText(s.savedAt, Date.now()) : null;
  return age ? ` — the last known state (${age})` : ' — the last known state';
}

/**
 * For a wss:// address: what to try when it will not connect.
 *
 * Three shapes, three answers:
 *  - a hub (`…/ws`): its certificate is self-signed, and a browser accepts it once;
 *  - a relay (`…/client/<hub-id>`): its certificate is signed by a public CA, so there
 *    is nothing to accept - and the address has to be the client side of the tunnel;
 *  - anything else: usually the relay's own home page, which is what a person copies
 *    off the status page, and it is not an address the app can talk to.
 */
export function secureHint(url: string, s: HomeState): string | null {
  if (!/^wss:\/\//i.test(url)) return null;
  if (s.status !== 'connecting' && s.status !== 'reconnecting' && s.status !== 'authenticating') return null;
  const rest = url.replace(/^wss:\/\//i, '');
  const host = rest.replace(/\/.*$/, '');
  if (isRelayUrl(url)) {
    return "A relay's certificate is signed by a public CA - there is nothing to accept in a browser. The app talks to the client side of the tunnel: …/client/<hub-id>.";
  }
  if (/\/ws\s*$/i.test(rest)) {
    return `Not connecting? In a browser, open https://${host}/ once and accept the hub's certificate, then come back.`;
  }
  return `This address is neither a hub (…/ws) nor a relay (…/client/<hub-id>). If it is a relay, use the client side of the tunnel - the relay's status page says which address that is.`;
}

export function Banner({ state, url, action }: { state: HomeState; url?: string; action?: BannerAction | null }) {
  const t = useTheme();
  const n = connectionNotice(state);
  if (!n) return null;
  const hint = url ? secureHint(url, state) : null;
  return (
    <View accessibilityRole="alert" style={[styles.banner, { backgroundColor: n.tone === 'bad' ? t.bad : t.warn }]}>
      <Text style={styles.text}>{n.text}</Text>
      {hint ? <Text style={styles.hint}>{hint}</Text> : null}
      {action ? (
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={action.label}
          onPress={action.onPress}
          style={styles.action}
        >
          <Text style={styles.actionText}>{action.label}</Text>
        </Pressable>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  banner: { paddingVertical: 8, paddingHorizontal: 14 },
  text: { color: '#fff', fontWeight: '600', textAlign: 'center' },
  hint: { color: '#fff', textAlign: 'center', fontSize: 12, marginTop: 3 },
  action: { alignSelf: 'center', marginTop: 8, paddingHorizontal: 14, paddingVertical: 6, borderRadius: 999, borderWidth: 1, borderColor: '#fff' },
  actionText: { color: '#fff', fontWeight: '600', fontSize: 13 },
});
