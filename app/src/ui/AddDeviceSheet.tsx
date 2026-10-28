// "Add a device": a bottom sheet that opens the Zigbee network on the hub for a
// couple of minutes and shows what joins meanwhile. The hub does not report the
// window itself (it is fire-and-forget), so the countdown is this sheet's own clock;
// new devices appear because the hub re-sends its list.
import { useEffect, useRef, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { deviceTitle, PAIR_SECONDS } from '../model/devices';
import type { HomeState } from '../protocol/store';
import { Button } from './controls';
import { Glyph } from './Glyph';
import { Sheet } from './Sheet';
import { useTheme } from './theme';

const clock = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

/** "5 s ago", "2 min ago" — how long ago a device joined. */
function ago(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return `${s} s ago`;
  return `${Math.floor(s / 60)} min ago`;
}

export function AddDeviceSheet({
  visible,
  onClose,
  home,
  commands,
}: {
  visible: boolean;
  onClose(): void;
  home: HomeState;
  commands: Commands;
}) {
  const t = useTheme();
  const [window, setWindow] = useState<{ until: number; known: Set<number> } | null>(null);
  const [opening, setOpening] = useState(false);
  const [failed, setFailed] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const joinedAt = useRef(new Map<number, number>());
  const alive = useRef(true);

  useEffect(() => () => { alive.current = false; }, []);
  useEffect(() => {
    if (!visible || !window) return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [visible, window]);

  // Closed: forget the old window, so the next open starts a fresh one.
  useEffect(() => {
    if (!visible) {
      setWindow(null);
      setFailed(false);
      joinedAt.current.clear();
    }
  }, [visible]);

  // Opening the sheet opens the network: that is what the person came for.
  useEffect(() => {
    if (!visible || window || opening) return;
    const known = new Set(home.entities.map((e) => e.id));
    setOpening(true);
    void (async () => {
      const ok = await commands.request('pair', { seconds: PAIR_SECONDS });
      if (!alive.current) return;
      setOpening(false);
      if (ok === null) {
        setFailed(true);
        return;
      }
      setNow(Date.now());
      setWindow({ until: Date.now() + PAIR_SECONDS * 1000, known });
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible]);

  const joined = home.entities.filter((e) => window !== null && !window.known.has(e.id));
  for (const e of joined) if (!joinedAt.current.has(e.id)) joinedAt.current.set(e.id, Date.now());
  const left = window ? Math.max(0, Math.ceil((window.until - now) / 1000)) : PAIR_SECONDS;
  const expired = window !== null && left === 0;

  return (
    <Sheet visible={visible} title="Add a device" onCancel={onClose}>
      {failed ? (
        <View style={styles.box}>
          <Text style={{ color: t.bad, fontSize: 14, fontWeight: '600' }}>The hub did not open the network.</Text>
          <Text style={{ color: t.muted, fontSize: 13, lineHeight: 19 }}>
            Check that the hub is awake and connected, then try again.
          </Text>
          <View style={styles.buttons}>
            <Button kind="secondary" size="compact" label="Cancel" onPress={onClose} />
            <Button label="Try again" onPress={() => setFailed(false)} />
          </View>
        </View>
      ) : !window ? (
        <View style={styles.box}>
          <Text style={{ color: t.muted, fontSize: 14, lineHeight: 20 }}>
            {opening ? 'Opening the Zigbee network for 2 minutes…' : 'Opening the Zigbee network…'}
          </Text>
        </View>
      ) : expired ? (
        <View style={styles.box}>
          <View style={styles.result}>
            <Glyph name={joined.length > 0 ? 'check' : 'signal'} size={22} color={joined.length > 0 ? t.good : t.muted} />
            <Text style={[styles.resultText, { color: t.text }]}>
              {joined.length > 0
                ? `${joined.length} ${joined.length === 1 ? 'device' : 'devices'} added`
                : 'No devices joined'}
            </Text>
          </View>
          <Button label="Close" onPress={onClose} />
        </View>
      ) : (
        <View style={styles.box}>
          <Text style={[styles.title, { color: t.text }]}>Press the pair button on your Zigbee device (or power-cycle it). You have 2 minutes.</Text>
          <View style={styles.clock}>
            <Glyph name="plus" size={14} color={t.accent} />
            <Text style={[styles.clockText, { color: t.text }]}>{clock(left)}</Text>
          </View>
          {joined.map((e) => (
            <View key={e.id} style={styles.joined}>
              <Glyph name="check" size={14} color={t.good} />
              <Text style={{ color: t.good, fontWeight: '600' }}>
                {deviceTitle(e)} joined, {ago(joinedAt.current.get(e.id) ?? now, now)}
              </Text>
            </View>
          ))}
          <View style={styles.buttons}>
            <Button kind="secondary" label="Cancel" onPress={onClose} />
          </View>
        </View>
      )}
    </Sheet>
  );
}

const styles = StyleSheet.create({
  box: { gap: 10 },
  title: { fontSize: 15, lineHeight: 21 },
  clock: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 2 },
  clockText: { fontSize: 15, fontWeight: '600' },
  joined: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  result: { flexDirection: 'row', alignItems: 'center', gap: 10, marginBottom: 6 },
  resultText: { fontSize: 16, fontWeight: '600' },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
});
