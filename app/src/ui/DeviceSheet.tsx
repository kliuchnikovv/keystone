// Everything about one device, in a sheet: turn it on or off, dim it, colour it,
// what it measures, and the three things you do to a device you rarely do —
// rename it, make it blink, take it off the network.
import { useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';
import { hsvToCss, xyToHsv } from '../model/color';
import { MAX_NAME_CHARS, nameProblem } from '../model/names';
import { attrLabel, formatValue } from '../model/format';
import {
  attrOf,
  colorAttr,
  deviceIcon,
  deviceLabel,
  deviceTitle,
  isKnownOff,
  isPowered,
  meterAttrs,
  modeOptions,
  primarySwitch,
  readingAttrs,
  sliderFor,
  stepped,
  stepperFor,
  type DeviceStatus,
} from '../model/devices';
import type { Attr, DeviceOtaSession, Entity, Value } from '../protocol/messages';
import { Button, ErrorText, Input } from './controls';
import { ColorSheet } from './ColorSheet';
import { FirmwareUpdatePanel, OtaProgress } from './FirmwareUpdatePanel';
import { Glyph } from './Glyph';
import { Sheet, SheetRow } from './Sheet';
import { Slider } from './Slider';
import { useTheme } from './theme';

export interface DeviceSheetProps {
  entity: Entity;
  status: DeviceStatus;
  reachable: boolean;
  visible: boolean;
  isPending(attr: number): boolean;
  onSet(attr: number, value: Value): void;
  onClose(): void;
  /** Give the device a name, or remove it (null). Resolves true when the hub accepted. */
  onRename(name: string | null): Promise<boolean>;
  /** Zigbee devices only: blink the device so it can be found / take it off the network. */
  onIdentify?(): Promise<boolean>;
  /** Zigbee devices only: push a vendor firmware file to the device. Resolves true when the hub accepted it. */
  onStartUpdate?(url: string, sha256: string): Promise<boolean>;
  /** The last firmware session the hub reported for this device. */
  ota?: DeviceOtaSession;
  /** Ask to take the device off the network — the caller confirms. */
  onRemove?(): void;
  notify?(text: string): void;
  /** "updated 12 s ago" — the app's own memory of the last change. */
  updatedText?: string | null;
  /** "Last pressed: 2 min ago" (buttons only). */
  pressedText?: string | null;
}

export function DeviceSheet({
  entity,
  status,
  reachable,
  visible,
  isPending,
  onSet,
  onClose,
  onRename,
  onIdentify,
  onStartUpdate,
  ota,
  onRemove,
  notify,
  updatedText,
  pressedText,
}: DeviceSheetProps) {
  const t = useTheme();
  const [renaming, setRenaming] = useState(false);
  const [updating, setUpdating] = useState(false);
  const sw = primarySwitch(entity);
  const title = deviceTitle(entity);
  const powered = isPowered(entity);
  // A dimmer of a light that is off has nothing to say — but only if we know it is off.
  const off = isKnownOff(entity);
  // A button's `event` is a 0.4 s pulse: nothing worth showing, so say what it is for.
  const isButton = entity.device === 'button';
  const meters = meterAttrs(entity);
  const readings = readingAttrs(entity);
  const controlled = [sw, ...meters, readings.first, ...readings.rest].filter((a): a is Attr => a !== undefined);
  const colour = colorAttr(entity);
  const rows = entity.attrs.filter((a) => !controlled.includes(a) && a.name !== 'event' && a !== colour);
  const sliders = rows.filter((a) => sliderFor(a) !== null);
  const plain = rows.filter((a) => sliderFor(a) === null);

  // A command that just went out: a short outline so the eye sees that it happened.
  const pingTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (pingTimer.current) clearTimeout(pingTimer.current); }, []);
  const send = (attr: number, value: Value) => {
    onSet(attr, value);
    if (pingTimer.current) clearTimeout(pingTimer.current);
    pingTimer.current = setTimeout(() => undefined, 500);
  };

  return (
    <Sheet visible={visible} title={title} onCancel={onClose}>
      <Text style={[styles.subtitle, { color: t.muted }]}>
        {[deviceLabel(entity.device), powered ? 'On' : sw ? 'Off' : 'reading', status === 'online' ? null : status]
          .filter((part): part is string => part !== null)
          .join(' · ')}
      </Text>

      {sw && (
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`${powered ? 'Turn off' : 'Turn on'} ${title}`}
          disabled={!reachable || isPending(sw.id)}
          onPress={() => send(sw.id, !powered)}
          style={[styles.toggle, { borderColor: t.border, backgroundColor: t.fill, opacity: reachable ? 1 : 0.5 }]}
        >
          <Glyph name={deviceIcon(entity.device)} size={20} color={powered ? t.accent : t.muted} filled={powered} />
          <Text style={[styles.toggleText, { color: t.text }]}>{powered ? 'Turn off' : 'Turn on'}</Text>
          {isPending(sw.id) && <ActivityIndicator size="small" color={t.accent} />}
        </Pressable>
      )}

      {isButton && (
        <Text style={[styles.note, { color: t.muted }]}>This button has no settings here — it can trigger rules.</Text>
      )}
      {isButton && pressedText ? (
        <Text style={[styles.note, { color: t.muted }]}>Last pressed: {pressedText}</Text>
      ) : null}

      {meters.length > 0 && (
        <View style={[styles.block, { borderTopColor: t.border }]}>
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
        <View style={[styles.block, { borderTopColor: t.border }]}>
          {readings.first && <Text style={[styles.bigNumber, { color: t.text }]}>{formatValue(readings.first)}</Text>}
          <View style={styles.readingRest}>
            {readings.rest.map((a) => (
              <View key={a.id} style={styles.readingPill}>
                <Text style={{ color: t.muted, fontSize: 11 }}>{attrLabel(a.name)}</Text>
                <Text style={{ color: t.text, fontSize: 13, fontWeight: '600' }}>{formatValue(a)}</Text>
              </View>
            ))}
          </View>
          {updatedText ? <Text style={[styles.updated, { color: t.muted }]}>updated {updatedText}</Text> : null}
        </View>
      )}

      {colour && (
        <ColorRow attr={colour} title={title} reachable={reachable} off={off} onSet={send} />
      )}

      {sliders.length > 0 && (
        <View style={[styles.block, { borderTopColor: t.border }]}>
          {sliders.map((a) => (
            <SliderRow key={a.id} attr={a} title={title} reachable={reachable} pending={isPending(a.id)} off={off} onSet={send} />
          ))}
        </View>
      )}

      {plain.length > 0 && (
        <View style={[styles.block, { borderTopColor: t.border }]}>
          {plain.map((a) => (
            <AttrRow key={a.id} attr={a} title={title} reachable={reachable} pending={isPending(a.id)} onSet={send} />
          ))}
        </View>
      )}

      {renaming && <RenameBox current={entity.name} onDone={() => setRenaming(false)} onSave={onRename} />}

      <View style={[styles.rows, { borderTopColor: t.border }]}>
        <SheetRow
          label="Rename"
          a11y={`Rename ${title}`}
          onPress={() => setRenaming(true)}
        />
        {onIdentify ? (
          <SheetRow
            label="Blink"
            a11y={`Identify ${title}`}
            onPress={async () => {
              onClose();
              if (await onIdentify()) notify?.('The device should blink for about 10 seconds.');
            }}
          />
        ) : null}
        {onStartUpdate ? (
          <SheetRow
            label="Update firmware"
            last={!onRemove}
            a11y={`Update firmware ${title}`}
            onPress={() => setUpdating(true)}
          />
        ) : null}
        {onRemove ? (
          <SheetRow
            label="Remove device"
            tone="danger"
            last
            a11y={`Remove ${title}`}
            onPress={() => {
              onClose();
              onRemove();
            }}
          />
        ) : null}
      </View>

      {updating && onStartUpdate ? (
        <FirmwareUpdatePanel
          title={title}
          session={ota}
          onStart={onStartUpdate}
          onClose={() => setUpdating(false)}
        />
      ) : null}

      {ota && ota.phase !== 'idle' && !updating ? (
        <View style={[styles.block, { borderTopColor: t.border }]}>
          <OtaProgress session={ota} />
        </View>
      ) : null}
    </Sheet>
  );
}

/** A slider for brightness or colour temperature: the thumb follows the finger, the
 * command goes out when the finger lifts. */
function SliderRow(props: { attr: Attr; title: string; reachable: boolean; pending: boolean; off: boolean; onSet(attr: number, v: Value): void }) {
  const t = useTheme();
  const { attr: a, title, reachable, pending, off, onSet } = props;
  const spec = sliderFor(a);
  const [local, setLocal] = useState<number | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const hub = spec && typeof a.value === 'number' ? spec.show(a.value) : 0;
  const shown = local ?? hub;

  // The hub caught up with what we asked for: let its value show again.
  useEffect(() => {
    if (local !== null && Math.abs(local - hub) < 0.001) setLocal(null);
  }, [hub, local]);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);

  if (!spec) return null;
  const commit = (v: number) => {
    onSet(a.id, spec.wire(v));
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => setLocal(null), 3000);
  };
  // Big − and + buttons: a step is worth a tenth of the range (10 % of brightness).
  const stepBy = (dir: 1 | -1) => {
    const next = Math.min(spec.max, Math.max(spec.min, shown + dir * spec.step));
    setLocal(next);
    commit(next);
  };

  return (
    <View>
      <Slider
        label={attrLabel(a.name)}
        a11yLabel={`${title} ${spec.noun}`}
        value={shown}
        min={spec.min}
        max={spec.max}
        step={spec.step}
        disabled={!reachable || pending || off}
        onChange={(v) => setLocal(v)}
        onCommit={commit}
        format={spec.text}
        stepper={{
          onDown: () => stepBy(-1),
          onUp: () => stepBy(1),
          downLabel: `${title} ${spec.noun} down`,
          upLabel: `${title} ${spec.noun} up`,
        }}
      />
      {pending && <Text style={{ color: t.muted, fontSize: 11, marginTop: 4 }}>Sending…</Text>}
    </View>
  );
}

/** The colour well: tap it and pick a colour in a sheet. */
function ColorRow(props: { attr: Attr; title: string; reachable: boolean; off: boolean; onSet(attr: number, v: Value): void }) {
  const t = useTheme();
  const { attr: a, title, reachable, off, onSet } = props;
  const [open, setOpen] = useState(false);
  const xy = Array.isArray(a.value) ? ([a.value[0], a.value[1]] as [number, number]) : null;

  return (
    <View style={[styles.block, { borderTopColor: t.border }]}>
      <View style={styles.row}>
        <Text style={[styles.rowLabel, { color: t.muted }]}>Color</Text>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`${title} colour`}
          disabled={!reachable || off}
          onPress={() => setOpen(true)}
          style={[styles.well, { backgroundColor: xy ? hsvToCss(xyToHsv(xy[0], xy[1])) : t.border, opacity: reachable ? 1 : 0.5 }]}
        />
      </View>
      <ColorSheet
        visible={open}
        title={`${title} colour`}
        value={xy}
        onCancel={() => setOpen(false)}
        onApply={(pair) => onSet(a.id, pair)}
      />
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
                <Text style={{ color: on ? t.onAccent : t.text, fontSize: 12, fontWeight: '500' }}>{m.label}</Text>
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
    <View style={[styles.block, { borderTopColor: t.border }]}>
      <Input
        label="Name"
        value={text}
        onChangeText={setText}
        placeholder="e.g. Living room lamp"
        maxLength={MAX_NAME_CHARS}
        autoFocus
        onSubmitEditing={save}
      />
      <Text style={{ color: t.muted, fontSize: 12, marginTop: 4 }}>
        Up to {MAX_NAME_CHARS} characters, spaces allowed. Start with a room to group it, e.g. Living room lamp.
      </Text>
      <ErrorText text={error} />
      <View style={styles.renameButtons}>
        {current !== null && <Button kind="danger" size="compact" label="Remove name" onPress={async () => { if (await onSave(null)) onDone(); }} />}
        <Button kind="secondary" size="compact" label="Cancel" onPress={onDone} />
        <Button size="compact" label="Save name" a11y="Save name" onPress={save} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  subtitle: { fontSize: 13, marginTop: 2, marginBottom: 10 },
  toggle: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    borderWidth: 1,
    borderRadius: 14,
    paddingHorizontal: 16,
    paddingVertical: 15,
  },
  toggleText: { fontSize: 16, fontWeight: '600', flex: 1 },
  note: { fontSize: 13, marginTop: 8, lineHeight: 19 },
  block: { marginTop: 12, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth, gap: 8 },
  meter: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  meterValue: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  bigNumber: { fontSize: 34, fontWeight: '700', letterSpacing: -0.02 },
  readingRest: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 },
  readingPill: { paddingHorizontal: 10, paddingVertical: 6, borderRadius: 11 },
  updated: { fontSize: 12, marginTop: 8 },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  rowLabel: { fontSize: 14 },
  rowValue: { fontSize: 15, fontWeight: '500', textAlign: 'right' },
  stepValue: { fontSize: 15, fontWeight: '600', minWidth: 72, textAlign: 'center' },
  rows: { marginTop: 12, paddingTop: 4, borderTopWidth: StyleSheet.hairlineWidth },
  renameButtons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 10, flexWrap: 'wrap' },
  stepper: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  modes: { flexDirection: 'row', gap: 6, flexWrap: 'wrap', justifyContent: 'flex-end', flex: 1 },
  chipPill: { borderWidth: 1, borderRadius: 999, paddingHorizontal: 9, paddingVertical: 4 },
  step: { width: 34, height: 34, borderRadius: 17, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
  well: { width: 34, height: 34, borderRadius: 17 },
});
