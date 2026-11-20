import { useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from './kit';
import { Platform } from 'react-native';
import { canIdentifyDevice, canRemoveDevice, canUpdateFirmware, deviceStatus, deviceTitle, isReachable, primarySwitch, sortedDevices, agoText, roomSummary, summaryText, type DeviceStatus } from '../model/devices';
import { groupByRoom, roomChoices, roomCount } from '../model/rooms';
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

export function DevicesScreen({
  home,
  commands,
  onFindDevices,
  showScenes = true,
  temperatureUnit = 'C',
}: {
  home: HomeState;
  commands: Commands;
  onFindDevices(): void;
  showScenes?: boolean;
  temperatureUnit?: 'C' | 'F';
}) {
  const t = useTheme();
  const allRooms = groupByRoom(sortedDevices(home.entities));
  // The rooms at the top as chips; null means all of them.
  const [activeRoom, setActiveRoom] = useState<string | null>(null);
  const rooms = activeRoom === null ? allRooms : allRooms.filter((r) => r.room === activeRoom);
  const roomNames = allRooms.map((r) => r.room).filter((r): r is string => r !== null);
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
        {home.status === 'ready' && showScenes ? <ScenesBar home={home} commands={commands} /> : null}

        {roomNames.length > 0 && (
          <RoomChips rooms={roomNames} active={activeRoom} onPick={setActiveRoom} />
        )}

        {rooms.map((room) => {
          const summary = roomSummary(room.devices);
          // (climate tiles removed: sensor widgets now carry the readings)
          return (
            <View key={room.room ?? ''} style={styles.room}>
              <View style={styles.sectionHead}>
                <Glyph name="home" size={14} color={t.muted} />
                <Text accessibilityLabel={`Room ${room.room ?? 'Unassigned'}`} style={[styles.sectionTitle, { color: t.muted }]} numberOfLines={1}>
                  {room.room ?? 'Unassigned'}
                </Text>
                <Text style={[styles.sectionCount, { color: t.muted }]}>{room.devices.length}</Text>
              </View>

              {summary && summary.total > 1 && (
                <View style={[styles.pill, { backgroundColor: t.card, borderColor: t.border }]}>
                  <Text style={[styles.pillText, { color: t.muted }]}>{summaryText(summary)}</Text>
                </View>
              )}

              {/* sensor widgets now carry the readings; climate tiles removed as duplicate */}

              <DeviceList
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
                temperatureUnit={temperatureUnit}
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
          onSetRoom={async (room) => {
            // The room lives in the hub, next to the name: a device can be moved
            // without being renamed, and two devices share a room freely.
            const ok =
              (await commands.request(room === null ? 'room.clear' : 'room.set', room === null ? { entity: open.id } : { entity: open.id, room })) !== null;
            return ok;
          }}
          roomOptions={roomChoices(home.entities)}
          {...(canIdentifyDevice(home, open)
            ? {
                onIdentify: async () => (await commands.request('identify', { entity: open.id })) !== null,
              }
            : {})}
          {...(canRemoveDevice(home, open)
            ? {
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
        body="This will remove it from the hub and from Apple Home."
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

/** The rooms at the top, as chips: *All* and every room that has a device. */
export function RoomChips({
  rooms,
  active,
  onPick,
}: {
  rooms: string[];
  active: string | null;
  onPick(room: string | null): void;
}) {
  const t = useTheme();
  const chip = (label: string, room: string | null, key: string) => {
    const on = active === room;
    return (
      <Pressable
        key={key}
        accessibilityRole="button"
        accessibilityLabel={room === null ? 'All rooms' : room}
        accessibilityState={{ selected: on }}
        onPress={() => onPick(room)}
        style={[
          styles.roomChip,
          { backgroundColor: on ? t.primary : t.card, borderColor: on ? t.primary : t.border },
        ]}
      >
        <Text style={{ color: on ? t.onPrimary : t.muted, fontSize: 12.5, fontWeight: '600' }}>{label}</Text>
      </Pressable>
    );
  };
  return (
    <View style={styles.roomChips} accessibilityLabel="Rooms">
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.roomChipsInner}>
        {chip('All', null, '__all')}
        {rooms.map((r) => chip(r, r, r))}
      </ScrollView>
    </View>
  );
}

/** The room's devices, one card each. */
function DeviceList({
  devices,
  statusOf,
  reachableOf,
  onToggle,
  onOpen,
  onUnreachable,
  updatedOf,
  temperatureUnit,
}: {
  devices: Entity[];
  statusOf(e: Entity): DeviceStatus;
  reachableOf(e: Entity): boolean;
  onToggle(e: Entity): void;
  onOpen(e: Entity): void;
  onUnreachable(): void;
  updatedOf(e: Entity): string | null;
  temperatureUnit: 'C' | 'F';
}) {
  return (
    <View style={styles.cards}>
      {devices.map((entity) => (
        <DeviceWidget
          key={entity.id}
          entity={entity}
          status={statusOf(entity)}
          reachable={reachableOf(entity)}
          onToggle={() => onToggle(entity)}
          onOpen={() => onOpen(entity)}
          onUnreachable={onUnreachable}
          updatedText={updatedOf(entity)}
          temperatureUnit={temperatureUnit}
        />
      ))}
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
  cards: { gap: 8, marginTop: 10 },
  roomChips: { marginBottom: 4 },
  roomChipsInner: { gap: 8, paddingRight: 8 },
  roomChip: { paddingHorizontal: 13, paddingVertical: 8, borderRadius: 999, borderWidth: 1 },
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
