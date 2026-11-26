// The thermostat's ring: the target is a dial, not a button.
//
// The arc shows how far the current temperature is from the scale; the target
// sits in the middle with the current above it.
import { StyleSheet, Text, View } from './kit';
import { useTheme } from './theme';

export interface ThermostatRingProps {
  /** The temperature now, in °C. */
  current: number;
  /** The temperature asked for, in °C. */
  target: number;
  /** The scale the ring draws: from the coldest to the hottest. */
  min?: number;
  max?: number;
}

/** The arc, drawn as a ring of segments — no SVG needed, and it works on the web. */
export function ThermostatRing({ current, target, min = 5, max = 35 }: ThermostatRingProps) {
  const t = useTheme();
  const size = 128;
  const stroke = 10;
  const radius = (size - stroke) / 2;
  const span = Math.max(1, max - min);
  // Where the current temperature sits on the scale, as a turn.
  const turn = Math.max(0, Math.min(1, (current - min) / span));
  // The scale runs three quarters of the way round, leaving a gap at the bottom.
  const arc = 0.75;
  const segments = 36;

  return (
    <View style={styles.box} accessibilityLabel={`Thermostat, now ${current.toFixed(1)} degrees, target ${target.toFixed(1)} degrees`}>
      <View style={[styles.ring, { width: size, height: size, borderRadius: size / 2, borderColor: t.fill }]}>
        {Array.from({ length: segments }, (_, i) => {
          const at = i / segments;
          const lit = at <= turn * arc;
          return (
            <View
              key={i}
              style={[
                styles.tick,
                {
                  backgroundColor: lit ? t.accent : t.border,
                  transform: [{ rotate: `${at * 360 - 135}deg` }, { translateY: -radius }],
                },
              ]}
            />
          );
        })}
        <View style={styles.middle}>
          <Text style={[styles.current, { color: t.text }]}>{current.toFixed(1)}°</Text>
          <Text style={[styles.target, { color: t.accentInk }]}>target {target.toFixed(0)}°</Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  box: { alignItems: 'center', marginVertical: 4 },
  ring: { alignItems: 'center', justifyContent: 'center', borderWidth: 0 },
  tick: { position: 'absolute', width: 3, height: 8, borderRadius: 2 },
  middle: { alignItems: 'center' },
  current: { fontSize: 30, fontWeight: '700', letterSpacing: -1 },
  target: { fontSize: 11.5, fontWeight: '600' },
});
