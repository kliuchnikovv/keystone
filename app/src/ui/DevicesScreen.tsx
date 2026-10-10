import { FlatList, StyleSheet, Text, View } from 'react-native';
import { isReachable, sortedDevices } from '../model/devices';
import type { Commands } from '../hooks/useCommands';
import type { HomeState } from '../protocol/store';
import { DeviceCard } from './DeviceCard';
import { useTheme } from './theme';

export function DevicesScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const devices = sortedDevices(home.entities);
  return (
    <FlatList
      data={devices}
      keyExtractor={(e) => String(e.id)}
      contentContainerStyle={styles.list}
      renderItem={({ item }) => (
        <DeviceCard
          entity={item}
          reachable={isReachable(home, item)}
          isPending={(attr) => commands.isPending(item.id, attr)}
          onSet={(attr, value) => void commands.send(item.id, attr, value)}
        />
      )}
      ListEmptyComponent={
        <View style={styles.empty}>
          <Text style={[styles.emptyTitle, { color: t.text }]}>{home.status === 'ready' ? 'No devices yet' : 'Waiting for the hub…'}</Text>
          {home.status === 'ready' && (
            <Text style={{ color: t.muted, textAlign: 'center' }}>Pair a device on the hub (the pair command) and it will show up here.</Text>
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
