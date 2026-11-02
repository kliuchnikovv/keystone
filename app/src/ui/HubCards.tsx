// The hub's own cards: firmware and updates, who may talk to it, and what can be done
// to it. These act on the hub itself (reboot, reset, forget the network), so they are
// kept apart from the devices.
import { useCallback, useEffect, useState } from 'react';
import { Pressable, Share, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import type { HubConnection } from '../hooks/useHub';
import type { HomeState } from '../protocol/store';
import { DEFAULT_RELAY_HOST, relayClientUrl, relayTunnelUrlFromParts } from '../settings';
import { Button, ErrorText, Input } from './controls';
import { ConfirmDialog, Sheet } from './Sheet';
import { radius, useTheme } from './theme';

/** What the hub answers about the running update. */
interface OtaStatus {
  state: 'idle' | 'downloading' | 'verifying' | 'done' | 'failed' | string;
  pct: number;
  chip: number;
  message: string;
}

const OTA_TONE: Record<string, 'good' | 'warn' | 'bad' | 'normal'> = {
  idle: 'normal',
  downloading: 'warn',
  verifying: 'warn',
  done: 'good',
  failed: 'bad',
};

export function FirmwareCard({ home, client, commands }: { home: HomeState; client: HubConnection; commands: Commands }) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  const [chip, setChip] = useState<number>(1);
  const [url, setUrl] = useState('');
  const [sha, setSha] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<OtaStatus | null>(null);

  const chips = home.chips;
  useEffect(() => {
    if (chips.length > 0 && !chips.some((c) => c.id === chip)) setChip(chips[0]!.id);
  }, [chips, chip]);

  // While something is being written, follow the hub's progress.
  const running = status?.state === 'downloading' || status?.state === 'verifying';
  useEffect(() => {
    if (!running) return;
    const id = setInterval(() => {
      void client
        .req('ota.status')
        .then((r) => setStatus({ state: String(r.state ?? 'idle'), pct: Number(r.pct ?? 0), chip: Number(r.chip ?? 0), message: String(r.message ?? '') }))
        .catch(() => undefined);
    }, 2000);
    return () => clearInterval(id);
  }, [running, client]);

  const start = async () => {
    setError(null);
    const r = await commands.request('ota.start', { chip, url: url.trim(), sha256: sha.trim() });
    if (r === null) return; // the reason is already on screen
    setOpen(false);
    setUrl('');
    setSha('');
    setStatus({ state: 'downloading', pct: 0, chip, message: 'starting…' });
  };

  const tone = OTA_TONE[status?.state ?? 'idle'] ?? 'normal';
  const toneColour = tone === 'good' ? t.good : tone === 'warn' ? t.warn : tone === 'bad' ? t.bad : t.muted;

  return (
    <View accessibilityLabel="Firmware" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <View style={styles.head}>
        <Text style={[styles.h2, { color: t.text }]}>Firmware</Text>
        {status && status.state !== 'idle' && <Text style={{ color: toneColour, fontWeight: '600', fontSize: 12 }}>{status.state}</Text>}
      </View>

      <View style={styles.kv}>
        <Text style={{ color: t.muted }}>Hub</Text>
        <Text style={{ color: t.text, fontWeight: '600' }}>{home.hub?.fw ?? '—'}</Text>
      </View>
      {chips.map((c) => (
        <View key={c.id} style={styles.kv}>
          <Text style={{ color: t.muted }}>{c.label}</Text>
          <Text style={{ color: t.text, fontWeight: '600' }}>{c.status}</Text>
        </View>
      ))}

      {status && status.state !== 'idle' && (
        <View style={{ marginTop: 8 }}>
          <View style={[styles.progress, { backgroundColor: t.border }]}>
            <View style={[styles.progressFill, { width: `${Math.min(100, Math.max(0, status.pct))}%`, backgroundColor: t.accent }]} />
          </View>
          <Text style={{ color: t.muted, fontSize: 12, marginTop: 4 }}>
            {status.message || `${status.pct} %`}
          </Text>
        </View>
      )}

      <View style={styles.row}>
        <Button kind="secondary" size="compact" label="Update from URL" onPress={() => setOpen(true)} />
      </View>

      <Sheet visible={open} title="Update a co-processor" onCancel={() => setOpen(false)}>
        <Text style={{ color: t.muted, fontSize: 13, marginTop: 4 }}>
          The hub pulls the image over the network and writes it to the chip over the link.
          The chip reboots at the end.
        </Text>
        <Text style={[styles.label, { color: t.muted }]}>Chip</Text>
        <View style={styles.chipRow}>
          {chips.map((c) => (
            <Pressable
              key={c.id}
              accessibilityRole="button"
              accessibilityLabel={`Update ${c.label}`}
              onPress={() => setChip(c.id)}
              style={[styles.chip, { borderColor: chip === c.id ? t.accent : t.border, backgroundColor: chip === c.id ? t.accent : 'transparent' }]}
            >
              <Text style={{ color: chip === c.id ? t.onAccent : t.text, fontSize: 12.5 }}>{c.label}</Text>
            </Pressable>
          ))}
        </View>
        <Text style={[styles.label, { color: t.muted }]}>Image URL</Text>
        <Input label="Image URL" value={url} onChangeText={setUrl} placeholder="https://example.com/firmware.bin" keyboardType="url" />
        <Text style={[styles.label, { color: t.muted }]}>SHA-256</Text>
        <Input label="SHA-256" value={sha} onChangeText={setSha} placeholder="64 hex characters" autoCapitalize="characters" />
        <ErrorText text={error} />
        <View style={styles.row}>
          <Button kind="secondary" label="Cancel" onPress={() => setOpen(false)} />
          <Button label="Start update" onPress={() => void start()} />
        </View>
      </Sheet>
    </View>
  );
}

export function AccessCard({ client, commands, onTokenChange, onUseRelay }: { client: HubConnection; commands: Commands; onTokenChange?(token: string): Promise<void>; onUseRelay?(url: string): void }) {
  const t = useTheme();
  const [info, setInfo] = useState<{ token: string; fingerprint: string; remote: boolean; relay_url: string } | null>(null);
  const [showToken, setShowToken] = useState(false);
  const [open, setOpen] = useState(false);
  const [relayHost, setRelayHost] = useState(DEFAULT_RELAY_HOST);
  const [relayId, setRelayId] = useState('');
  const [secret, setSecret] = useState('');
  // The secret is only needed for a relay the person runs themselves. Our relay
  // knows the hub already, so the field stays out of the way until it is asked for.
  const [showSecret, setShowSecret] = useState(false);
  const [error, setError] = useState<string | null>(null);
  // The hub's own address ends in /tunnel/<id>; the app talks to the client side.
  const clientUrl = info?.relay_url ? relayClientUrl(info.relay_url) : '';

  const load = useCallback(async () => {
    try {
      const r = await client.req('access.get');
      setInfo({
        token: String(r.token ?? ''),
        fingerprint: String(r.fingerprint ?? ''),
        remote: r.remote === true,
        relay_url: String(r.relay_url ?? ''),
      });
    } catch {
      setInfo(null);
    }
  }, [client]);

  useEffect(() => {
    void load();
  }, [load]);

  const setRelay = async () => {
    setError(null);
    const tunnelUrl = relayTunnelUrlFromParts(relayHost.trim(), relayId.trim());
    const trimmed = secret.trim();
    const args: { url: string; secret?: string } = { url: tunnelUrl };
    if (trimmed) args.secret = trimmed;
    const r = await commands.request('access.relay.set', args);
    if (r === null) return;
    setOpen(false);
    setRelayHost(DEFAULT_RELAY_HOST);
    setRelayId('');
    setSecret('');
    await load();
    commands.notify('The hub now uses this relay. Connect through it below.');
  };

  const clearRelay = async () => {
    const r = await commands.request('access.relay.clear');
    if (r !== null) await load();
  };

  const newToken = async () => {
    const next = Math.random().toString(36).slice(2, 10) + Math.random().toString(36).slice(2, 10);
    const r = await commands.request('access.token.set', { token: next });
    if (r === null) return;
    if (onTokenChange) await onTokenChange(next);
  };

  const share = () => {
    if (info?.token) void Share.share({ message: info.token, title: 'Hub token' });
  };

  return (
    <View accessibilityLabel="Access" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <View style={styles.head}>
        <Text style={[styles.h2, { color: t.text }]}>Access</Text>
      </View>

      <View style={styles.kv}>
        <Text style={{ color: t.muted }}>Token</Text>
        <View style={styles.inline}>
          <Text style={{ color: t.text, fontWeight: '600' }}>{info ? (showToken ? info.token : '••••••••••••') : '—'}</Text>
          <Pressable accessibilityRole="button" accessibilityLabel={showToken ? 'Hide the token' : 'Show the token'} onPress={() => setShowToken((s) => !s)} hitSlop={8}>
            <Text style={{ color: t.accent, fontSize: 12.5, fontWeight: '600' }}>{showToken ? 'Hide' : 'Show'}</Text>
          </Pressable>
          <Pressable accessibilityRole="button" accessibilityLabel="Share the token" onPress={share} hitSlop={8}>
            <Text style={{ color: t.accent, fontSize: 12.5, fontWeight: '600' }}>Share</Text>
          </Pressable>
        </View>
      </View>

      <View style={styles.kv}>
        <Text style={{ color: t.muted }}>Certificate</Text>
        <Text style={{ color: t.text, fontWeight: '600', flex: 1, textAlign: 'right' }} numberOfLines={1}>
          {info?.fingerprint ? `${info.fingerprint.slice(0, 17)}…` : 'no TLS'}
        </Text>
      </View>

      <View style={styles.kv}>
        <Text style={{ color: t.muted }}>Remote access</Text>
        <Text style={{ color: t.text, fontWeight: '600', flex: 1, textAlign: 'right' }} numberOfLines={1}>
          {info?.relay_url ? info.relay_url : 'not set up'}
        </Text>
      </View>

      {clientUrl ? (
        <View style={styles.kv}>
          <Text style={{ color: t.muted }}>Through the relay</Text>
          <Text style={{ color: t.text, fontWeight: '600', flex: 1, textAlign: 'right' }} numberOfLines={1}>
            {clientUrl}
          </Text>
        </View>
      ) : null}

      {clientUrl && onUseRelay ? (
        <View style={{ marginTop: 8 }}>
          <Button label="Connect through the relay" a11y="Connect through the relay" onPress={() => onUseRelay(clientUrl)} />
          <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 6 }}>
            The app keeps its token and switches to the relay address — nothing to type.
          </Text>
        </View>
      ) : null}

      <View style={styles.row}>
        <Button
          kind="secondary"
          size="compact"
          label={info?.relay_url ? 'Change relay' : 'Set up relay'}
          onPress={() => {
            const ru = info?.relay_url ?? '';
            const hm = ru.match(/^wss:\/\/([^/]+)\/tunnel\//i);
            const im = ru.match(/\/tunnel\/([^\s/]+)\s*$/i);
            setRelayHost(hm?.[1] ?? DEFAULT_RELAY_HOST);
            setRelayId(im?.[1] ?? '');
            setSecret('');
            setShowSecret(false);
            setOpen(true);
          }}
        />
        {info?.relay_url ? (
          <Button kind="secondary" size="compact" label="Remove relay" onPress={() => void clearRelay()} />
        ) : null}
        <Button size="compact" label="New token" a11y="New token" onPress={() => void newToken()} />
      </View>
      <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 6 }}>
        A new token closes this connection: the app reconnects with the new one.
        {'\n'}Changing the relay drops a connection that goes through the old one — connect with the new address.
      </Text>

      <Sheet visible={open} title="Remote access" onCancel={() => setOpen(false)}>
        <Text style={{ color: t.muted, fontSize: 13, marginTop: 4 }}>
          The hub keeps a tunnel to a relay server; the app talks to it through there.
        </Text>
        <Text style={[styles.label, { color: t.muted }]}>Relay server</Text>
        <Input label="Relay server" value={relayHost} onChangeText={setRelayHost} placeholder={DEFAULT_RELAY_HOST} keyboardType="url" />
        <Text style={[styles.label, { color: t.muted }]}>Hub ID</Text>
        <Input label="Hub ID" value={relayId} onChangeText={setRelayId} placeholder="e.g. home" />
        {showSecret ? (
          <>
            <Text style={[styles.label, { color: t.muted }]}>Secret</Text>
            <Input label="Secret" value={secret} onChangeText={setSecret} placeholder="the same secret on both sides" secureTextEntry />
            <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 4 }}>
              For a relay you run yourself. The hub keeps it; it is never sent back.
            </Text>
          </>
        ) : (
          <Pressable
            accessibilityRole="button"
            accessibilityLabel="I run my own relay"
            onPress={() => setShowSecret(true)}
            hitSlop={8}
            style={{ marginTop: 10 }}
          >
            <Text style={{ color: t.accent, fontSize: 12.5, fontWeight: '600' }}>I run my own relay — it needs a secret</Text>
          </Pressable>
        )}
        <ErrorText text={error} />
        <View style={styles.row}>
          <Button kind="secondary" label="Cancel" onPress={() => setOpen(false)} />
          <Button label="Save" onPress={() => void setRelay()} />
        </View>
      </Sheet>
    </View>
  );
}

export function HubActionsCard({ commands, onChangeHub }: { commands: Commands; onChangeHub(): void }) {
  const t = useTheme();
  const [armed, setArmed] = useState(false);
  const [reset, setReset] = useState(false);
  const [forget, setForget] = useState(false);

  const reboot = async () => {
    setArmed(false);
    await commands.request('hub.reboot');
  };

  return (
    <View accessibilityLabel="Hub actions" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <Text style={[styles.h2, { color: t.text }]}>Hub actions</Text>

      <View style={styles.row}>
        <Button
          kind="secondary"
          size="compact"
          label={armed ? 'Tap again to reboot' : 'Reboot'}
          a11y={armed ? 'Tap again to reboot the hub' : 'Reboot the hub'}
          onPress={() => {
            if (armed) void reboot();
            else setArmed(true);
          }}
        />
        <Button
          kind="secondary"
          size="compact"
          label={armed ? 'Tap again to erase everything' : 'Factory reset'}
          a11y={armed ? 'Tap again to erase everything' : 'Factory reset the hub'}
          onPress={() => {
            if (armed) setReset(true);
            else setArmed(true);
          }}
        />
      </View>
      <View style={styles.row}>
        <Button kind="secondary" size="compact" label="Forget Wi-Fi" a11y="Forget the Wi-Fi network" onPress={() => setForget(true)} />
        <Button kind="secondary" size="compact" label="Switch hub" a11y="Switch hub" onPress={onChangeHub} />
      </View>

      <ConfirmDialog
        visible={reset}
        title="Factory reset the hub?"
        body="Every device, rule, schedule, scene and name is forgotten, and the hub's own settings are wiped. This cannot be undone."
        confirmLabel="Factory reset"
        danger
        onCancel={() => {
          setReset(false);
          setArmed(false);
        }}
        onConfirm={() => {
          setReset(false);
          setArmed(false);
          void commands.request('hub.factory_reset');
        }}
      />

      <ConfirmDialog
        visible={forget}
        title="Forget the Wi-Fi network?"
        body="The hub forgets the saved network and opens its own access point again. You will have to set it up once more."
        confirmLabel="Forget Wi-Fi"
        danger
        onCancel={() => setForget(false)}
        onConfirm={() => {
          setForget(false);
          void commands.request('wifi.forget');
        }}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 14, gap: 4 },
  head: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  h2: { fontSize: 17, fontWeight: '600' },
  kv: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10 },
  inline: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  row: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'flex-end', gap: 8, marginTop: 6 },
  label: { fontSize: 13, marginTop: 12, marginBottom: 6 },
  chipRow: { flexDirection: 'row', gap: 8 },
  chip: { paddingHorizontal: 11, paddingVertical: 6, borderRadius: radius.pill, borderWidth: 1 },
  progress: { height: 4, borderRadius: 2, overflow: 'hidden' },
  progressFill: { height: 4, borderRadius: 2 },
});
