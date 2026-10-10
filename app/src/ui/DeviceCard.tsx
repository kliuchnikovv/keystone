import { ActivityIndicator, Pressable, StyleSheet, Switch, Text, View } from 'react-native';
import { attrLabel, brightnessPercent, formatValue } from '../model/format';
import { deviceIcon, deviceLabel, deviceTitle, primarySwitch, stepped, stepperFor } from '../model/devices';
import type { Attr, Entity, Value } from '../protocol/messages';
import { useTheme } from './theme';

export interface DeviceCardProps {
  entity: Entity;
  reachable: boolean;
  isPending(attr: number): boolean;
  onSet(attr: number, value: Value): void;
}

export function DeviceCard({ entity, reachable, isPending, onSet }: DeviceCardProps) {
  const t = useTheme();
  const sw = primarySwitch(entity);
  const title = deviceTitle(entity);
  const rows = entity.attrs.filter((a) => a !== sw);

  return (
    <View
      style={[styles.card, { backgroundColor: t.card, borderColor: t.border }, !reachable && styles.offline]}
      accessibilityLabel={`Device ${title}`}
    >
      <View style={styles.head}>
        <Text style={styles.icon}>{deviceIcon(entity.device)}</Text>
        <View style={styles.titles}>
          <Text style={[styles.title, { color: t.text }]} numberOfLines={1}>
            {title}
          </Text>
          <Text style={{ color: reachable ? t.muted : t.bad }}>
            {deviceLabel(entity.device)} · {reachable ? 'online' : 'offline'}
          </Text>
        </View>
        {sw && (
          <View style={styles.switchBox}>
            {isPending(sw.id) && <ActivityIndicator size="small" color={t.accent} />}
            <Switch
              accessibilityLabel={`${title} power`}
              value={sw.value === true}
              disabled={!reachable || isPending(sw.id)}
              onValueChange={(v) => onSet(sw.id, v)}
              trackColor={{ true: t.accent, false: t.border }}
              thumbColor="#ffffff"
              // react-native-web only: the thumb colour of the "on" state (default is teal).
              {...({ activeThumbColor: '#ffffff' } as object)}
            />
          </View>
        )}
      </View>
      {rows.length > 0 && (
        <View style={[styles.rows, { borderTopColor: t.border }]}>
          {rows.map((a) => (
            <AttrRow key={a.id} attr={a} title={title} reachable={reachable} pending={isPending(a.id)} onSet={onSet} />
          ))}
        </View>
      )}
    </View>
  );
}

function AttrRow(props: { attr: Attr; title: string; reachable: boolean; pending: boolean; onSet(attr: number, v: Value): void }) {
  const t = useTheme();
  const { attr: a, title, reachable, pending, onSet } = props;
  const canStep = stepperFor(a) !== null && reachable && !pending;
  const down = stepped(a, -1);
  const up = stepped(a, 1);
  return (
    <View style={styles.row}>
      <Text style={[styles.rowLabel, { color: t.muted }]}>{attrLabel(a.name)}</Text>
      {stepperFor(a) !== null ? (
        <View style={styles.stepper}>
          <StepButton label="−" a11y={`${title} dimmer`} disabled={!canStep || down === null} onPress={() => down !== null && onSet(a.id, down)} />
          <Text style={[styles.stepValue, { color: t.text }]} accessibilityLabel={`${title} brightness`}>
            {typeof a.value === 'number' ? `${brightnessPercent(a.value)} %` : '—'}
          </Text>
          <StepButton label="+" a11y={`${title} brighter`} disabled={!canStep || up === null} onPress={() => up !== null && onSet(a.id, up)} />
        </View>
      ) : (
        <Text style={[styles.rowValue, { color: t.text }]}>{formatValue(a)}</Text>
      )}
    </View>
  );
}

function StepButton(props: { label: string; a11y: string; disabled: boolean; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={props.a11y}
      accessibilityState={{ disabled: props.disabled }}
      disabled={props.disabled}
      onPress={props.onPress}
      style={[styles.step, { borderColor: t.border, opacity: props.disabled ? 0.4 : 1 }]}
    >
      <Text style={{ color: t.text, fontSize: 18 }}>{props.label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  card: { borderRadius: 14, borderWidth: 1, padding: 14, marginBottom: 12 },
  head: { flexDirection: 'row', alignItems: 'center' },
  icon: { fontSize: 28, marginRight: 12 },
  titles: { flex: 1 },
  title: { fontSize: 17, fontWeight: '600' },
  switchBox: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  rows: { marginTop: 12, paddingTop: 10, borderTopWidth: StyleSheet.hairlineWidth, gap: 8 },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  rowLabel: { fontSize: 14 },
  rowValue: { fontSize: 15, fontWeight: '500', textAlign: 'right' },
  stepValue: { fontSize: 15, fontWeight: '500', minWidth: 56, textAlign: 'center' },
  offline: { opacity: 0.6 },
  stepper: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  step: { width: 34, height: 34, borderRadius: 17, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
});
