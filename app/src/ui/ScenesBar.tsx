import { useEffect, useRef, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import type { HomeState } from '../protocol/store';
import { Chip, ChipRow } from './controls';
import { useTheme } from './theme';

/** One tap per scene, above the device list. Says how many settings went out (and how many could not). */
export function ScenesBar({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [note, setNote] = useState<string | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);
  if (home.scenes.length === 0) return null;

  const run = async (name: string) => {
    const res = await commands.request('scene.apply', { name });
    if (res === null) return; // the failure toast says why
    const sent = Number(res.sent ?? 0);
    const skipped = Number(res.skipped ?? 0);
    setNote(skipped > 0 ? `${name}: ${sent} applied, ${skipped} could not be (a device is gone or read-only)` : `${name} applied`);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => setNote(null), 4000);
  };

  return (
    <View style={styles.box} accessibilityLabel="Scenes">
      <Text style={[styles.label, { color: t.muted }]}>SCENES</Text>
      <ChipRow>
        {home.scenes.map((s) => (
          <Chip key={s.name} label={`▶ ${s.name}`} a11y={`Apply scene ${s.name}`} selected={false} onPress={() => void run(s.name)} />
        ))}
      </ChipRow>
      {note && <Text style={{ color: t.muted, fontSize: 12, marginTop: 6 }}>{note}</Text>}
    </View>
  );
}

const styles = StyleSheet.create({
  box: { marginBottom: 12 },
  label: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginBottom: 6 },
});
