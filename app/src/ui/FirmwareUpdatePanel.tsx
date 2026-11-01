// "Update firmware": the hub fetches a vendor .ota file over HTTP(S), checks it against
// the SHA-256 and hands it to the Zigbee coordinator, which serves it to the device. The
// panel only asks for the file; the progress arrives from the hub as `device_ota` events.
//
// It lives in the device sheet, so it follows the sheet's language: a block under a
// hairline, quiet buttons, the same type sizes.
import { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { otaInputProblem, otaPhaseLabel, otaStatusLabel } from '../model/devices';
import type { DeviceOtaSession } from '../protocol/messages';
import { Button, ErrorText, Field, Input } from './controls';
import { space, useTheme } from './theme';

export interface FirmwareUpdatePanelProps {
  /** The device being updated (for the accessibility labels). */
  title: string;
  /** The last firmware session the hub reported for this device. */
  session: DeviceOtaSession | undefined;
  /** Ask the hub to start the update. Resolves true when the hub accepted it. */
  onStart(url: string, sha256: string): Promise<boolean>;
  /** Close the panel (the update keeps running). */
  onClose(): void;
}

export function FirmwareUpdatePanel({ title, session, onStart, onClose }: FirmwareUpdatePanelProps) {
  const t = useTheme();
  const [url, setUrl] = useState('');
  const [sha256, setSha256] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const start = async () => {
    const problem = otaInputProblem(url, sha256);
    if (problem) {
      setError(problem);
      return;
    }
    setError(null);
    setBusy(true);
    try {
      if (!(await onStart(url.trim(), sha256.trim().toLowerCase()))) {
        setError('The hub did not start the update.'); // the toast says why
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <View style={[styles.box, { borderTopColor: t.border }]} accessibilityLabel={`Firmware update ${title}`}>
      <Text style={[styles.title, { color: t.text }]}>Update the firmware</Text>
      <Text style={[styles.note, { color: t.muted }]}>
        The hub downloads the .ota file and hands it to the device. Keep the device on its own power
        while it updates.
      </Text>
      <Field label="Firmware file">
        <Input label="Firmware file URL" value={url} onChangeText={setUrl} placeholder="https://example.com/lamp.ota" />
      </Field>
      <Field label="SHA-256 of the file" hint="The checksum of the file, exactly as the vendor published it.">
        <Input label="SHA-256" value={sha256} onChangeText={setSha256} placeholder="e3b0c442…" />
      </Field>
      <ErrorText text={error} />
      {session && session.phase !== 'idle' && <OtaProgress session={session} />}
      <View style={styles.buttons}>
        <Button kind="plain" size="compact" label="Cancel" a11y="Cancel firmware update" onPress={onClose} />
        <Button size="compact" label="Start" disabled={busy} onPress={() => void start()} />
      </View>
    </View>
  );
}

/** How far the update has come, and what stopped it if something did. */
export function OtaProgress({ session }: { session: DeviceOtaSession }) {
  const t = useTheme();
  const bad = session.phase === 'failed' || session.status !== 'ok';
  return (
    <View style={styles.progress} accessibilityLabel="Firmware update progress">
      <Text style={{ color: bad ? t.bad : t.text, fontWeight: '600' }}>
        {otaPhaseLabel(session.phase)}
        {bad ? `: ${otaStatusLabel(session.status)}` : ''}
      </Text>
      <Text style={{ color: t.muted, fontSize: 12 }}>{session.percent} %</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  box: { marginTop: space.md, paddingTop: space.md, borderTopWidth: StyleSheet.hairlineWidth },
  title: { fontSize: 15, fontWeight: '600' },
  note: { fontSize: 13, lineHeight: 19, marginTop: space.xs },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: space.xs, marginTop: space.md, flexWrap: 'wrap' },
  progress: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: space.sm, gap: space.xs },
});
