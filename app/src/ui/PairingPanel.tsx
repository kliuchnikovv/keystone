import { useEffect, useRef, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { deviceTitle, PAIR_SECONDS } from '../model/devices';
import type { HomeState } from '../protocol/store';
import { Button } from './controls';
import { useTheme } from './theme';

const clock = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

/**
 * "Add a device": opens the Zigbee network on the hub for a couple of minutes and shows what
 * joins meanwhile. The hub does not report the window itself (it is fire-and-forget), so the
 * countdown is this screen's own clock; new devices appear because the hub re-sends its list.
 */
export function PairingPanel({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [window, setWindow] = useState<{ until: number; known: Set<number> } | null>(null);
  const [now, setNow] = useState(() => Date.now());
  const alive = useRef(true);
  useEffect(() => () => { alive.current = false; }, []);
  useEffect(() => {
    if (!window) return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [window]);

  const start = async () => {
    const known = new Set(home.entities.map((e) => e.id));
    if ((await commands.request('pair', { seconds: PAIR_SECONDS })) !== null && alive.current) {
      setNow(Date.now());
      setWindow({ until: Date.now() + PAIR_SECONDS * 1000, known });
    }
  };

  const box = [styles.box, { backgroundColor: t.card, borderColor: t.border }];
  if (!window) {
    return (
      <View style={box}>
        <Button label="Add a device" onPress={start} />
        <Text style={[styles.hint, { color: t.muted }]}>Opens the Zigbee network for {PAIR_SECONDS / 60} minutes.</Text>
      </View>
    );
  }
  const left = Math.max(0, Math.ceil((window.until - now) / 1000));
  const joined = home.entities.filter((e) => !window.known.has(e.id));
  return (
    <View style={box} accessibilityLabel="Pairing">
      {left > 0 ? (
        <>
          <Text style={[styles.title, { color: t.text }]}>Looking for devices… {clock(left)}</Text>
          <Text style={[styles.hint, { color: t.muted }]}>
            Put the device into pairing mode now (usually: hold its button, or switch it on and off a few times).
          </Text>
        </>
      ) : (
        <Text style={[styles.title, { color: t.text }]}>The network is closed again.</Text>
      )}
      {joined.map((e) => (
        <Text key={e.id} style={{ color: t.good, fontWeight: '600' }}>
          ✓ Found {deviceTitle(e)}
        </Text>
      ))}
      {left === 0 && joined.length === 0 && <Text style={{ color: t.muted }}>Nothing joined.</Text>}
      <View style={styles.buttons}>
        {left === 0 && <Button label="Try again" onPress={start} />}
        <Button kind="plain" label="Done" onPress={() => setWindow(null)} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  box: { borderRadius: 14, borderWidth: 1, padding: 14, marginBottom: 12, gap: 6 },
  title: { fontSize: 16, fontWeight: '600' },
  hint: { fontSize: 12, lineHeight: 17 },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
});
