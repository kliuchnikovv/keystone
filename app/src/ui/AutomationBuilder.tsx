// The one screen every automation is made in.
//
// A person does not think in rules, schedules and timers — they think "when this, do
// that". So the screen asks for the "when" (a device, a time, a date, a delay) and for
// the "do", and the model decides which of the hub's three calls that is. The sentence
// at the bottom says what will happen, in words, while it is being built.
//
// The extra options ("only while", "only between", "not more often than") belong to the
// device kind alone: the hub keeps them in a rule and nowhere else, so for a time or a
// delay they are not shown at all.

import { useEffect, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import {
  COMPARISONS,
  MAX_EXTRA_CONDITIONS,
  WEEK,
  attrOptions,
  automationRequest,
  automationToSentence,
  describeTimePoint,
  inputUnit,
  isSunPoint,
  opsFor,
  presenceValueText,
  type ActionDraft,
  type AttrOption,
  type AutomationDraft,
  type ConditionDraft,
  type TriggerKind,
} from '../model/automation';
import type { Days, Entity, Scene, Weekday } from '../protocol/messages';
import { Button, Chip, ChipRow, Choice, ErrorText, Input } from './controls';
import { useTheme } from './theme';

/** The kinds of "when", named the way a person names them — never "rule/schedule/timer". */
export const TRIGGER_KINDS: { kind: TriggerKind; icon: string; label: string; hint: string }[] = [
  { kind: 'device', icon: '📡', label: 'A device changes', hint: 'when a sensor or a switch does something' },
  { kind: 'timeOfDay', icon: '🕗', label: 'A time of day', hint: 'every day, or on chosen days' },
  { kind: 'date', icon: '📅', label: 'A date, once', hint: 'one day, one time' },
  { kind: 'delay', icon: '⏱️', label: 'After a delay', hint: 'in so many minutes' },
];

const DAY_LABEL: Record<Weekday, string> = { mon: 'Mon', tue: 'Tue', wed: 'Wed', thu: 'Thu', fri: 'Fri', sat: 'Sat', sun: 'Sun' };

export interface AutomationBuilderProps {
  entities: Entity[];
  scenes: Scene[];
  hasLocation: boolean;
  initial: AutomationDraft;
  busy: boolean;
  onSubmit(op: string, args: Record<string, unknown>): Promise<boolean>;
  onCancel(): void;
}

// ---- blocks ---------------------------------------------------------------------------------

/** A titled block: the WHEN, the DO. */
function Block({ title, children, note }: { title: string; children: React.ReactNode; note?: string | null }) {
  const t = useTheme();
  return (
    <View style={[styles.block, { backgroundColor: t.card, borderColor: t.border }]}>
      <Text style={[styles.blockTitle, { color: t.muted }]}>{title}</Text>
      {children}
      {note ? <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 6 }}>{note}</Text> : null}
    </View>
  );
}

/** The arrow between two blocks: this is a chain, read from top to bottom. */
function Connector() {
  const t = useTheme();
  return (
    <View style={styles.connector}>
      <View style={[styles.line, { backgroundColor: t.border }]} />
      <Text style={{ color: t.muted, fontSize: 11 }}>▼</Text>
    </View>
  );
}

/** The value of a condition or an action: on/off for a boolean, a number otherwise. */
function ValueInput({ scope, option, value, onChange }: { scope: string; option: AttrOption | null; value: string; onChange(v: string): void }) {
  if (!option) return null;
  if (option.vt === 'bool') {
    const wording = (v: string) => presenceValueText(option.name, v === 'on') ?? (v === 'on' ? 'On' : 'Off');
    return (
      <ChipRow>
        {['on', 'off'].map((v) => (
          <Chip key={v} label={wording(v)} a11y={`${scope}: ${wording(v)}`} selected={value === v} onPress={() => onChange(v)} />
        ))}
      </ChipRow>
    );
  }
  const unit = inputUnit(option.name);
  return (
    <Input
      label={`${scope} value`}
      value={value}
      onChangeText={onChange}
      placeholder={unit ? `number, in ${unit}` : 'number'}
      keyboardType="numbers-and-punctuation"
    />
  );
}

/**
 * One condition: the device and attribute, the comparison, the value. Every part is a
 * chip, so the whole condition is visible on one line.
 */
function ConditionBlock({
  scope,
  options,
  allowAny,
  value,
  onChange,
}: {
  scope: string;
  options: AttrOption[];
  allowAny: boolean;
  value: ConditionDraft;
  onChange(c: ConditionDraft): void;
}) {
  const ops = opsFor(value.option).filter((op) => allowAny || op !== 'any');
  const opLabel = (op: string) => COMPARISONS.find((c) => c.op === op)?.label ?? op;
  return (
    <View style={styles.group}>
      <ChipRow>
        <Choice
          label={`${scope}: what to look at`}
          placeholder="Choose a device"
          options={options}
          selected={value.option}
          onSelect={(o) =>
            onChange({
              option: o,
              op: o.vt === 'bool' ? (value.op === 'any' ? 'any' : '==') : '==',
              // A bool keeps the value it had: an example that says "someone is there"
              // should not lose that when the person picks the sensor.
              value: o.vt === 'bool' ? value.value : '',
            })
          }
          keyOf={(o) => `${o.entity}:${o.attr}`}
          textOf={(o) => o.label}
        />
        {value.option && (
          <Choice
            label={`${scope}: comparison`}
            placeholder="comparison"
            options={ops}
            selected={ops.includes(value.op) ? value.op : null}
            onSelect={(op) => onChange({ ...value, op: op as ConditionDraft['op'], value: op === 'any' ? '' : value.value })}
            keyOf={(o) => o}
            textOf={opLabel}
            optionLabel={(o) => `${scope} ${opLabel(o)}`}
          />
        )}
      </ChipRow>
      {value.option && value.op !== 'any' && (
        <View style={{ marginTop: 8 }}>
          <ValueInput scope={scope} option={value.option} value={value.value} onChange={(v) => onChange({ ...value, value: v })} />
        </View>
      )}
    </View>
  );
}

/** "07:30", or sunrise / sunset with an offset: a text field plus quick chips that fill in the sun words. */
function TimeInput({ label, value, onChange, hasLocation }: {
  label: string; value: string; onChange(v: string): void; hasLocation: boolean;
}) {
  const t = useTheme();
  return (
    <View>
      <Input label={label} value={value} onChangeText={onChange} placeholder="07:30 or sunset+30" maxLength={14} />
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

// ---- the builder ------------------------------------------------------------------------------

export function AutomationBuilder({ entities, scenes, hasLocation, initial, busy, onSubmit, onCancel }: AutomationBuilderProps) {
  const t = useTheme();
  const [draft, setDraft] = useState<AutomationDraft>(initial);
  const [error, setError] = useState<string | null>(null);
  const [more, setMore] = useState(false);

  // An error belongs to what the person had then; as soon as they change something, it
  // is stale and must not stay on the screen.
  useEffect(() => { setError(null); }, [draft]);

  const all = attrOptions(entities);
  const writable = attrOptions(entities, { writable: true });
  // A schedule or a timer can only switch a device on or off; a rule can set anything.
  // It is the hub's own on/off attribute, not just any yes/no.
  const actionOptions = draft.kind === 'device' ? writable : writable.filter((o) => o.name === 'on_off');

  // With no scenes in the hub, a scene cannot be what happens: the draft's scene is
  // dropped, so the block offers a device instead of nothing.
  const sceneMode = scenes.length > 0 && draft.action?.scene !== undefined && draft.action?.scene !== null;

  const setKind = (kind: TriggerKind) =>
    setDraft((d) => {
      const next = { ...d, kind };
      // What the old kind allowed, the new kind may not: a brightness cannot be switched on.
      if (kind !== 'device' && next.action?.option && next.action.option.name !== 'on_off') {
        next.action = { ...next.action, option: null, value: '' };
      }
      return next;
    });
  const setTrigger = (c: ConditionDraft) => setDraft((d) => ({ ...d, trigger: c }));
  const setAction = (a: ActionDraft) => setDraft((d) => ({ ...d, action: a }));
  const setCondition = (i: number, c: ConditionDraft) =>
    setDraft((d) => ({ ...d, conditions: d.conditions.map((x, k) => (k === i ? c : x)) }));

  const custom = Array.isArray(draft.days);
  const toggleDay = (day: Weekday) => {
    const cur = Array.isArray(draft.days) ? draft.days : [];
    setDraft((d) => ({ ...d, days: cur.includes(day) ? cur.filter((x) => x !== day) : [...cur, day] }));
  };

  /** The draft as it will be sent: with no scenes, a scene cannot be the action. */
  const effective = (): AutomationDraft => {
    const a = draft.action;
    if (scenes.length > 0 || !a || a.scene === undefined || a.scene === null) return draft;
    return { ...draft, action: { ...a, scene: null } };
  };

  const save = async () => {
    // The model turns the draft into the hub's request, so the same checks run here and
    // in the tests — and the kind decides which of the three calls it is.
    const built = automationRequest(effective());
    if (!built.ok) return setError(built.error);
    setError(null);
    await onSubmit(built.value.op, built.value.args);
  };

  const trigger = draft.trigger ?? { option: null, op: 'any' as const, value: '' };

  return (
    <View>
      <Block title="WHEN">
        <ChipRow>
          <Choice
            label="When: what starts it"
            placeholder="Choose what starts it"
            options={TRIGGER_KINDS}
            selected={TRIGGER_KINDS.find((k) => k.kind === draft.kind) ?? null}
            onSelect={(k) => setKind(k.kind)}
            keyOf={(k) => k.kind}
            textOf={(k) => `${k.icon} ${k.label}`}
            optionLabel={(k) => `Choose ${k.label}`}
          />
        </ChipRow>
        <Text style={{ color: t.muted, fontSize: 11.5, marginTop: 6 }}>
          {TRIGGER_KINDS.find((k) => k.kind === draft.kind)?.hint}
        </Text>

        <View style={{ marginTop: 10 }}>
          {draft.kind === 'device' && (
            <ConditionBlock scope="When" options={all} allowAny value={trigger} onChange={setTrigger} />
          )}

          {draft.kind === 'timeOfDay' && (
            <View>
              <TimeInput label="Time" value={draft.time} onChange={(v) => setDraft((d) => ({ ...d, time: v }))} hasLocation={hasLocation} />
              <View style={{ marginTop: 10 }}>
                <ChipRow>
                  <Chip label="Every day" a11y="Every day" selected={draft.days === 'daily'} onPress={() => setDraft((d) => ({ ...d, days: 'daily' }))} />
                  <Chip label="Weekdays" a11y="Weekdays" selected={draft.days === 'weekdays'} onPress={() => setDraft((d) => ({ ...d, days: 'weekdays' }))} />
                  <Chip label="Weekends" a11y="Weekends" selected={draft.days === 'weekends'} onPress={() => setDraft((d) => ({ ...d, days: 'weekends' }))} />
                  <Chip label="Pick days" a11y="Pick days" selected={custom} onPress={() => setDraft((d) => ({ ...d, days: custom ? 'daily' : [] }))} />
                </ChipRow>
                {custom && (
                  <View style={{ marginTop: 8 }}>
                    <ChipRow>
                      {WEEK.map((day) => (
                        <Chip
                          key={day}
                          label={DAY_LABEL[day]}
                          a11y={`Day ${DAY_LABEL[day]}`}
                          selected={(draft.days as Weekday[]).includes(day)}
                          onPress={() => toggleDay(day)}
                        />
                      ))}
                    </ChipRow>
                  </View>
                )}
              </View>
            </View>
          )}

          {draft.kind === 'date' && (
            <View>
              <Input label="Date" value={draft.date ?? ''} onChangeText={(v) => setDraft((d) => ({ ...d, date: v }))} placeholder="YYYY-MM-DD" maxLength={10} />
              <View style={{ marginTop: 10 }}>
                <TimeInput label="Time" value={draft.time} onChange={(v) => setDraft((d) => ({ ...d, time: v }))} hasLocation={hasLocation} />
              </View>
            </View>
          )}

          {draft.kind === 'delay' && (
            <View>
              <ChipRow>
                {['5', '15', '30', '60'].map((m) => (
                  <Chip key={m} label={`${m} min`} a11y={`Preset ${m} minutes`} selected={draft.minutes === m} onPress={() => setDraft((d) => ({ ...d, minutes: m }))} />
                ))}
              </ChipRow>
              <View style={{ marginTop: 8 }}>
                <Input label="Minutes" value={draft.minutes} onChangeText={(v) => setDraft((d) => ({ ...d, minutes: v }))} placeholder="or type minutes" keyboardType="decimal-pad" />
              </View>
            </View>
          )}
        </View>
      </Block>

      <Connector />

      <Block title="DO" note={draft.action?.option || draft.action?.scene ? undefined : 'What happens'}>
        {scenes.length > 0 && (
          <ChipRow>
            <Chip
              label="A device"
              a11y="Set a device"
              selected={!sceneMode}
              onPress={() => setDraft((d) => ({ ...d, action: { option: d.action?.option ?? null, value: d.action?.value ?? '', scene: null } }))}
            />
            <Chip
              label="A scene"
              a11y="Apply a scene"
              selected={sceneMode}
              onPress={() => setDraft((d) => ({ ...d, action: { option: null, value: '', scene: d.action?.scene || scenes[0]?.name || '' } }))}
            />
          </ChipRow>
        )}
        {sceneMode ? (
          <View style={{ marginTop: 8 }}>
            <ChipRow>
              {scenes.map((s) => (
                <Chip
                  key={s.name}
                  label={s.name}
                  a11y={`Scene ${s.name}`}
                  selected={draft.action?.scene === s.name}
                  onPress={() => setDraft((d) => ({ ...d, action: { option: null, value: '', scene: s.name } }))}
                />
              ))}
            </ChipRow>
          </View>
        ) : (
          <View style={{ marginTop: 8 }}>
            <ChipRow>
              <Choice
                label="Then: what to set"
                placeholder="Choose what to change"
                options={actionOptions}
                selected={draft.action?.option ?? null}
                onSelect={(o) => {
                  // The value belongs to the attribute it was typed for: it follows only
                  // into an attribute that takes the same kind of value.
                  const prev = draft.action?.option ?? null;
                  const typed = draft.action?.value ?? '';
                  const keep = prev ? prev.vt === o.vt : o.vt === 'bool' && (typed === 'on' || typed === 'off');
                  const value = keep ? typed : o.vt === 'bool' ? 'off' : '';
                  setAction({ option: o, value, scene: null });
                }}
                keyOf={(o) => `${o.entity}:${o.attr}`}
                textOf={(o) => o.label}
              />
            </ChipRow>
            {draft.action?.option && (
              <View style={{ marginTop: 8 }}>
                <ValueInput scope="Then" option={draft.action.option} value={draft.action.value} onChange={(v) => setAction({ ...draft.action!, value: v })} />
              </View>
            )}
          </View>
        )}
      </Block>

      {/* the sentence: what the person is building, in words */}
      <View style={[styles.preview, { borderColor: t.border, backgroundColor: t.bg }]}>
        <Text style={{ color: t.muted, fontSize: 11 }}>THIS WILL HAPPEN</Text>
        <Text style={{ color: t.text, fontSize: 13.5, lineHeight: 19, marginTop: 4 }}>{automationToSentence(draft)}</Text>
      </View>

      {/* the extra options belong to a rule alone: the hub keeps them nowhere else */}
      {draft.kind === 'device' && (
        <>
          <View style={{ marginTop: 12 }}>
            <Button kind="plain" label={more ? 'Fewer options' : 'More options'} onPress={() => setMore((m) => !m)} />
          </View>

          {more && (
            <View style={[styles.block, { backgroundColor: t.card, borderColor: t.border }]}>
              <Text style={[styles.blockTitle, { color: t.muted }]}>ONLY WHILE</Text>
              {draft.conditions.map((c, i) => (
                <View key={i} style={styles.group}>
                  <ConditionBlock
                    scope={`Only while ${i + 1}`}
                    options={all}
                    allowAny={false}
                    value={c}
                    onChange={(x) => setCondition(i, x)}
                  />
                  <View style={styles.rowEnd}>
                    <Button
                      kind="danger"
                      size="mini"
                      label="Remove"
                      a11y={`Remove condition ${i + 1}`}
                      onPress={() => setDraft((d) => ({ ...d, conditions: d.conditions.filter((_, k) => k !== i) }))}
                    />
                  </View>
                </View>
              ))}
              {draft.conditions.length === 0 && (
                <View style={styles.rowEnd}>
                  <Button
                    kind="plain"
                    size="mini"
                    label="+ Add a condition"
                    a11y="Add a condition"
                    onPress={() => setDraft((d) => ({ ...d, conditions: [{ option: null, op: '>', value: '' }] }))}
                  />
                </View>
              )}
              {draft.conditions.length > MAX_EXTRA_CONDITIONS && (
                <Text style={{ color: t.warn, fontSize: 11.5, marginTop: 6 }}>
                  The hub keeps {MAX_EXTRA_CONDITIONS} extra condition. Remove one to save.
                </Text>
              )}

              <Text style={[styles.blockTitle, { color: t.muted, marginTop: 14 }]}>ONLY BETWEEN</Text>
              <ChipRow>
                <Chip label="Any time" a11y="Any time" selected={!draft.window} onPress={() => setDraft((d) => ({ ...d, window: null }))} />
                <Chip label="Between times" a11y="Between times" selected={!!draft.window} onPress={() => setDraft((d) => ({ ...d, window: d.window ?? { from: '', to: '' } }))} />
              </ChipRow>
              {draft.window && (
                <View style={{ marginTop: 8 }}>
                  <Input label="From" value={draft.window.from} onChangeText={(v) => setDraft((d) => ({ ...d, window: { from: v, to: d.window?.to ?? '' } }))} placeholder="18:00 or sunset" maxLength={14} />
                  <View style={{ height: 8 }} />
                  <Input label="To" value={draft.window.to} onChangeText={(v) => setDraft((d) => ({ ...d, window: { from: d.window?.from ?? '', to: v } }))} placeholder="23:00 or sunrise-15" maxLength={14} />
                  {!hasLocation && (
                    <Text style={{ color: t.warn, fontSize: 11.5, marginTop: 6 }}>
                      Sunrise and sunset need the hub's location (Hub tab) until then.
                    </Text>
                  )}
                </View>
              )}

              <Text style={[styles.blockTitle, { color: t.muted, marginTop: 14 }]}>AT MOST ONCE PER</Text>
              <ChipRow>
                {[30, 60, 300, 600].map((s) => (
                  <Chip
                    key={s}
                    label={s < 60 ? `${s} s` : `${s / 60} min`}
                    a11y={`Preset ${s} seconds`}
                    selected={draft.cooldown_s === s}
                    onPress={() => setDraft((d) => ({ ...d, cooldown_s: d.cooldown_s === s ? null : s }))}
                  />
                ))}
              </ChipRow>
              <View style={{ marginTop: 8 }}>
                <Input
                  label="Cooldown (seconds)"
                  value={draft.cooldown_s === null ? '' : String(draft.cooldown_s)}
                  onChangeText={(v) => setDraft((d) => ({ ...d, cooldown_s: v.trim() === '' ? null : Number(v) }))}
                  placeholder="e.g. 300"
                  keyboardType="number-pad"
                />
              </View>
            </View>
          )}
        </>
      )}

      <ErrorText text={error} />
      <View style={styles.buttons}>
        <Button kind="secondary" label="Cancel" onPress={onCancel} />
        <Button label="Save automation" onPress={save} disabled={busy} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  block: { borderWidth: 1, borderRadius: 14, padding: 12, marginBottom: 0 },
  blockTitle: { fontSize: 10.5, fontWeight: '700', letterSpacing: 0.8, marginBottom: 8 },
  group: { marginBottom: 6 },
  rowEnd: { flexDirection: 'row', justifyContent: 'flex-end', marginTop: 4 },
  connector: { alignItems: 'center', paddingVertical: 8 },
  line: { width: 2, height: 10, marginBottom: 2 },
  preview: { borderWidth: 1, borderRadius: 12, padding: 12, marginTop: 14 },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 16 },
});
