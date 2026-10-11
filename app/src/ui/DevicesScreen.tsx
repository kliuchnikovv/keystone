import { FlatList, StyleSheet, Text, View } from 'react-native';
import { canManageDevice, isReachable, sortedDevices } from '../model/devices';
import type { Commands } from '../hooks/useCommands';
import type { HomeState } from '../protocol/store';
import { DeviceCard } from './DeviceCard';
import { PairingPanel } from './PairingPanel';
import { ScenesBar } from './ScenesBar';
import { useTheme } from './theme';

export function DevicesScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const devices = sortedDevices(home.entities);
  return (
    <FlatList
      data={devices}
      keyExtractor={(e) => String(e.id)}
      contentContainerStyle={styles.list}
      ListHeaderComponent={
        home.status === 'ready' ? (
          <>
            <ScenesBar home={home} commands={commands} />
            <PairingPanel home={home} commands={commands} />
          </>
        ) : null
      }
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
            <Text style={{ color: t.muted, textAlign: 'center' }}>Use "Add a device" above and it will show up here.</Text>
          )}
        </View>
      }
    />
  );
}

const styles = StyleSheet.create({
  list: { padding: 16, flexGrow: 1 },
  empty: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingTop: 80, gap: 6 },
  emptyTitle: { fontSize: 18, fontWeight: '600' },
});
