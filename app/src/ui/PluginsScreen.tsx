// The plugin store — a shell.
//
// The hub has no plugin mechanism yet, so this screen is the frame of one:
// the tabs, the search, the tags, the cards. Nothing here talks to the hub;
// an *Install* only moves the card to *Installed* for this session.
import { useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, TextInput, View } from './kit';
import { Glyph } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { useTheme } from './theme';

export interface Plugin {
  id: string;
  name: string;
  author: string;
  description: string;
  rating: number;
  installs: number;
  tags: string[];
  /** null = free. */
  price: string | null;
}

const PLUGINS: Plugin[] = [
  { id: 'energy', name: 'Energy', author: 'Keystone', description: 'Watch what the house draws, hour by hour.', rating: 4.8, installs: 12400, tags: ['Energy'], price: null },
  { id: 'air', name: 'Air quality', author: 'Keystone', description: 'CO₂, PM2.5 and humidity on one screen.', rating: 4.6, installs: 8300, tags: ['Devices'], price: null },
  { id: 'weather', name: 'Weather', author: 'Nils A.', description: 'Rain, wind and outside temperature for your rules.', rating: 4.4, installs: 6100, tags: ['Energy'], price: '$2' },
  { id: 'wake', name: 'Good morning', author: 'Keystone', description: 'A scene that follows the sunrise and your alarm.', rating: 4.9, installs: 15800, tags: ['Automations'], price: null },
  { id: 'presence', name: 'Presence', author: 'Marta L.', description: 'Room presence from the motion sensors you already have.', rating: 4.7, installs: 9900, tags: ['Devices', 'Security'], price: '$1' },
  { id: 'guard', name: 'Guard', author: 'Keystone', description: 'Tell the hub to watch the doors while you are away.', rating: 4.3, installs: 4200, tags: ['Security'], price: '$3' },
  { id: 'voice', name: 'Voice', author: 'Iris K.', description: 'Say it out loud: the hub reads the house state.', rating: 4.2, installs: 2700, tags: ['Voice'], price: null },
  { id: 'dash', name: 'Dashboards', author: 'Keystone', description: 'Build a screen of the numbers you actually look at.', rating: 4.5, installs: 7600, tags: ['Dashboards'], price: '$2' },
];

const TAGS = ['All', 'Devices', 'Energy', 'Voice', 'Dashboards', 'Automations', 'Security'];

export function PluginsScreen({ onBack }: { onBack(): void }) {
  const t = useTheme();
  const [tab, setTab] = useState<'browse' | 'installed'>('browse');
  const [query, setQuery] = useState('');
  const [tag, setTag] = useState('All');
  const [installed, setInstalled] = useState<string[]>([]);

  const shown = PLUGINS.filter((p) => {
    if (tab === 'installed') return installed.includes(p.id);
    if (tag !== 'All' && !p.tags.includes(tag)) return false;
    if (query.trim() === '') return true;
    const q = query.trim().toLowerCase();
    return p.name.toLowerCase().includes(q) || p.description.toLowerCase().includes(q) || p.author.toLowerCase().includes(q);
  });

  return (
    <ScrollView contentContainerStyle={styles.page} keyboardShouldPersistTaps="handled">
      <ScreenHeader
        title="Plugins"
        left={
          <Pressable accessibilityRole="button" accessibilityLabel="Settings" onPress={onBack} style={styles.back}>
            <Glyph name="back" size={20} color={t.muted} />
          </Pressable>
        }
      />

      <View style={styles.tabs}>
        <Pressable
          accessibilityRole="tab"
          accessibilityLabel="Browse"
          accessibilityState={{ selected: tab === 'browse' }}
          onPress={() => setTab('browse')}
          style={[styles.tab, tab === 'browse' ? { backgroundColor: t.card } : null]}
        >
          <Text style={{ color: tab === 'browse' ? t.text : t.muted, fontSize: 12.5, fontWeight: '600' }}>Browse</Text>
        </Pressable>
        <Pressable
          accessibilityRole="tab"
          accessibilityLabel="Installed"
          accessibilityState={{ selected: tab === 'installed' }}
          onPress={() => setTab('installed')}
          style={[styles.tab, tab === 'installed' ? { backgroundColor: t.card } : null]}
        >
          <Text style={{ color: tab === 'installed' ? t.text : t.muted, fontSize: 12.5, fontWeight: '600' }}>
            Installed{installed.length > 0 ? ` · ${installed.length}` : ''}
          </Text>
        </Pressable>
      </View>

      {tab === 'browse' ? (
        <>
          <TextInput
            accessibilityLabel="Search plugins"
            placeholder="Search plugins"
            placeholderTextColor={t.muted}
            value={query}
            onChangeText={setQuery}
            style={[styles.search, { backgroundColor: t.bg, borderColor: t.border, color: t.text }]}
          />
          <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.tags}>
            {TAGS.map((x) => {
              const on = x === tag;
              return (
                <Pressable
                  key={x}
                  accessibilityRole="button"
                  accessibilityLabel={`Tag ${x}`}
                  accessibilityState={{ selected: on }}
                  onPress={() => setTag(x)}
                  style={[styles.tag, { backgroundColor: on ? t.accentSoft : t.card, borderColor: on ? t.accent : t.border }]}
                >
                  <Text style={{ color: on ? t.accentInk : t.muted, fontSize: 12, fontWeight: '600' }}>{x}</Text>
                </Pressable>
              );
            })}
          </ScrollView>
        </>
      ) : null}

      {shown.length === 0 ? (
        <Text style={[styles.empty, { color: t.muted }]}>
          {tab === 'installed' ? 'No plugins yet' : 'Nothing found'}
        </Text>
      ) : (
        shown.map((p) => (
          <View key={p.id} accessibilityLabel={`Plugin ${p.name}`} style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
            <View style={styles.cardTop}>
              <View style={[styles.tile, { backgroundColor: t.accentSoft }]}>
                <Glyph name="chip" size={20} color={t.accentInk} />
              </View>
              <View style={styles.cardText}>
                <View style={styles.nameLine}>
                  <Text style={[styles.name, { color: t.text }]} numberOfLines={1}>
                    {p.name}
                  </Text>
                  {p.price === null ? <Text style={[styles.badge, { color: t.accentInk, backgroundColor: t.accentSoft }]}>Free</Text> : null}
                </View>
                <Text style={[styles.author, { color: t.muted }]} numberOfLines={1}>
                  {p.author}
                </Text>
              </View>
            </View>
            <Text style={[styles.desc, { color: t.muted }]} numberOfLines={2}>
              {p.description}
            </Text>
            <View style={styles.meta}>
              <Text style={[styles.metaText, { color: t.muted }]}>★ {p.rating.toFixed(1)}</Text>
              <Text style={[styles.metaText, { color: t.muted }]}>
                {p.installs >= 1000 ? `${Math.round(p.installs / 1000)}k installs` : `${p.installs} installs`}
              </Text>
              <Pressable
                accessibilityRole="button"
                accessibilityLabel={`Install ${p.name}`}
                onPress={() => setInstalled((cur) => (cur.includes(p.id) ? cur : [...cur, p.id]))}
                style={[styles.install, { backgroundColor: t.primary }]}
              >
                <Text style={{ color: t.onPrimary, fontSize: 12.5, fontWeight: '600' }}>
                  {installed.includes(p.id) ? 'Installed' : 'Install'}
                </Text>
              </Pressable>
            </View>
          </View>
        ))
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: 40, gap: 10 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
  tabs: { flexDirection: 'row', backgroundColor: 'transparent', borderRadius: 999, padding: 3, gap: 4 },
  tab: { flex: 1, alignItems: 'center', paddingVertical: 9, borderRadius: 999 },
  search: { borderWidth: 1, borderRadius: 12, paddingHorizontal: 14, paddingVertical: 11, fontSize: 15 },
  tags: { gap: 8, paddingRight: 8 },
  tag: { paddingHorizontal: 13, paddingVertical: 8, borderRadius: 999, borderWidth: 1 },
  card: { borderWidth: 1, borderRadius: 18, padding: 13, gap: 8 },
  cardTop: { flexDirection: 'row', alignItems: 'center', gap: 11 },
  cardText: { flex: 1, gap: 2, minWidth: 0 },
  nameLine: { flexDirection: 'row', alignItems: 'center', gap: 7 },
  name: { fontSize: 14.5, fontWeight: '600', flexShrink: 1 },
  badge: { fontSize: 10, fontWeight: '700', paddingHorizontal: 7, paddingVertical: 3, borderRadius: 999 },
  author: { fontSize: 11.5 },
  tile: { width: 40, height: 40, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  desc: { fontSize: 12.5, lineHeight: 17 },
  meta: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  metaText: { fontSize: 11.5, fontWeight: '600' },
  install: { marginLeft: 'auto', paddingHorizontal: 14, paddingVertical: 8, borderRadius: 999 },
  empty: { textAlign: 'center', paddingVertical: 28, fontSize: 13 },
});
