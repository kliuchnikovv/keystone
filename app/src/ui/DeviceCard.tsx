import { useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Switch, Text, View } from 'react-native';
import { nameProblem } from '../model/automation';
import { Button, ConfirmDelete, ErrorText, Input } from './controls';
import { attrLabel, formatValue } from '../model/format';
import { deviceIcon, deviceLabel, deviceTitle, modeOptions, primarySwitch, stepped, stepperFor } from '../model/devices';
import type { Attr, Entity, Value } from '../protocol/messages';
import { useTheme } from './theme';

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
  // A button's `event` is a 0.4 s pulse: nothing worth showing on a card, so say what it is for.
  const isButton = entity.device === 'button';
  const rows = entity.attrs.filter((a) => a !== sw && a.name !== 'event');

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
        <Pressable accessibilityRole="button" accessibilityLabel={`Rename ${title}`} onPress={() => setRenaming((r) => !r)} style={styles.edit}>
          <Text style={{ color: t.muted, fontSize: 16 }}>✎</Text>
        </Pressable>
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
      {renaming && (
        <RenameBox
          current={entity.name}
          onDone={() => setRenaming(false)}
          onSave={onRename}
        />
      )}
      {renaming && (onIdentify || onRemove) && (
        <DeviceActions title={title} onIdentify={onIdentify} onRemove={onRemove} />
      )}
      {isButton && (
        <Text style={{ color: t.muted, fontSize: 13, marginTop: 8 }}>
          Press it to start automations (rules can react to this button).
        </Text>
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
                style={[styles.chip, { borderColor: on ? t.accent : t.border, backgroundColor: on ? t.accent : 'transparent' }]}
              >
                <Text style={{ color: on ? '#ffffff' : t.text, fontSize: 13 }}>{m.label}</Text>
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
  stepValue: { fontSize: 15, fontWeight: '500', minWidth: 72, textAlign: 'center' },
  offline: { opacity: 0.6 },
  edit: { paddingHorizontal: 10, paddingVertical: 6 },
  rename: { marginTop: 12, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth },
  actions: { marginTop: 4 },
  renameButtons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 10, flexWrap: 'wrap' },
  stepper: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  modes: { flexDirection: 'row', gap: 6, flexWrap: 'wrap', justifyContent: 'flex-end', flex: 1 },
  chip: { borderWidth: 1, borderRadius: 14, paddingHorizontal: 10, paddingVertical: 5 },
  step: { width: 34, height: 34, borderRadius: 17, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
});
