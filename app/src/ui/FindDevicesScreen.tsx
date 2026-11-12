// "Find devices": what is around the phone, and what the hub is opening right now.
//
// Two halves, because two different radios are involved. Bluetooth says what is near
// the phone — only a device that advertises over Bluetooth can be placed, so a Zigbee
// device without Bluetooth never shows up here. The hub's own network says what can
// join the home.
import { useEffect, useRef, useState } from 'react';
import { Platform, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { useBleScan } from '../hooks/useBleScan';
import { useJoinWindow } from '../hooks/useJoinWindow';
import type { BleDevice, MatterDevice } from '../model/bleDevices';
import { matterDevices } from '../model/bleDevices';
import { deviceTitle } from '../model/devices';
import { matterBusy, matterWords, type MatterIdentity } from '../model/matter';
import { proximityLabel, tooFar } from '../model/proximity';
import type { Entity, MatterEvent } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import { Button, Input, Note } from './controls';
import { Glyph } from './Glyph';
import { ScanQrScreen } from './ScanQrScreen';
import { ScreenHeader } from './ScreenHeader';
import { radius, useTheme } from './theme';

const clock = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

/** "5 s ago", "2 min ago" — how long ago a device joined. */
function ago(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return `${s} s ago`;
  return `${Math.floor(s / 60)} min ago`;
}

export function FindDevicesScreen({
  home,
  commands,
  onClose,
}: {
  home: HomeState;
  commands: Commands;
  onClose(): void;
}) {
  const t = useTheme();
  const scan = useBleScan(true);
  const join = useJoinWindow(true, home, commands);
  const attempt = home.matter;
  // One Matter device at a time: the hub commissions them one by one.
  const busy = attempt !== null && matterBusy(attempt);
  // The hub told us it cannot do it (its Matter chip has no commissioner) — do not offer it.
  const [refused, setRefused] = useState(false);
  const commandsRef = useRef(commands);
  commandsRef.current = commands;
  useEffect(() => {
    // The last attempt is worth asking about: after a restart of the app the hub still
    // knows that its Matter chip cannot commission anything.
    let alive = true;
    void commandsRef.current.request('matter.status').then((res) => {
      if (alive && res !== null && res.code === 'unsupported') setRefused(true);
    });
    return () => {
      alive = false;
    };
  }, []);
  const hubCannot = refused || (attempt !== null && attempt.code === 'unsupported');
  // The code printed on the device, and the camera that reads it.
  const [scanning, setScanning] = useState(false);
  const [scanned, setScanned] = useState<MatterIdentity | null>(null);

  const addMatter = async (matter: MatterIdentity) => {
    const ok = await commands.request('matter.commission', {
      kind: matter.kind,
      payload: matter.payloadHex,
    });
    if (ok) commands.notify('The hub is adding the device');
  };

  if (scanning) {
    return (
      <ScanQrScreen
        onScanned={(matter) => {
          setScanned(matter);
          setScanning(false);
        }}
        onClose={() => setScanning(false)}
      />
    );
  }

  return (
    <View style={[styles.screen, { backgroundColor: t.bg }]}>
      <ScreenHeader
        title="Find devices"
        left={
          <Pressable accessibilityRole="button" accessibilityLabel="Back" onPress={onClose} style={styles.back} hitSlop={8}>
            <Glyph name="back" size={22} color={t.accent} />
          </Pressable>
        }
        right={
          <Pressable accessibilityRole="button" accessibilityLabel="Look again" onPress={scan.rescan} style={styles.back} hitSlop={8}>
            <Glyph name="signal" size={20} color={t.accent} />
          </Pressable>
        }
      />

      <ScrollView contentContainerStyle={styles.page} showsVerticalScrollIndicator={false}>
        {attempt ? <AttemptNote attempt={attempt} /> : null}

        <Text style={[styles.section, { color: t.muted }]}>NEAR YOU</Text>
        <NearbySection
          scan={scan}
          onAdd={addMatter}
          busy={busy}
          hubCannot={hubCannot}
          onScanQr={() => setScanning(true)}
          scanned={scanned}
        />

        <Text style={[styles.section, { color: t.muted }]}>ON YOUR HUB</Text>
        <HubSection join={join} commands={commands} />
      </ScrollView>
    </View>
  );
}

/** What the hub says about the attempt to add a Matter device. */
function AttemptNote({ attempt }: { attempt: MatterEvent }) {
  const t = useTheme();
  const text = matterWords(attempt);
  if (!text) return null;
  const failed = attempt.state === 'failed';
  const working = matterBusy(attempt);
  return (
    <View style={[styles.attempt, { borderColor: failed ? t.warn : t.border, backgroundColor: t.card }]}>
      <Glyph name={failed ? 'signal' : working ? 'chip' : 'check'} size={15} color={failed ? t.warn : t.accent} />
      <Text style={{ color: failed ? t.warn : t.text, fontSize: 12.5, lineHeight: 18, flex: 1 }}>{text}</Text>
    </View>
  );
}

/** What the phone hears around it — and the code printed on the device. */
function NearbySection({
  scan,
  onAdd,
  busy,
  hubCannot,
  onScanQr,
  scanned,
}: {
  scan: ReturnType<typeof useBleScan>;
  onAdd(matter: MatterIdentity): Promise<void>;
  busy: boolean;
  hubCannot: boolean;
  onScanQr(): void;
  scanned: MatterIdentity | null;
}) {
  return (
    <View style={styles.gap}>
      {/* The code is printed on the device, so this works without any radio at all. */}
      <Button kind="secondary" size="compact" label="Scan a QR code" onPress={onScanQr} />
      {scanned ? (
        <ScannedRow matter={scanned} busy={busy} hubCannot={hubCannot} onAdd={() => void onAdd(scanned)} />
      ) : null}
      <NearbyList scan={scan} onAdd={onAdd} busy={busy} hubCannot={hubCannot} />
    </View>
  );
}

/**
 * A device the person read off its own QR code. It is right in front of them, so
 * there is no signal to judge — the code says what the device is.
 */
function ScannedRow({
  matter,
  busy,
  hubCannot,
  onAdd,
}: {
  matter: MatterIdentity;
  busy: boolean;
  hubCannot: boolean;
  onAdd(): void;
}) {
  const t = useTheme();
  return (
    <View style={[styles.row, { borderColor: t.border, backgroundColor: t.card }]}>
      <View style={styles.rowHead}>
        <Glyph name="chip" size={16} color={t.accent} />
        <View style={styles.rowText}>
          <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }} numberOfLines={1}>
            Matter device
          </Text>
          <Text style={{ color: t.muted, fontSize: 12 }} numberOfLines={1}>
            From the QR code{matter.discriminator !== null ? ` · disc ${matter.discriminator}` : ''}
          </Text>
        </View>
      </View>
      {hubCannot ? (
        <Text style={{ color: t.warn, fontSize: 12.5, marginTop: 6 }}>
          This hub&apos;s Matter chip cannot add devices yet. To put the device in Apple Home, open Home → Add
          Accessory and scan the QR code on the device.
        </Text>
      ) : (
        <View style={styles.actions}>
          <Button size="mini" label="Add to the hub" disabled={busy} onPress={onAdd} />
          <Text style={{ color: t.muted, fontSize: 12, flexBasis: '100%' }}>
            The hub adds the device — it has to reach it over the network.
          </Text>
        </View>
      )}
    </View>
  );
}

/** The devices the radio hears, as far as the hub could do something with them. */
function NearbyList({
  scan,
  onAdd,
  busy,
  hubCannot,
}: {
  scan: ReturnType<typeof useBleScan>;
  onAdd(matter: MatterIdentity): Promise<void>;
  busy: boolean;
  hubCannot: boolean;
}) {
  const t = useTheme();
  if (!scan.available) {
    return (
      <Note
        tone="quiet"
        title="No Bluetooth here."
        text="Finding devices needs the app on a phone. In the browser, and in Expo Go, there is nothing to listen with."
      />
    );
  }
  if (scan.status.state === 'unauthorized') {
    return (
      <Note
        tone="quiet"
        title="Bluetooth is not allowed."
        text="Keystone needs the Bluetooth permission to see what is around you. Turn it on in the settings of the phone."
      />
    );
  }
  if (scan.status.state === 'poweredOff') {
    return <Note tone="quiet" title="Bluetooth is off." text="Turn Bluetooth on to see the devices around you." />;
  }
  if (scan.status.state === 'unsupported') {
    return <Note tone="quiet" title="No Bluetooth here." text="This device cannot look for Bluetooth devices." />;
  }
  if (scan.status.state !== 'ready') {
    return <Note tone="quiet" text="Getting the radio ready…" />;
  }
  // Only the devices that speak Matter: the hub cannot add the others, so there is
  // nothing the person could do with them.
  const devices = matterDevices(scan.devices);
  if (devices.length === 0) {
    return (
      <Note
        tone="quiet"
        title="Nothing nearby yet."
        text="Wake the device up (press its button) and it will show up here. Only devices that speak Matter can be added."
      />
    );
  }
  return (
    <View style={styles.gap}>
      <Text style={{ color: t.muted, fontSize: 12.5, lineHeight: 18 }}>
        A Matter device is added by the hub — the hub has to reach the device over the network.
      </Text>
      {devices.map((device) => (
        <NearbyRow
          key={device.id}
          device={device}
          busy={busy}
          hubCannot={hubCannot}
          onAdd={() => void onAdd(device.matter)}
        />
      ))}
    </View>
  );
}

/** One device around the phone: what it is, how close, and what can be done with it. */
function NearbyRow({
  device,
  busy,
  hubCannot,
  onAdd,
}: {
  device: MatterDevice;
  busy: boolean;
  hubCannot: boolean;
  onAdd(): void;
}) {
  const t = useTheme();
  const matter = device.matter;
  const far = tooFar(device.proximity);
  const title = device.name ?? 'Matter device';
  const subtitle = `Matter device${matter.discriminator !== null ? ` · disc ${matter.discriminator}` : ''}`;

  return (
    <View style={[styles.row, { borderColor: t.border, backgroundColor: t.card }]}>
      <View style={styles.rowHead}>
        <Glyph name="chip" size={16} color={t.accent} />
        <View style={styles.rowText}>
          <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }} numberOfLines={1}>
            {title}
          </Text>
          <Text style={{ color: t.muted, fontSize: 12 }} numberOfLines={1}>
            {subtitle}
          </Text>
        </View>
        <Text style={[styles.band, { color: bandColor(t, device.proximity) }]}>{proximityLabel(device.proximity)}</Text>
      </View>
      {far ? (
        <Text style={{ color: t.warn, fontSize: 12.5, marginTop: 6 }}>
          Too far from the device — move closer and try again.
        </Text>
      ) : hubCannot ? (
        <Text style={{ color: t.warn, fontSize: 12.5, marginTop: 6 }}>
          This hub&apos;s Matter chip cannot add devices yet. To put the device in Apple Home, open Home → Add
          Accessory and scan the QR code on the device.
        </Text>
      ) : (
        <View style={styles.actions}>
          <Button size="mini" label="Add to the hub" disabled={busy} onPress={onAdd} />
          {matter.source === 'service' ? (
            <Text style={{ color: t.muted, fontSize: 12, flexBasis: '100%' }}>
              Only the discriminator was read from the air — if the hub cannot add it, use the QR code on the device.
            </Text>
          ) : null}
          <Text style={{ color: t.muted, fontSize: 12, flexBasis: '100%' }}>
            To put the device in Apple Home, open Home → Add Accessory and scan the QR code on the device.
          </Text>
        </View>
      )}
    </View>
  );
}

function bandColor(t: ReturnType<typeof useTheme>, proximity: BleDevice['proximity']): string {
  switch (proximity) {
    case 'veryClose':
    case 'close':
      return t.good;
    case 'near':
      return t.muted;
    case 'far':
      return t.warn;
    case 'unknown':
      return t.muted;
  }
}

/** What the hub is opening: the Zigbee network, and the devices that join it. */
function HubSection({
  join,
  commands,
}: {
  join: ReturnType<typeof useJoinWindow>;
  commands: Commands;
}) {
  const t = useTheme();
  if (join.failed) {
    return (
      <View style={styles.gap}>
        <Note
          tone="quiet"
          title="The hub did not open the network."
          text="Check that the hub is awake and connected, then try again."
        />
        <Button kind="secondary" size="compact" label="Try again" onPress={join.reopen} />
      </View>
    );
  }
  if (join.opening) {
    return <Text style={{ color: t.muted, fontSize: 13.5 }}>Opening the Zigbee network for 2 minutes…</Text>;
  }
  if (!join.open) {
    return (
      <View style={styles.gap}>
        <Text style={{ color: t.muted, fontSize: 13.5, lineHeight: 19 }}>
          The Zigbee network is closed. Open it when you want to add a Zigbee device.
        </Text>
        <Button kind="secondary" size="compact" label="Open the network" onPress={join.reopen} />
      </View>
    );
  }
  if (join.expired) {
    return (
      <View style={styles.gap}>
        <Note
          tone={join.joined.length > 0 ? 'good' : 'quiet'}
          text={
            join.joined.length > 0
              ? `${join.joined.length} ${join.joined.length === 1 ? 'device' : 'devices'} added.`
              : 'The window is over. No devices joined.'
          }
        />
        <Button kind="secondary" size="compact" label="Open the network again" onPress={join.reopen} />
      </View>
    );
  }
  return (
    <View style={styles.gap}>
      <View style={styles.clockRow}>
        <Glyph name="plus" size={14} color={t.accent} />
        <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }}>{clock(join.left)}</Text>
        <Text style={{ color: t.muted, fontSize: 13, flex: 1 }}>
          Press the pair button on your Zigbee device (or power-cycle it).
        </Text>
      </View>
      {join.joined.map((entity) => (
        <JoinedRow
          key={entity.id}
          entity={entity}
          joinedAt={join.joinedAt.get(entity.id) ?? join.now}
          now={join.now}
          commands={commands}
        />
      ))}
    </View>
  );
}

/** A device that just joined: does it blink? And what is it called? */
function JoinedRow({
  entity,
  joinedAt,
  now,
  commands,
}: {
  entity: Entity;
  joinedAt: number;
  now: number;
  commands: Commands;
}) {
  const t = useTheme();
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const title = deviceTitle(entity);
  // Only a Zigbee device can be made to flash: the Matter chip has no such command yet.
  const blinkable = entity.chip === 1;

  const save = async () => {
    setSaving(true);
    const ok = await commands.request('name.set', { entity: entity.id, name: name.trim() });
    setSaving(false);
    if (ok) commands.notify('Named');
  };

  return (
    <View style={[styles.row, { borderColor: t.border, backgroundColor: t.card }]}>
      <View style={styles.rowHead}>
        <Glyph name="check" size={16} color={t.good} />
        <View style={styles.rowText}>
          <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }} numberOfLines={1}>
            {title}
          </Text>
          <Text style={{ color: t.muted, fontSize: 12 }}>joined {ago(joinedAt, now)}</Text>
        </View>
      </View>
      <Text style={{ color: t.muted, fontSize: 12.5, marginTop: 6 }}>
        {blinkable ? 'Does it blink? Then it is the one you found.' : 'A new device on the hub.'}
      </Text>
      <View style={styles.actions}>
        {blinkable ? (
          <Button
            kind="secondary"
            size="mini"
            label="Blink"
            onPress={() => void commands.request('identify', { entity: entity.id })}
          />
        ) : null}
        <Input
          label={`Name for ${title}`}
          value={name}
          onChangeText={setName}
          placeholder="Name it"
          style={styles.nameInput}
        />
        <Button size="mini" label="Save" disabled={saving || name.trim() === ''} onPress={() => void save()} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  page: { padding: 16, paddingBottom: Platform.OS === 'ios' ? 40 : 32, gap: 8 },
  section: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginTop: 14, marginBottom: 2 },
  gap: { gap: 10 },
  row: { borderWidth: 1, borderRadius: radius.card, padding: 13, gap: 2 },
  rowHead: { flexDirection: 'row', alignItems: 'center', gap: 9 },
  rowText: { flex: 1 },
  band: { fontSize: 12, fontWeight: '600' },
  actions: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 9, flexWrap: 'wrap' },
  attempt: { flexDirection: 'row', alignItems: 'center', gap: 8, borderWidth: 1, borderRadius: radius.card, padding: 12 },
  nameInput: { flex: 1, minWidth: 120, paddingVertical: 8, fontSize: 14 },
  clockRow: { flexDirection: 'row', alignItems: 'center', gap: 7 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
});
