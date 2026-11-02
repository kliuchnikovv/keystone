import { useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { Platform } from 'react-native';
import { canManageDevice, canUpdateFirmware, deviceStatus, deviceTitle, isReachable, primarySwitch, sortedDevices, agoText, climateTiles, roomSummary, summaryText, type DeviceStatus } from '../model/devices';
import { deviceShape, gridLayout } from '../model/grid';
import { groupByRoom, roomCount } from '../model/rooms';
import { useEntityActivity } from '../hooks/useEntityActivity';
import { useNow } from '../hooks/useNow';
import type { Commands } from '../hooks/useCommands';
import type { Entity } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import { DeviceSheet } from './DeviceSheet';
import { DeviceWidget } from './DeviceWidget';
import { EmptyArt } from './EmptyArt';
import { Glyph } from './Glyph';
import { ScenesBar } from './ScenesBar';
import { ConfirmDialog } from './Sheet';
import { radius, useTheme } from './theme';

export function DevicesScreen({ home, commands, onFindDevices }: { home: HomeState; commands: Commands; onFindDevices(): void }) {
  const t = useTheme();
  const rooms = groupByRoom(sortedDevices(home.entities));
  const [openId, setOpenId] = useState<number | null>(null);
  const [removingId, setRemovingId] = useState<number | null>(null);
  const now = useNow(1000);
  const activity = useEntityActivity(home.entities, home.savedAt);
  const offline = home.status !== 'ready';

  const open = home.entities.find((e) => e.id === openId) ?? null;
  const removing = home.entities.find((e) => e.id === removingId) ?? null;

  return (
    <>
      <ScrollView contentContainerStyle={styles.list} style={offline ? styles.offline : undefined}>
        <Greeting home={home} />
        {home.status === 'ready' ? <ScenesBar home={home} commands={commands} /> : null}

        {rooms.map((room) => {
          const summary = roomSummary(room.devices);
          const tiles = climateTiles(room.devices);
          return (
            <View key={room.room ?? ''} style={styles.room}>
              <View style={styles.sectionHead}>
                <Glyph name="home" size={14} color={t.muted} />
                <Text style={[styles.sectionTitle, { color: t.muted }]} numberOfLines={1}>
                  {room.room ?? 'Unassigned'}
                </Text>
                <Text style={[styles.sectionCount, { color: t.muted }]}>{room.devices.length}</Text>
              </View>

              {summary && summary.total > 1 && (
                <View style={[styles.pill, { backgroundColor: t.card, borderColor: t.border }]}>
                  <Text style={[styles.pillText, { color: t.muted }]}>{summaryText(summary)}</Text>
                </View>
              )}

              {tiles.length > 0 && <ClimateCard tiles={tiles} />}

              <DeviceGrid
                devices={room.devices}
                statusOf={(e) => deviceStatus(home, e)}
                reachableOf={(e) => isReachable(home, e)}
                onToggle={(e) => {
                  const sw = primarySwitch(e);
                  if (sw) commands.send(e.id, sw.id, sw.value !== true);
                }}
                onOpen={(e) => setOpenId(e.id)}
                onUnreachable={() => commands.notify('Device unreachable')}
                updatedOf={(e) => agoText(activity.updatedAt.get(e.id), now)}
              />
            </View>
          );
        })}

        {rooms.length === 0 && (
          <View style={styles.empty}>
            <EmptyArt />
            <Text style={[styles.emptyTitle, { color: t.text }]}>
              {home.status === 'ready' ? 'No devices yet' : 'Waiting for the hub…'}
            </Text>
            <Text style={[styles.emptyText, { color: t.muted }]}>
              {home.status === 'ready' ? 'Tap below to add your first one.' : 'The devices show up as soon as the hub answers.'}
            </Text>
          </View>
        )}

        {home.status === 'ready' && (
          <Pressable
            accessibilityRole="button"
            accessibilityLabel="Add a device"
            onPress={onFindDevices}
            style={[styles.add, { borderColor: t.border }]}
          >
            <Glyph name="plus" size={18} color={t.accent} />
            <Text style={[styles.addText, { color: t.accent }]}>Add a device</Text>
          </Pressable>
        )}
      </ScrollView>

      {open && (
        <DeviceSheet
          entity={open}
          status={deviceStatus(home, open)}
          reachable={isReachable(home, open)}
          visible={openId !== null}
          isPending={(attr) => commands.isPending(open.id, attr)}
          onSet={(attr, value) => commands.send(open.id, attr, value)}
          onClose={() => setOpenId(null)}
          onRename={async (name) => {
            const ok =
              (await commands.request(name === null ? 'name.clear' : 'name.set', name === null ? { entity: open.id } : { entity: open.id, name })) !== null;
            if (ok) commands.notify(name === null ? 'Name removed' : 'Device renamed');
            return ok;
          }}
          {...(canManageDevice(home, open)
            ? {
                onIdentify: async () => (await commands.request('identify', { entity: open.id })) !== null,
                onRemove: () => setRemovingId(open.id),
              }
            : {})}
          notify={commands.notify}
          updatedText={agoText(activity.updatedAt.get(open.id), now)}
          pressedText={agoText(activity.pressedAt.get(open.id), now)}
          {...(canUpdateFirmware(open)
            ? {
                // Only a Zigbee device can be given new firmware.
                onStartUpdate: async (url: string, sha256: string) =>
                  (await commands.request('device.ota', { entity: open.id, url, sha256 })) !== null,
              }
            : {})}
          ota={home.deviceOta[open.id]}
        />
      )}

      <ConfirmDialog
        visible={removing !== null}
        title={removing ? `Remove ${deviceTitle(removing)}?` : 'Remove device?'}
        body="This will take it off the Zigbee network and from Apple Home."
        confirmLabel="Remove device"
        danger
        onCancel={() => setRemovingId(null)}
        onConfirm={() => {
          const id = removing?.id;
          setRemovingId(null);
          if (id === undefined) return;
          void (async () => {
            const ok = (await commands.request('unpair', { entity: id })) !== null;
            if (ok) commands.notify('Device removed');
          })();
        }}
      />
    </>
  );
}

/** The room's devices on a two-column grid: wide buttons, squares and tall ones.
 * The grid is measured first, then the buttons are placed in it — that is how a
 * button can be wider than its neighbour. */
function DeviceGrid({ devices, statusOf, reachableOf, onToggle, onOpen, onUnreachable, updatedOf }: {
  devices: Entity[];
  statusOf(e: Entity): DeviceStatus;
  reachableOf(e: Entity): boolean;
  onToggle(e: Entity): void;
  onOpen(e: Entity): void;
  onUnreachable(): void;
  updatedOf(e: Entity): string | null;
}) {
  const [width, setWidth] = useState(0);
  const items = devices.map((e) => ({ id: e.id, shape: deviceShape(e) }));
  const { boxes, height } = gridLayout(items, width);
  const byId = new Map(devices.map((e) => [e.id, e]));
  return (
    <View style={[styles.grid, { height }]} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
      {boxes.map((b) => {
        const entity = byId.get(b.id);
        if (!entity) return null;
        return (
          <View key={b.id} style={[styles.cell, { left: b.x, top: b.y, width: b.width, height: b.height }]}>
            <DeviceWidget
              entity={entity}
              shape={b.shape}
              status={statusOf(entity)}
              reachable={reachableOf(entity)}
              onToggle={() => onToggle(entity)}
              onOpen={() => onOpen(entity)}
              onUnreachable={onUnreachable}
              updatedText={updatedOf(entity)}
            />
          </View>
        );
      })}
    </View>
  );
}

/** Who and where: the hub's name, the time of day, and how much home there is. */
function Greeting({ home }: { home: HomeState }) {
  const t = useTheme();
  const name = home.hub?.name ?? 'Home';
  const rooms = roomCount(home.entities);
  const count = home.entities.length;
  // "1 room · 1 device" says nothing the screen does not already show.
  const worthCounting = rooms > 1 || count > 1;
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
        {worthCounting && (
          <View style={[styles.pill, { backgroundColor: t.card }]}>
            <Glyph name="home" size={12} color={t.muted} />
            <Text style={[styles.pillText, { color: t.muted }]}>
              {rooms > 0 && `${rooms} ${rooms === 1 ? 'room' : 'rooms'} · `}
              {count} {count === 1 ? 'device' : 'devices'}
            </Text>
          </View>
        )}
      </View>
    </View>
  );
}

/** The room's climate as tiles: what the sensors say, and nothing more. */
function ClimateCard({ tiles }: { tiles: ReturnType<typeof climateTiles> }) {
  const t = useTheme();
  const tone = (kind: 'warm' | 'cool' | 'good') => (kind === 'warm' ? t.accent : kind === 'cool' ? t.cool : t.good);
  return (
    <View style={[styles.climate, { backgroundColor: t.glass, borderColor: t.glassBorder, borderRadius: t.cardRadius }]}>
      <View style={styles.climateHead}>
        <Glyph name="climate" size={15} color={t.muted} />
        <Text style={[styles.climateTitle, { color: t.muted }]}>Climate</Text>
      </View>
      <View style={styles.tiles}>
        {tiles.map((tile) => (
          <View
            key={tile.label}
            style={[styles.tile, { backgroundColor: t.fill, borderColor: t.border }]}
          >
            <Text style={[styles.tileValue, { color: t.text }]} numberOfLines={1}>
              {tile.value}
            </Text>
            <Text style={[styles.tileLabel, { color: tone(tile.kind) }]} numberOfLines={1}>
              {tile.label}
            </Text>
          </View>
        ))}
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
  list: { padding: 16, paddingBottom: Platform.OS === 'ios' ? 110 : 98, flexGrow: 1 },
  greeting: { marginBottom: 18 },
  greetLabel: { fontSize: 13, fontWeight: '500' },
  greetTitle: { fontSize: 30, fontWeight: '700', letterSpacing: -0.02, marginTop: 2 },
  pills: { flexDirection: 'row', gap: 8, marginTop: 10 },
  pill: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, paddingVertical: 6, borderRadius: radius.pill, borderWidth: StyleSheet.hairlineWidth, alignSelf: 'flex-start' },
  pillText: { fontSize: 12, fontWeight: '500' },
  room: { marginBottom: 22 },
  sectionHead: { flexDirection: 'row', alignItems: 'center', gap: 6, marginBottom: 8, paddingHorizontal: 2 },
  sectionTitle: { fontSize: 12, fontWeight: '700', letterSpacing: 0.8, textTransform: 'uppercase', flexShrink: 1 },
  sectionCount: { fontSize: 12, fontWeight: '500' },
  climate: { borderWidth: 1, padding: 16, marginTop: 10 },
  climateHead: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 14 },
  climateTitle: { fontSize: 11, fontWeight: '700', letterSpacing: 1, textTransform: 'uppercase' },
  tiles: { flexDirection: 'row', gap: 8 },
  tile: { flex: 1, borderRadius: 14, borderWidth: StyleSheet.hairlineWidth, paddingHorizontal: 10, paddingVertical: 11 },
  tileValue: { fontSize: 20, fontWeight: '700', letterSpacing: -0.5 },
  tileLabel: { fontSize: 10, fontWeight: '500', marginTop: 4 },
  grid: { marginTop: 10, position: 'relative' },
  cell: { position: 'absolute' },
  // Without the hub the values on screen are the last known ones: dim them a little,
  // but keep them readable — they are what the user came to see.
  offline: { opacity: 0.75 },
  add: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 8,
    borderWidth: 1,
    borderStyle: 'dashed',
    borderRadius: radius.card,
    paddingVertical: 14,
    marginTop: 4,
  },
  addText: { fontSize: 15, fontWeight: '600' },
  empty: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingTop: 40, gap: 6 },
  emptyTitle: { fontSize: 18, fontWeight: '600', marginTop: 10 },
  emptyText: { fontSize: 14, textAlign: 'center' },
});
