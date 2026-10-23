import { SectionList, StyleSheet, Text, View } from 'react-native';
import { canManageDevice, isReachable, sortedDevices } from '../model/devices';
import { groupByRoom, roomCount } from '../model/rooms';
import type { Commands } from '../hooks/useCommands';
import type { Entity } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import { DeviceCard } from './DeviceCard';
import { Glyph } from './Glyph';
import { PairingPanel } from './PairingPanel';
import { ScenesBar } from './ScenesBar';
import { radius, useTheme } from './theme';

export function DevicesScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const rooms = groupByRoom(sortedDevices(home.entities));
  const sections = rooms.map((r) => ({ room: r.room, data: r.devices }));

  return (
    <SectionList
      sections={sections}
      keyExtractor={(e) => String(e.id)}
      stickySectionHeadersEnabled={false}
      contentContainerStyle={styles.list}
      ListHeaderComponent={
        <>
          <Greeting home={home} />
          {home.status === 'ready' ? (
            <>
              <ScenesBar home={home} commands={commands} />
              <PairingPanel home={home} commands={commands} />
            </>
          ) : null}
        </>
      }
      renderSectionHeader={({ section }) => (
        <View style={styles.sectionHead}>
          <Glyph name="home" size={14} color={t.muted} />
          <Text style={[styles.sectionTitle, { color: t.muted }]} numberOfLines={1}>
            {section.room ?? 'Unassigned'}
          </Text>
          <Text style={[styles.sectionCount, { color: t.muted }]}>{section.data.length}</Text>
        </View>
      )}
      renderItem={({ item }) => (
        <DeviceCard
          entity={item}
          reachable={isReachable(home, item)}
          isPending={(attr) => commands.isPending(item.id, attr)}
          onSet={(attr, value) => void commands.send(item.id, attr, value)}
          onRename={async (name) =>
            (await commands.request(name === null ? 'name.clear' : 'name.set', name === null ? { entity: item.id } : { entity: item.id, name })) !== null
          }
          {...(canManageDevice(home, item)
            ? {
                onIdentify: async () => (await commands.request('identify', { entity: item.id })) !== null,
                onRemove: async () => (await commands.request('unpair', { entity: item.id })) !== null,
              }
            : {})}
        />
      )}
      ListEmptyComponent={
        <View style={styles.empty}>
          <Text style={[styles.emptyTitle, { color: t.text }]}>{home.status === 'ready' ? 'No devices yet' : 'Waiting for the hub…'}</Text>
          {home.status === 'ready' && (
            <Text style={{ color: t.muted, textAlign: 'center' }}>
              Use “Add a device” above, then name it like kitchen_lamp — the room comes from the name.
            </Text>
          )}
        </View>
      }
    />
  );
}

/** Who and where: the hub's name, the time of day, and how much home there is. */
function Greeting({ home }: { home: HomeState }) {
  const t = useTheme();
  const name = home.hub?.name ?? 'Home';
  const rooms = roomCount(home.entities);
  const count = home.entities.length;
  return (
    <View style={styles.greeting}>
      <Text style={[styles.greetLabel, { color: t.muted }]}>{greeting(home)}</Text>
      <Text style={[styles.greetTitle, { color: t.text }]}>{name}</Text>
      <View style={styles.pills}>
        {home.hub?.sun?.set && (
          <View style={[styles.pill, { backgroundColor: t.card }]}>
            <Glyph name="light" size={12} color={t.accent} />
            <Text style={[styles.pillText, { color: t.muted }]}>sunset {home.hub.sun.set}</Text>
          </View>
        )}
        <View style={[styles.pill, { backgroundColor: t.card }]}>
          <Glyph name="home" size={12} color={t.muted} />
          <Text style={[styles.pillText, { color: t.muted }]}>
            {rooms} {rooms === 1 ? 'room' : 'rooms'} · {count} {count === 1 ? 'device' : 'devices'}
          </Text>
        </View>
      </View>
    </View>
  );
}

function greeting(home: HomeState): string {
  const now = home.hub?.synced === true ? home.hub.time : null;
  if (now === null || typeof now !== 'number' || now <= 0) return 'Connected';
  const h = new Date(now * 1000).getHours();
  if (h < 5) return 'Good night';
  if (h < 12) return 'Good morning';
  if (h < 18) return 'Good afternoon';
  return 'Good evening';
}

const styles = StyleSheet.create({
  list: { padding: 16, paddingBottom: 40, flexGrow: 1 },
  greeting: { marginBottom: 18 },
  greetLabel: { fontSize: 13, fontWeight: '500' },
  greetTitle: { fontSize: 30, fontWeight: '700', letterSpacing: -0.02, marginTop: 2 },
  pills: { flexDirection: 'row', gap: 8, marginTop: 10 },
  pill: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, paddingVertical: 6, borderRadius: radius.pill },
  pillText: { fontSize: 12, fontWeight: '500' },
  sectionHead: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 22, marginBottom: 8, paddingHorizontal: 2 },
  sectionTitle: { fontSize: 12, fontWeight: '700', letterSpacing: 0.8, textTransform: 'uppercase', flexShrink: 1 },
  sectionCount: { fontSize: 12, fontWeight: '500' },
  empty: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingTop: 80, gap: 6 },
  emptyTitle: { fontSize: 18, fontWeight: '600' },
});