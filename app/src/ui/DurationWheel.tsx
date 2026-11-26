// The duration wheel: the picker a phone uses for «how long».
//
// Two columns — hours and minutes — that scroll and snap. The chosen value
// sits in the band in the middle; the rows fade towards the edges so the eye
// lands on the band first.
import { useEffect, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, View, type NativeScrollEvent, type NativeSyntheticEvent } from './kit';
import { useTheme } from './theme';

/** The height of the band in the middle: one item plus a little air. */
export const BAND_HEIGHT = 38;
/** The height of one item. */
export const ITEM_HEIGHT = 34;
/** How many items sit above and below the band. */
export const VISIBLE = 2;

/**
 * The item under the band, given how far the list is scrolled.
 *
 * The band's centre sits at `offset + band/2` in the content; the item whose
 * own centre is there is the chosen one.
 */
export function wheelValue(offset: number, band: number, item: number): number {
  if (item <= 0) return 0;
  const centre = offset + band / 2;
  return Math.max(0, Math.round((centre - item / 2) / item));
}

/** The hours a wheel shows: 0 to 23. */
export function hoursAt(index: number): number {
  return ((index % 24) + 24) % 24;
}

/** The minutes a wheel shows: every five, wrapping round. */
export function minutesAt(index: number): number {
  return (((index % 12) + 12) % 12) * 5;
}

/** One column of the wheel. */
function Column({
  values,
  selected,
  onChange,
  label,
  unit,
}: {
  values: number[];
  selected: number;
  onChange(v: number): void;
  label: string;
  unit: string;
}) {
  const t = useTheme();
  const ref = useRef<ScrollView>(null);
  const [offset, setOffset] = useState(0);

  // The scroll position tells which value is in the band.
  const onScroll = (e: NativeSyntheticEvent<NativeScrollEvent>) => {
    // On the phone the offset comes in contentOffset; on the web it is the
    // element's scrollTop.
    const native = e.nativeEvent as unknown as {
      contentOffset?: { y?: number };
      scrollTop?: number;
    };
    const y = native.contentOffset?.y ?? native.scrollTop ?? 0;
    setOffset(y);
    const index = wheelValue(y, BAND_HEIGHT, ITEM_HEIGHT);
    const value = values[index % values.length];
    if (value !== undefined && value !== selected) onChange(value);
  };

  // Keep the chosen value in the band when it changes from outside.
  useEffect(() => {
    const index = values.indexOf(selected);
    if (index >= 0) ref.current?.scrollTo({ y: index * ITEM_HEIGHT, animated: true });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selected]);

  const height = BAND_HEIGHT + VISIBLE * 2 * ITEM_HEIGHT;

  return (
    <View style={styles.column} accessibilityLabel={label}>
      <View style={[styles.band, { height: BAND_HEIGHT, backgroundColor: t.accentSoft, borderColor: t.border }]} />
      <ScrollView
        ref={ref}
        showsVerticalScrollIndicator={false}
        snapToInterval={ITEM_HEIGHT}
        decelerationRate="fast"
        contentContainerStyle={{ paddingVertical: VISIBLE * ITEM_HEIGHT }}
        onScroll={onScroll}
        scrollEventThrottle={16}
        style={{ height }}
        testID={`wheel-${label}`}
      >
        {values.map((v, i) => {
          const distance = Math.abs(i - wheelValue(offset, BAND_HEIGHT, ITEM_HEIGHT));
          const dim = distance === 0 ? 1 : distance === 1 ? 0.55 : 0.25;
          return (
            <View key={`${v}-${i}`} style={[styles.item, { height: ITEM_HEIGHT, opacity: dim }]}>
              <Text style={[styles.value, { color: t.text }]}>
                {v}
                <Text style={[styles.unit, { color: t.muted }]}> {unit}</Text>
              </Text>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );
}

export interface DurationWheelProps {
  hours: number;
  minutes: number;
  onChange(hours: number, minutes: number): void;
}

export function DurationWheel({ hours, minutes, onChange }: DurationWheelProps) {
  const hourValues = Array.from({ length: 24 }, (_, i) => i);
  const minuteValues = Array.from({ length: 12 }, (_, i) => i * 5);
  return (
    <View style={styles.wheel}>
      <Column
        label="hours"
        unit="hours"
        values={hourValues}
        selected={hours}
        onChange={(h) => onChange(h, minutes)}
      />
      <Column
        label="minutes"
        unit="min"
        values={minuteValues}
        selected={minutes}
        onChange={(m) => onChange(hours, m)}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  wheel: { flexDirection: 'row', justifyContent: 'center', gap: 10, marginTop: 6 },
  column: { width: 120, alignItems: 'center' },
  band: {
    position: 'absolute',
    left: 8,
    right: 8,
    top: 0,
    borderRadius: 10,
    borderWidth: StyleSheet.hairlineWidth,
  },
  item: { alignItems: 'center', justifyContent: 'center' },
  value: { fontSize: 22, fontWeight: '700' },
  unit: { fontSize: 12, fontWeight: '600' },
});
