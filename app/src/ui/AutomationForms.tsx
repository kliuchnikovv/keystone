import { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import {
  COMPARISONS, WEEK, actionText, localDate, attrOptions, addableSettings, captureScene, describeScene, describeTimePoint, draftsFromCurrentState, draftsRequest, isSunPoint, sceneDrafts, type SceneDraft, inputUnit, ruleRequest, sceneRequest, scheduleRequest, switchTargets, timerRequest,
  type AttrOption, type ConditionForm, type RuleForm,
} from '../model/automation';
import { deviceTitle } from '../model/devices';
import type { Action, CmpOp, Days, Entity, Scene, Weekday } from '../protocol/messages';
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

type Target = 'device' | 'scene';

/** "A device" or "A scene": shown only when the hub has scenes. */
function TargetToggle({ scenes, value, onChange }: { scenes: Scene[]; value: Target; onChange(t: Target): void }) {
  if (scenes.length === 0) return null;
  return (
    <ChipRow>
      <Chip label="A device" a11y="Target a device" selected={value === 'device'} onPress={() => onChange('device')} />
      <Chip label="A scene" a11y="Target a scene" selected={value === 'scene'} onPress={() => onChange('scene')} />
    </ChipRow>
  );
}

function SceneChoice({ scenes, value, onChange }: { scenes: Scene[]; value: string | null; onChange(name: string): void }) {
  return (
    <ChipRow>
      {scenes.map((s) => (
        <Chip key={s.name} label={s.name} a11y={`Scene ${s.name}`} selected={value === s.name} onPress={() => onChange(s.name)} />
      ))}
    </ChipRow>
  );
}

/** "07:30", or sunrise / sunset with an offset: a text field plus quick chips that fill in the sun words. */
function TimePointInput({ label, value, onChange, placeholder, hasLocation }: {
  label: string; value: string; onChange(v: string): void; placeholder: string; hasLocation: boolean;
}) {
  const t = useTheme();
  return (
    <View>
      <Input label={label} value={value} onChangeText={onChange} placeholder={placeholder} maxLength={14} />
      <View style={{ marginTop: 6 }}>
        <ChipRow>
          <Chip label="Sunrise" a11y={`${label}: sunrise`} selected={/^sunrise/.test(value)} onPress={() => onChange('sunrise')} />
          <Chip label="Sunset" a11y={`${label}: sunset`} selected={/^sunset/.test(value)} onPress={() => onChange('sunset')} />
        </ChipRow>
      </View>
      {isSunPoint(value) && (
        <Text style={{ color: hasLocation ? t.muted : t.warn, fontSize: 12, marginTop: 6 }}>
          {describeTimePoint(value)}
          {hasLocation ? '. Add +30 or -15 for minutes after or before.' : ". The hub needs its location first (Hub tab); until then this waits."}
        </Text>
      )}
    </View>
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
        <Button kind="secondary" label="Cancel" onPress={onCancel} />
        <Button label={saveLabel} onPress={onSave} disabled={busy} />
      </View>
    </View>
  );
}

// ---- schedule ---------------------------------------------------------------------------

export function ScheduleForm({ entities, scenes, hasLocation, busy, onSubmit, onCancel }: { entities: Entity[]; scenes: Scene[]; hasLocation: boolean; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const [time, setTime] = useState('');
  const [days, setDays] = useState<Days>('daily');
  const [date, setDate] = useState<string | null>(null);  // set = a one-off on that day
  const [entity, setEntity] = useState<number | null>(null);
  const [action, setAction] = useState<Action>('off');
  const [target, setTarget] = useState<Target>('device');
  const [scene, setScene] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const custom = date === null && Array.isArray(days);
  const toggleDay = (d: Weekday) => {
    const cur = Array.isArray(days) ? days : [];
    setDays(cur.includes(d) ? cur.filter((x) => x !== d) : [...cur, d]);
  };

  const save = async () => {
    const r = scheduleRequest({ time, days, date, entity, action, scene: target === 'scene' ? scene ?? '' : null });
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New schedule" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Save schedule">
      <Field label="Time (24 h, hub's time zone — or at the sun)">
        <TimePointInput label="Time" value={time} onChange={setTime} placeholder="07:30 or sunset+30" hasLocation={hasLocation} />
      </Field>
      <Field label="Days">
        <ChipRow>
          <Chip label="Every day" selected={date === null && days === 'daily'} onPress={() => { setDate(null); setDays('daily'); }} />
          <Chip label="Weekdays" selected={date === null && days === 'weekdays'} onPress={() => { setDate(null); setDays('weekdays'); }} />
          <Chip label="Weekends" selected={date === null && days === 'weekends'} onPress={() => { setDate(null); setDays('weekends'); }} />
          <Chip label="Pick days" selected={custom} onPress={() => { setDate(null); setDays(custom ? 'daily' : []); }} />
          <Chip label="Once, on a date" selected={date !== null} onPress={() => setDate(date === null ? localDate() : null)} />
        </ChipRow>
        {date !== null && (
          <View style={{ marginTop: 8 }}>
            <Input label="Date" value={date} onChangeText={setDate} placeholder="YYYY-MM-DD" autoCapitalize="none" />
          </View>
        )}
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
      <TargetToggle scenes={scenes} value={target} onChange={setTarget} />
      {target === 'scene' ? (
        <Field label="Scene">
          <SceneChoice scenes={scenes} value={scene} onChange={setScene} />
        </Field>
      ) : (
        <>
          <Field label="Device">
            <DevicePicker label="Device" devices={switchTargets(entities)} value={entity} onChange={setEntity} />
          </Field>
          <Field label="Do">
            <ActionChips value={action} onChange={setAction} />
          </Field>
        </>
      )}
    </FormShell>
  );
}

// ---- timer ------------------------------------------------------------------------------

export function TimerForm({ entities, scenes, busy, onSubmit, onCancel }: { entities: Entity[]; scenes: Scene[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const [entity, setEntity] = useState<number | null>(null);
  const [action, setAction] = useState<Action>('off');
  const [minutes, setMinutes] = useState('');
  const [target, setTarget] = useState<Target>('device');
  const [scene, setScene] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const save = async () => {
    const r = timerRequest({ entity, action, minutes, scene: target === 'scene' ? scene ?? '' : null });
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };

  return (
    <FormShell title="New timer" error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Start timer">
      <TargetToggle scenes={scenes} value={target} onChange={setTarget} />
      {target === 'scene' ? (
        <Field label="Scene">
          <SceneChoice scenes={scenes} value={scene} onChange={setScene} />
        </Field>
      ) : (
        <>
          <Field label="Device">
            <DevicePicker label="Device" devices={switchTargets(entities)} value={entity} onChange={setEntity} />
          </Field>
          <Field label="Then">
            <ActionChips value={action} onChange={setAction} />
          </Field>
        </>
      )}
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

export function RuleFormView({ entities, scenes, hasLocation, busy, onSubmit, onCancel }: { entities: Entity[]; scenes: Scene[]; hasLocation: boolean; busy: boolean; onSubmit: Submit; onCancel(): void }) {
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
  const [thenTarget, setThenTarget] = useState<Target>('device');
  const [thenScene, setThenScene] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const save = async () => {
    const form: RuleForm = {
      when,
      then: { ...then, scene: thenTarget === 'scene' ? thenScene ?? '' : null },
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
      <TargetToggle scenes={scenes} value={thenTarget} onChange={setThenTarget} />
      {thenTarget === 'scene' ? (
        <Field label="Then apply scene">
          <SceneChoice scenes={scenes} value={thenScene} onChange={setThenScene} />
        </Field>
      ) : (
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
      )}

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
              <View>
                <TimePointInput label="From" value={from} onChange={setFrom} placeholder="18:00 or sunset" hasLocation={hasLocation} />
                <View style={{ height: 8 }} />
                <TimePointInput label="To" value={to} onChange={setTo} placeholder="23:00 or sunrise-15" hasLocation={hasLocation} />
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
          <Field label="At most once per (seconds)" hint="Leave empty to allow every time it matches.">
            <ChipRow>
              {[30, 60, 300, 600].map((s) => (
                <Chip key={s} label={s < 60 ? `${s} s` : `${s / 60} min`} a11y={`Preset ${s} seconds`} selected={cooldown === String(s)} onPress={() => setCooldown(String(s))} />
              ))}
            </ChipRow>
            <View style={{ marginTop: 8 }}>
              <Input label="Cooldown (seconds)" value={cooldown} onChangeText={setCooldown} placeholder="e.g. 300" keyboardType="number-pad" />
            </View>
          </Field>
        </View>
      )}
    </FormShell>
  );
}

// ---- editing a scene -------------------------------------------------------------------------

/** Change the values of an existing scene, drop settings, add others, or take everything from the devices' state right now. Saves under the same name. */
export function SceneEditor({ scene, entities, busy, onSubmit, onCancel }: { scene: Scene; entities: Entity[]; busy: boolean; onSubmit: Submit; onCancel(): void }) {
  const t = useTheme();
  const initial = sceneDrafts(scene, entities);
  const [drafts, setDrafts] = useState<SceneDraft[]>(initial.drafts);
  const [error, setError] = useState<string | null>(null);
  const addable = addableSettings(entities, drafts);

  const save = async () => {
    const r = draftsRequest(scene.name, drafts);
    if (!r.ok) return setError(r.error);
    setError(null);
    await onSubmit(r.value);
  };
  const setValue = (i: number, value: string) => setDrafts((ds) => ds.map((d, k) => (k === i ? { ...d, value } : d)));

  return (
    <FormShell title={`Edit scene ${scene.name}`} error={error} onCancel={onCancel} onSave={save} busy={busy} saveLabel="Save changes">
      {initial.gone.length > 0 && (
        <Text style={{ color: t.warn, fontSize: 12 }}>
          {initial.gone.length} setting{initial.gone.length === 1 ? '' : 's'} refer to devices that are gone or cannot be set any more; saving removes them.
        </Text>
      )}
      {drafts.map((d, i) => (
        <View key={`${d.option.entity}:${d.option.attr}`} accessibilityLabel={`Setting ${d.option.label}`} style={styles.setting}>
          <Text style={{ color: t.text, fontWeight: '600' }}>{d.option.label}</Text>
          <ValueField scope={d.option.label} option={d.option} value={d.value} onChange={(v) => setValue(i, v)} />
          <Button kind="danger" label="Remove" a11y={`Remove setting ${d.option.label}`} onPress={() => setDrafts((ds) => ds.filter((_, k) => k !== i))} />
        </View>
      ))}
      {drafts.length === 0 && <Text style={{ color: t.muted }}>No settings. Add one below.</Text>}
      <Field label="Add a setting">
        <Picker
          label="Add setting"
          placeholder={addable.length === 0 ? 'Nothing left to add' : 'Choose a device and what to set'}
          options={addable}
          selected={null}
          onSelect={(o) => setDrafts((ds) => [...ds, { option: o, value: '' }])}
          keyOf={(o) => `${o.entity}:${o.attr}`}
          textOf={(o) => o.label}
        />
      </Field>
      <View style={{ marginTop: 6 }}>
        <Button kind="plain" label="Use what the devices are doing now" a11y="Use current state" onPress={() => setDrafts((ds) => draftsFromCurrentState(ds, entities))} />
      </View>
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
  setting: { marginTop: 10, gap: 6 },
});
