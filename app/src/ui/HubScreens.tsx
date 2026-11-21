// The hub's own screens: each one thing, opened from Settings.
import { ScrollView, StyleSheet, Text, View } from './kit';
import type { Commands } from '../hooks/useCommands';
import type { HubConnection } from '../hooks/useHub';
import { chipRows, chipSummary } from '../model/health';
import type { HomeState } from '../protocol/store';
import { AccessCard, FirmwareCard, HubActionsCard } from './HubCards';
import { HubSummaryCard, LocationCard, MatterBridgeCard } from './HubScreen';
import { HubSubScreen, SectionLabel } from './HubSubScreen';
import { PresenceCard } from './PresenceCard';
import { WifiCard } from './WifiCard';
import { useTheme } from './theme';

/** Hub & health: who the hub is, what it is running, and what it can do. */
export function HubHealthScreen({
  home,
  url,
  commands,
  onBack,
}: {
  home: HomeState;
  url: string;
  commands: Commands;
  onBack(): void;
}) {
  const t = useTheme();
  return (
    <HubSubScreen title="Hub & health" onBack={onBack}>
      <HubSummaryCard home={home} url={url} />
      <SectionLabel>CHIPS</SectionLabel>
      {home.chips.length === 0 && <Text style={{ color: t.muted }}>No chips reported yet.</Text>}
      {home.chips.map((c) => {
        const s = chipSummary(c);
        return (
          <View
            key={c.id}
            accessibilityLabel={`Chip ${s.title}`}
            style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}
          >
            <Text style={[styles.h2, { color: t.text }]}>{s.title}</Text>
            {chipRows(c).map((r) => (
              <View key={r.label} style={styles.kv}>
                <Text style={{ color: t.muted }}>{r.label}</Text>
                <Text style={{ color: t.text, fontWeight: '600' }}>{r.value}</Text>
              </View>
            ))}
          </View>
        );
      })}
      <PresenceCard home={home} commands={commands} />
      <MatterBridgeCard home={home} />
    </HubSubScreen>
  );
}

/** Wi-Fi: the network the hub is on, and how to change it. */
export function WifiScreen({ home, commands, onBack }: { home: HomeState; commands: Commands; onBack(): void }) {
  return (
    <HubSubScreen title="Wi-Fi" onBack={onBack}>
      <WifiCard home={home} commands={commands} />
    </HubSubScreen>
  );
}

/** Remote access: the token, the certificate, the relay. */
export function RemoteAccessScreen({
  client,
  commands,
  onTokenChange,
  onUseRelay,
  onBack,
}: {
  client: HubConnection;
  commands: Commands;
  onTokenChange?(token: string): Promise<void>;
  onUseRelay?(url: string): void;
  onBack(): void;
}) {
  return (
    <HubSubScreen title="Remote access" onBack={onBack}>
      <AccessCard client={client} commands={commands} {...(onTokenChange ? { onTokenChange } : {})} {...(onUseRelay ? { onUseRelay } : {})} />
    </HubSubScreen>
  );
}

/** Firmware: the hub's own firmware and the chips'. */
export function FirmwareScreen({
  home,
  client,
  commands,
  onBack,
}: {
  home: HomeState;
  client: HubConnection;
  commands: Commands;
  onBack(): void;
}) {
  return (
    <HubSubScreen title="Firmware" onBack={onBack}>
      <FirmwareCard home={home} client={client} commands={commands} />
    </HubSubScreen>
  );
}

/** Location: where the hub is, so that sunrise and sunset mean something. */
export function LocationScreen({ home, commands, onBack }: { home: HomeState; commands: Commands; onBack(): void }) {
  return (
    <HubSubScreen title="Location" onBack={onBack}>
      <LocationCard home={home} commands={commands} />
    </HubSubScreen>
  );
}

/** The hub's actions: reboot, reset, forget the network. */
export function HubActionsScreen({
  commands,
  onChangeHub,
  onBack,
}: {
  commands: Commands;
  onChangeHub(): void;
  onBack(): void;
}) {
  return (
    <HubSubScreen title="Hub actions" onBack={onBack}>
      <HubActionsCard commands={commands} onChangeHub={onChangeHub} />
    </HubSubScreen>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 14, gap: 4 },
  h2: { fontSize: 17, fontWeight: '600' },
  kv: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10, fontSize: 12.5 },
});
