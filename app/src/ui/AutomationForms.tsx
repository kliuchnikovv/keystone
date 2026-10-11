import { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import {
  COMPARISONS, WEEK, actionText, attrOptions, captureScene, describeScene, inputUnit, ruleRequest, sceneRequest, scheduleRequest, switchTargets, timerRequest,
  type AttrOption, type ConditionForm, type RuleForm,
} from '../model/automation';
import { deviceTitle } from '../model/devices';
import type { Action, CmpOp, Days, Entity, Weekday } from '../protocol/messages';
import { Button, Chip, ChipRow, ErrorText, Field, Input, Picker } from './controls';
import { useTheme } from './theme';

type Submit = (args: Record<string, unknown>) => Promise<boolean>;

const ACTIONS: Action[] = ['on', 'off', 'toggle'];
const DAY_LABEL: Record<Weekday, string> = { mon: 'Mon', tue: 'Tue', wed: 'Wed', thu: 'Thu', fri: 'Fri', sat: 'Sat', sun: 'Sun' };

function DevicePicker({ label, devices, value, onChange }: { label: string; devices: Entity[]; value: number | null; onChange(id: number): void }) {
  return (
    <Picker
      label={label}
      placeholder="Choose a device"
      options={devices}
      selected={devices.find((d) => d.id === value) ?? null}
      onSelect={(d) => onChange(d.id)}
      keyOf={(d) => String(d.id)}
      textOf={deviceTitle}
    />
  );
}

function ActionChips({ value, onChange }: { value: Action; onChange(a: Action): void }) {
  return (
    <ChipRow>
      {ACTIONS.map((a) => (
        <Chip key={a} label={actionText(a)} a11y={`Action ${actionText(a)}`} selected={value === a} onPress={() => onChange(a)} />
      ))}
    </ChipRow>
  );
}

function FormShell({ title, children, error, onCancel, onSave, busy, saveLabel }: {
  title: string; children: React.ReactNode; error: string | null; onCancel(): void; onSave(): void; busy: boolean; saveLabel: string;
}) {
  const t = useTheme();
  return (
    <View style={[styles.form, { backgroundColor: t.card, borderColor: t.border }]}>
      <Text style={[styles.title, { color: t.text }]}>{title}</Text>
      {children}
      <ErrorText text={error} />
      <View style={styles.buttons}>
        <Button kind="plain" label="Cancel" onPress={onCancel} />
        <Button label={saveLabel} onPress={onSave} disabled={busy} />
      </View>
    </View>
  );
}

// ---- schedule ---------------------------------------------------------------------------

export function ScheduleForm({ entities, busy, onSubmit, onCancel }: { entities: Entity[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const [time, setTime] = useState('');
  const [days, setDays] = useState<Days>('daily');
  const [entity, setEntity] = useState<number | null>(null);
  const [action, setAction] = useState<Action>('off');
  const [error, setError] = useState<string | null>(null);

  const custom = Array.isArray(days);
  const toggleDay = (d: Weekday) => {
    const cur = Array.isArray(days) ? days : [];
    setDays(cur.includes(d) ? cur.filter((x) => x !== d) : [...cur, d]);
  };

  const save = async () => {
    const r = scheduleRequest({ time, days, entity, action });
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New schedule" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Save schedule">
      <Field label="Time (24 h, hub's time zone)">
        <Input label="Time" value={time} onChangeText={setTime} placeholder="07:30" keyboardType="numbers-and-punctuation" maxLength={5} />
      </Field>
      <Field label="Days">
        <ChipRow>
          <Chip label="Every day" selected={days === 'daily'} onPress={() => setDays('daily')} />
          <Chip label="Weekdays" selected={days === 'weekdays'} onPress={() => setDays('weekdays')} />
          <Chip label="Weekends" selected={days === 'weekends'} onPress={() => setDays('weekends')} />
          <Chip label="Pick days" selected={custom} onPress={() => setDays(custom ? 'daily' : [])} />
        </ChipRow>
        {custom && (
          <View style={{ marginTop: 8 }}>
            <ChipRow>
              {WEEK.map((d) => (
                <Chip key={d} label={DAY_LABEL[d]} a11y={`Day ${DAY_LABEL[d]}`} selected={(days as Weekday[]).includes(d)} onPress={() => toggleDay(d)} />
              ))}
            </ChipRow>
          </View>
        )}
      </Field>
      <Field label="Device">
        <DevicePicker label="Device" devices={switchTargets(entities)} value={entity} onChange={setEntity} />
      </Field>
      <Field label="Do">
        <ActionChips value={action} onChange={setAction} />
      </Field>
    </FormShell>
  );
}

// ---- timer ------------------------------------------------------------------------------

export function TimerForm({ entities, busy, onSubmit, onCancel }: { entities: Entity[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const [entity, setEntity] = useState<number | null>(null);
  const [action, setAction] = useState<Action>('off');
  const [minutes, setMinutes] = useState('');
  const [error, setError] = useState<string | null>(null);

  const save = async () => {
    const r = timerRequest({ entity, action, minutes });
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New timer" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Start timer">
      <Field label="Device">
        <DevicePicker label="Device" devices={switchTargets(entities)} value={entity} onChange={setEntity} />
      </Field>
      <Field label="Then">
        <ActionChips value={action} onChange={setAction} />
      </Field>
      <Field label="After (minutes)">
        <ChipRow>
          {['5', '15', '30', '60'].map((m) => (
            <Chip key={m} label={`${m} min`} a11y={`Preset ${m} minutes`} selected={minutes === m} onPress={() => setMinutes(m)} />
          ))}
        </ChipRow>
        <View style={{ marginTop: 8 }}>
          <Input label="Minutes" value={minutes} onChangeText={setMinutes} placeholder="or type minutes" keyboardType="decimal-pad" />
        </View>
      </Field>
    </FormShell>
  );
}

// ---- rule -------------------------------------------------------------------------------

/** `scope` ("When", "Then", "Only while") keeps the labels of the three value fields distinct. */
function ValueField({ scope, option, value, onChange }: { scope: string; option: AttrOption | null; value: string; onChange(v: string): void }) {
  if (!option) return null;
  if (option.vt === 'bool') {
    return (
      <ChipRow>
        <Chip label="On" a11y={`${scope} value On`} selected={value === 'on'} onPress={() => onChange('on')} />
        <Chip label="Off" a11y={`${scope} value Off`} selected={value === 'off'} onPress={() => onChange('off')} />
      </ChipRow>
    );
  }
  const unit = inputUnit(option.name);
  return <Input label={`${scope} value`} value={value} onChangeText={onChange} placeholder={unit ? `number, in ${unit}` : 'number'} keyboardType="numbers-and-punctuation" />;
}

function ConditionEditor({ name, options, value, onChange, allowAny }: { name: string; options: AttrOption[]; value: ConditionForm; onChange(c: ConditionForm): void; allowAny: boolean }) {
  const isBool = value.option?.vt === 'bool';
  const ops = COMPARISONS.filter((c) => (allowAny || c.op !== 'any') && (!isBool || c.op === 'any' || c.op === '==' || c.op === '!='));
  return (
    <View>
      <Picker
        label={`${name} attribute`}
        placeholder="Choose a device and what to look at"
        options={options}
        selected={value.option}
        onSelect={(o) => onChange({ option: o, op: o.vt === 'bool' && value.op !== 'any' ? '==' : value.op === 'any' && !allowAny ? '==' : value.op, value: '' })}
        keyOf={(o) => `${o.entity}:${o.attr}`}
        textOf={(o) => o.label}
      />
      {value.option && (
        <View style={{ marginTop: 8 }}>
          <ChipRow>
            {ops.map((c) => (
              <Chip key={c.op} label={c.label} a11y={`${name} ${c.label}`} selected={value.op === c.op} onPress={() => onChange({ ...value, op: c.op })} />
            ))}
          </ChipRow>
          {value.op !== 'any' && (
            <View style={{ marginTop: 8 }}>
              <ValueField scope={name} option={value.option} value={value.value} onChange={(v) => onChange({ ...value, value: v })} />
            </View>
          )}
        </View>
      )}
    </View>
  );
}

const EMPTY: ConditionForm = { option: null, op: '==', value: '' };

export function RuleFormView({ entities, busy, onSubmit, onCancel }: { entities: Entity[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const t = useTheme();
  const all = attrOptions(entities);
  const writable = attrOptions(entities, { writable: true });
  const [when, setWhen] = useState<ConditionForm>({ ...EMPTY, op: 'any' });
  const [then, setThen] = useState<{ option: AttrOption | null; value: string }>({ option: null, value: '' });
  const [more, setMore] = useState(false);
  const [useWindow, setUseWindow] = useState(false);
  const [from, setFrom] = useState('');
  const [to, setTo] = useState('');
  const [useAnd, setUseAnd] = useState(false);
  const [and, setAnd] = useState<ConditionForm>({ ...EMPTY, op: '>' });
  const [cooldown, setCooldown] = useState('');
  const [error, setError] = useState<string | null>(null);

  const save = async () => {
    const form: RuleForm = {
      when,
      then,
      between: useWindow ? { from, to } : null,
      and: useAnd ? and : null,
      cooldown,
    };
    const r = ruleRequest(form);
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New rule" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Save rule">
      <Field label="When">
        <ConditionEditor name="When" options={all} value={when} onChange={setWhen} allowAny />
      </Field>
      <Field label="Then set">
        <Picker
          label="Then attribute"
          placeholder="Choose what to change"
          options={writable}
          selected={then.option}
          onSelect={(o) => setThen({ option: o, value: '' })}
          keyOf={(o) => `${o.entity}:${o.attr}`}
          textOf={(o) => o.label}
        />
        <View style={{ marginTop: 8 }}>
          <ValueField scope="Then" option={then.option} value={then.value} onChange={(v) => setThen({ ...then, value: v })} />
        </View>
      </Field>

      <View style={{ marginTop: 14 }}>
        <Button kind="plain" label={more ? 'Fewer options' : 'More options'} onPress={() => setMore((m) => !m)} />
      </View>
      {more && (
        <View>
          <Field label="Only between">
            <ChipRow>
              <Chip label="Any time" selected={!useWindow} onPress={() => setUseWindow(false)} />
              <Chip label="Between times" selected={useWindow} onPress={() => setUseWindow(true)} />
            </ChipRow>
            {useWindow && (
              <View style={styles.pair}>
                <View style={{ flex: 1 }}><Input label="From" value={from} onChangeText={setFrom} placeholder="18:00" maxLength={5} /></View>
                <Text style={{ color: t.muted }}>to</Text>
                <View style={{ flex: 1 }}><Input label="To" value={to} onChangeText={setTo} placeholder="23:00" maxLength={5} /></View>
              </View>
            )}
          </Field>
          <Field label="Only while">
            <ChipRow>
              <Chip label="No condition" selected={!useAnd} onPress={() => setUseAnd(false)} />
              <Chip label="Add a condition" selected={useAnd} onPress={() => setUseAnd(true)} />
            </ChipRow>
            {useAnd && (
              <View style={{ marginTop: 8 }}>
                <ConditionEditor name="Only while" options={all} value={and} onChange={setAnd} allowAny={false} />
              </View>
            )}
          </Field>
          <Field label="At most once per (minutes)" hint="Leave empty to allow every time it matches.">
            <Input label="Cooldown (minutes)" value={cooldown} onChangeText={setCooldown} placeholder="e.g. 5" keyboardType="decimal-pad" />
          </Field>
        </View>
      )}
    </FormShell>
  );
}

// ---- scene ------------------------------------------------------------------------------

/** A scene is saved from the devices' state right now: set things up, pick the devices, name it. */
export function SceneForm({ entities, busy, onSubmit, onCancel }: { entities: Entity[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const t = useTheme();
  const [name, setName] = useState('');
  const [picked, setPicked] = useState<number[]>([]);
  const [error, setError] = useState<string | null>(null);

  // Only devices with something writable and known can be part of a scene.
  const candidates = entities.filter((e) => captureScene(entities, [e.id]).length > 0);
  const toggle = (id: number) => setPicked((p) => (p.includes(id) ? p.filter((x) => x !== id) : [...p, id]));
  const actions = captureScene(entities, picked);
  const save = async () => {
    const r = sceneRequest(name, actions);
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New scene" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Save scene">
      <Text style={{ color: t.muted, fontSize: 12, lineHeight: 17 }}>
        A scene remembers how the chosen devices are set right now (on or off, brightness). Set them up first, then save.
      </Text>
      <Field label="Name">
        <Input label="Scene name" value={name} onChangeText={setName} placeholder="e.g. evening" />
      </Field>
      <Field label="Devices">
        {candidates.length === 0 && <Text style={{ color: t.muted }}>No devices to save yet.</Text>}
        <ChipRow>
          {candidates.map((e) => (
            <Chip key={e.id} label={deviceTitle(e)} a11y={`Include ${deviceTitle(e)}`} selected={picked.includes(e.id)} onPress={() => toggle(e.id)} />
          ))}
        </ChipRow>
      </Field>
      {actions.length > 0 && (
        <Text accessibilityLabel="Scene contents" style={{ color: t.text, fontSize: 13, marginTop: 4 }}>
          Will save: {describeScene({ name, actions }, entities).detail}
        </Text>
      )}
    </FormShell>
  );
}

const styles = StyleSheet.create({
  form: { borderWidth: 1, borderRadius: 14, padding: 14, marginBottom: 14 },
  title: { fontSize: 17, fontWeight: '700' },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 16 },
  pair: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 },
});
