import { useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Switch, Text, View } from 'react-native';
import { nameProblem } from '../model/automation';
import { Button, ConfirmDelete, ErrorText, Input } from './controls';
import { attrLabel, formatValue } from '../model/format';
import { attrOf, deviceIcon, deviceLabel, deviceTitle, isPowered, meterAttrs, modeOptions, primarySwitch, readingAttrs, stepped, stepperFor } from '../model/devices';
import { Glyph } from './Glyph';
import type { Attr, Entity, Value } from '../protocol/messages';
import { radius, useTheme } from './theme';

export interface DeviceCardProps {
  entity: Entity;
  reachable: boolean;
  isPending(attr: number): boolean;
  onSet(attr: number, value: Value): void;
  /** Give the device a name, or remove it (null). Resolves true when the hub accepted. */
  onRename(name: string | null): Promise<boolean>;
  /** Zigbee devices only: blink the device so it can be found / take it off the network. */
  onIdentify?(): Promise<boolean>;
  onRemove?(): Promise<boolean>;
}

export function DeviceCard({ entity, reachable, isPending, onSet, onRename, onIdentify, onRemove }: DeviceCardProps) {
  const t = useTheme();
  const [renaming, setRenaming] = useState(false);
  const sw = primarySwitch(entity);
  const title = deviceTitle(entity);
  const powered = isPowered(entity);
  // A button's `event` is a 0.4 s pulse: nothing worth showing on a card, so say what it is for.
  const isButton = entity.device === 'button';
  const meters = meterAttrs(entity);
  const readings = readingAttrs(entity);
  const controlled = [sw, ...meters, readings.first, ...readings.rest].filter((a): a is Attr => a !== undefined);
  const rows = entity.attrs.filter((a) => !controlled.includes(a) && a.name !== 'event');

  return (
    <View
      style={[styles.card, { backgroundColor: t.glass, borderColor: t.glassBorder }, !reachable && styles.offline]}
      accessibilityLabel={`Device ${title}`}
    >
      <View style={styles.head}>
        <View style={[styles.chip, { backgroundColor: powered ? t.accent : t.accentSoft }]}>
          <Glyph name={deviceIcon(entity.device)} size={19} color={powered ? t.onAccent : t.accent} filled={powered} />
        </View>
        <View style={styles.titles}>
          <Text style={[styles.title, { color: t.text }]} numberOfLines={1}>
            {title}
          </Text>
          <Text style={{ color: reachable ? t.muted : t.bad }} numberOfLines={1}>
            {subtitle(entity, powered, reachable, meters)}
          </Text>
        </View>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Rename ${title}`}
          onPress={() => setRenaming((r) => !r)}
          style={styles.edit}
          hitSlop={8}
        >
          <Glyph name="pencil" size={15} color={t.muted} />
        </Pressable>
        {sw && (
          <View style={styles.switchBox}>
            {isPending(sw.id) && <ActivityIndicator size="small" color={t.accent} />}
            <Switch
              accessibilityLabel={`${title} power`}
              value={powered}
              disabled={!reachable || isPending(sw.id)}
              onValueChange={(v) => onSet(sw.id, v)}
              trackColor={{ true: t.good, false: t.border }}
              thumbColor="#ffffff"
              // react-native-web only: the thumb colour of the "on" state (default is teal).
              {...({ activeThumbColor: '#ffffff' } as object)}
            />
          </View>
        )}
      </View>

      {renaming && <RenameBox current={entity.name} onDone={() => setRenaming(false)} onSave={onRename} />}
      {renaming && (onIdentify || onRemove) && <DeviceActions title={title} onIdentify={onIdentify} onRemove={onRemove} />}
      {isButton && (
        <Text style={{ color: t.muted, fontSize: 13, marginTop: 8 }}>Press it to start automations (rules can react to this button).</Text>
      )}

      {meters.length > 0 && (
        <View style={[styles.meters, { borderTopColor: t.border }]}>
          {meters.map((a) => (
            <View key={a.id} style={styles.meter}>
              <Text style={{ color: t.muted, fontSize: 13 }}>{attrLabel(a.name)}</Text>
              <View style={styles.meterValue}>
                {a.name === 'power_w' && <Glyph name="bolt" size={11} color={t.accent} />}
                <Text style={{ color: t.text, fontSize: 15, fontWeight: '600' }}>{formatValue(a)}</Text>
              </View>
            </View>
          ))}
        </View>
      )}

      {(readings.first || readings.rest.length > 0) && (
        <View style={[styles.reading, { borderTopColor: t.border }]}>
          {readings.first && <Text style={[styles.bigNumber, { color: t.text }]}>{formatValue(readings.first)}</Text>}
          <View style={styles.readingRest}>
            {readings.rest.map((a) => (
              <View key={a.id} style={styles.readingPill}>
                <Text style={{ color: t.muted, fontSize: 11 }}>{attrLabel(a.name)}</Text>
                <Text style={{ color: t.text, fontSize: 13, fontWeight: '600' }}>{formatValue(a)}</Text>
              </View>
            ))}
          </View>
        </View>
      )}

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

/** One short line under the name: what it is, whether it answers, and what it is doing. */
function subtitle(e: Entity, powered: boolean, reachable: boolean, meters: Attr[]): string {
  const bits: string[] = [deviceLabel(e.device), reachable ? 'online' : 'offline'];
  if (!reachable) return bits.join(' · ');
  if (primarySwitch(e) !== undefined) bits.push(powered ? 'On' : 'Off');
  const watt = meters.find((a) => a.name === 'power_w');
  if (watt) bits.push(formatValue(watt));
  const bright = attrOf(e, 'brightness');
  if (bright) bits.push(formatValue(bright));
  return bits.join(' · ');
}

function RenameBox({ current, onSave, onDone }: { current: string | null; onSave(name: string | null): Promise<boolean>; onDone(): void }) {
  const t = useTheme();
  const [text, setText] = useState(current ?? '');
  const [error, setError] = useState<string | null>(null);
  const save = async () => {
    const problem = nameProblem(text);
    if (problem) return setError(problem);
    setError(null);
    if (await onSave(text)) onDone();
  };
  return (
    <View style={[styles.rename, { borderTopColor: t.border }]}>
      <Input label="Name" value={text} onChangeText={setText} placeholder="e.g. kitchen_lamp" onSubmitEditing={save} />
      <Text style={{ color: t.muted, fontSize: 12, marginTop: 4 }}>Prefix it with a room to group it, e.g. kitchen_lamp.</Text>
      <ErrorText text={error} />
      <View style={styles.renameButtons}>
        {current !== null && <Button kind="danger" label="Remove name" onPress={async () => { if (await onSave(null)) onDone(); }} />}
        <Button kind="plain" label="Cancel" onPress={onDone} />
        <Button label="Save name" onPress={save} />
      </View>
    </View>
  );
}

function DeviceActions({ title, onIdentify, onRemove }: { title: string; onIdentify?(): Promise<boolean>; onRemove?(): Promise<boolean> }) {
  const t = useTheme();
  const [note, setNote] = useState<string | null>(null);
  return (
    <View style={styles.actions}>
      <View style={styles.renameButtons}>
        {onIdentify && (
          <Button
            kind="plain"
            label="Blink"
            a11y={`Identify ${title}`}
            onPress={async () => setNote((await onIdentify()) ? 'The device should blink for about 10 seconds.' : null)}
          />
        )}
        {onRemove && <ConfirmDelete label="Remove device" a11y={`Remove ${title}`} onConfirm={() => void onRemove()} />}
      </View>
      {note && <Text style={{ color: t.muted, fontSize: 12, textAlign: 'right' }}>{note}</Text>}
    </View>
  );
}

function AttrRow(props: { attr: Attr; title: string; reachable: boolean; pending: boolean; onSet(attr: number, v: Value): void }) {
  const t = useTheme();
  const { attr: a, title, reachable, pending, onSet } = props;
  const stepper = stepperFor(a);
  const modes = modeOptions(a);
  const canStep = stepper !== null && reachable && !pending;
  const down = stepped(a, -1);
  const up = stepped(a, 1);
  return (
    <View style={styles.row}>
      <Text style={[styles.rowLabel, { color: t.muted }]}>{attrLabel(a.name)}</Text>
      {stepper !== null ? (
        <View style={styles.stepper}>
          <StepButton label="−" a11y={`${title} ${stepper.down}`} disabled={!canStep || down === null} onPress={() => down !== null && onSet(a.id, down)} />
          <Text style={[styles.stepValue, { color: t.text }]} accessibilityLabel={`${title} ${stepper.noun}`}>
            {formatValue(a)}
          </Text>
          <StepButton label="+" a11y={`${title} ${stepper.up}`} disabled={!canStep || up === null} onPress={() => up !== null && onSet(a.id, up)} />
        </View>
      ) : modes !== null ? (
        <View style={styles.modes}>
          {modes.map((m) => {
            const on = a.value === m.value;
            return (
              <Pressable
                key={m.value}
                accessibilityRole="button"
                accessibilityLabel={`${title} mode ${m.label.toLowerCase()}`}
                accessibilityState={{ selected: on, disabled: !reachable || pending }}
                disabled={!reachable || pending}
                onPress={() => onSet(a.id, m.value)}
                style={[styles.chipPill, { backgroundColor: on ? t.accent : 'transparent', borderColor: on ? t.accent : t.border }]}
              >
                <Text style={{ color: on ? t.onAccent : t.text, fontSize: 13 }}>{m.label}</Text>
              </Pressable>
            );
          })}
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
      style={[styles.step, { borderColor: t.border, backgroundColor: t.accentSoft, opacity: props.disabled ? 0.4 : 1 }]}
    >
      <Text style={{ color: t.text, fontSize: 18 }}>{props.label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  card: { borderRadius: radius.card, borderWidth: 1, padding: 14, marginBottom: 10 },
  head: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  chip: { width: 38, height: 38, borderRadius: 11, alignItems: 'center', justifyContent: 'center' },
  titles: { flex: 1, minWidth: 0 },
  title: { fontSize: 16, fontWeight: '600' },
  edit: { padding: 4 },
  switchBox: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  offline: { opacity: 0.6 },
  meters: { marginTop: 12, paddingTop: 10, borderTopWidth: StyleSheet.hairlineWidth, gap: 4 },
  meter: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  meterValue: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  reading: { marginTop: 12, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth },
  bigNumber: { fontSize: 34, fontWeight: '700', letterSpacing: -0.02 },
  readingRest: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 },
  readingPill: { paddingHorizontal: 10, paddingVertical: 6, borderRadius: radius.chip },
  rows: { marginTop: 12, paddingTop: 10, borderTopWidth: StyleSheet.hairlineWidth, gap: 8 },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  rowLabel: { fontSize: 14 },
  rowValue: { fontSize: 15, fontWeight: '500', textAlign: 'right' },
  stepValue: { fontSize: 15, fontWeight: '600', minWidth: 72, textAlign: 'center' },
  rename: { marginTop: 12, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth },
  actions: { marginTop: 4 },
  renameButtons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 10, flexWrap: 'wrap' },
  stepper: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  modes: { flexDirection: 'row', gap: 6, flexWrap: 'wrap', justifyContent: 'flex-end', flex: 1 },
  chipPill: { borderWidth: 1, borderRadius: radius.pill, paddingHorizontal: 10, paddingVertical: 5 },
  step: { width: 34, height: 34, borderRadius: 17, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
});