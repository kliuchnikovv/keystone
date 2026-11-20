// Several readings at once: a grid of small cells, each with its own threshold.
//
// A sensor that reports four numbers is not four cards — it is one card with
// four numbers in it, each coloured by whether it is normal.
import { StyleSheet, Text, View } from './kit';
import { attrLabel, formatValue } from '../model/format';
import { toneFor } from '../model/thresholds';
import type { Attr, Entity } from '../protocol/messages';
import { useTheme } from './theme';

export interface ReadingsGridProps {
  entity: Entity;
  /** The readings to show; by default every reading but the battery. */
  attrs?: Attr[];
}

/** The readings worth putting on a card. */
export function gridReadings(entity: Entity): Attr[] {
  return entity.attrs.filter((a) => a.flags.includes('r') && a.value !== null && a.name !== 'battery');
}

/** One cell: the name, the number, and whether it is normal. */
export function ReadingCell({ attr }: { attr: Attr }) {
  const t = useTheme();
  const value = typeof attr.value === 'number' ? attr.value : null;
  const tone = value === null ? 'none' : toneFor(attr.name, value);
  const colour = tone === 'good' ? t.good : tone === 'warn' ? t.warn : t.muted;
  return (
    <View
      accessibilityLabel={`${attrLabel(attr.name)} ${formatValue(attr)}`}
      style={[styles.cell, { backgroundColor: tone === 'warn' ? t.accentSoft : t.fill, borderColor: t.border }]}
    >
      <Text style={[styles.key, { color: t.muted }]} numberOfLines={1}>
        {attrLabel(attr.name)}
      </Text>
      <Text style={[styles.value, { color: tone === 'none' ? t.text : colour }]} numberOfLines={1}>
        {formatValue(attr)}
      </Text>
    </View>
  );
}

export function ReadingsGrid({ entity, attrs }: ReadingsGridProps) {
  const shown = attrs ?? gridReadings(entity);
  if (shown.length === 0) return null;
  return (
    <View style={styles.grid}>
      {shown.map((a) => (
        <ReadingCell key={a.id} attr={a} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 },
  cell: { flexBasis: '47%', flexGrow: 1, borderRadius: 12, borderWidth: 1, paddingHorizontal: 11, paddingVertical: 9, gap: 1 },
  key: { fontSize: 10.5, fontWeight: '600' },
  value: { fontSize: 17, fontWeight: '700' },
});
