import { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import {
  COMPARISONS,
  MAX_EXTRA_CONDITIONS,
  attrOptions,
  draftRequest,
  draftToSentence,
  emptyDraft,
  inputUnit,
  opsFor,
  presenceValueText,
  type ActionDraft,
  type AttrOption,
  type ConditionDraft,
  type RuleDraft,
} from '../model/automation';
import { deviceTitle } from '../model/devices';
import type { Entity, Scene } from '../protocol/messages';
import { Button, Chip, ChipRow, Choice, ErrorText, Field, Input, Picker } from './controls';
import { useTheme } from './theme';

// The rule editor: blocks with chips, not a list of fields.
//
// A rule on a phone used to be six or seven screens of scrolling ("When", "Then",
// "More options", "Between", "Only while", "Cooldown"), and the person could not see
// what they were building. Here every part of the rule is one block, every block is
// one row of chips, and the whole rule reads as a sentence under the blocks.
//
// The layout is a vertical column, because a horizontal flow (n8n, Node-RED) does not
// fit a phone: the width goes into zooming and the rule is never seen whole.

export interface RuleEditorProps {
  entities: Entity[];
  scenes: Scene[];
  hasLocation: boolean;
  busy: boolean;
  onSubmit(args: Record<string, unknown>): Promise<boolean>;
  onCancel(): void;
}

// ---- blocks ---------------------------------------------------------------------------------

/** A titled block: the WHEN, the AND conditions, the THEN. */
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

/** The value of a condition: on/off for a boolean, a number otherwise. */
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
          onSelect={(o) => onChange({ option: o, op: o.vt === 'bool' ? (value.op === 'any' ? 'any' : '==') : '==', value: '' })}
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

// ---- the editor ------------------------------------------------------------------------------

export function RuleEditor({ entities, scenes, hasLocation, busy, onSubmit, onCancel }: RuleEditorProps) {
  const t = useTheme();
  const all = attrOptions(entities);
  const writable = attrOptions(entities, { writable: true });
  const [draft, setDraft] = useState<RuleDraft>(emptyDraft);
  const [error, setError] = useState<string | null>(null);
  const [more, setMore] = useState(false);

  const setTrigger = (c: ConditionDraft) => setDraft((d) => ({ ...d, trigger: c }));
  const setAction = (a: ActionDraft) => setDraft((d) => ({ ...d, action: a }));
  const setCondition = (i: number, c: ConditionDraft) =>
    setDraft((d) => ({ ...d, conditions: d.conditions.map((x, k) => (k === i ? c : x)) }));

  const save = async () => {
    // The draft is turned into the request by the model, so the same checks run here
    // and in the tests.
    const built = buildArgs(draft);
    if (!built.ok) return setError(built.error);
    setError(null);
    await onSubmit(built.value);
  };

  const trigger = draft.trigger ?? { option: null, op: 'any' as const, value: '' };

  return (
    <View>
      <Block title="WHEN" note={trigger.option ? undefined : 'What starts the rule'}>
        <ConditionBlock scope="When" options={all} allowAny value={trigger} onChange={setTrigger} />
      </Block>

      <Connector />

      <Block title="AND" note="Extra conditions — all of them must hold">
        {draft.conditions.map((c, i) => (
          <View key={i} style={styles.group}>
            <ConditionBlock scope={`Only while ${i + 1}`} options={all} allowAny={false} value={c} onChange={(x) => setCondition(i, x)} />
            <View style={styles.rowEnd}>
              <Button kind="danger" size="mini" label="Remove" a11y={`Remove condition ${i + 1}`} onPress={() => setDraft((d) => ({ ...d, conditions: d.conditions.filter((_, k) => k !== i) }))} />
            </View>
          </View>
        ))}
        {draft.conditions.length === 0 && (
          <View style={styles.rowEnd}>
            <Button kind="plain" size="mini" label="+ Add a condition" a11y="Add a condition" onPress={() => setDraft((d) => ({ ...d, conditions: [{ option: null, op: '>', value: '' }] }))} />
          </View>
        )}
        {draft.conditions.length > MAX_EXTRA_CONDITIONS && (
          <Text style={{ color: t.warn, fontSize: 11.5, marginTop: 6 }}>
            The hub keeps {MAX_EXTRA_CONDITIONS} extra condition. Remove one to save the rule.
          </Text>
        )}
      </Block>

      <Connector />

      <Block title="THEN" note={draft.action?.option || draft.action?.scene ? undefined : 'What happens'}>
        {scenes.length > 0 && (
          <ChipRow>
            <Chip label="A device" a11y="Set a device" selected={!draft.action?.scene} onPress={() => setDraft((d) => ({ ...d, action: { option: d.action?.option ?? null, value: d.action?.value ?? '', scene: null } }))} />
            <Chip label="A scene" a11y="Apply a scene" selected={!!draft.action?.scene} onPress={() => setDraft((d) => ({ ...d, action: { option: null, value: '', scene: d.action?.scene ?? scenes[0]?.name ?? '' } }))} />
          </ChipRow>
        )}
        {draft.action?.scene ? (
          <View style={{ marginTop: 8 }}>
            <ChipRow>
              {scenes.map((s) => (
                <Chip key={s.name} label={s.name} a11y={`Scene ${s.name}`} selected={draft.action?.scene === s.name} onPress={() => setDraft((d) => ({ ...d, action: { option: null, value: '', scene: s.name } }))} />
              ))}
            </ChipRow>
          </View>
        ) : (
          <View style={{ marginTop: 8 }}>
            <ChipRow>
              <Choice
                label="Then: what to set"
                placeholder="Choose what to change"
                options={writable}
                selected={draft.action?.option ?? null}
                onSelect={(o) => setAction({ option: o, value: '', scene: null })}
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
        <Text style={{ color: t.muted, fontSize: 11 }}>THIS RULE</Text>
        <Text style={{ color: t.text, fontSize: 13.5, lineHeight: 19, marginTop: 4 }}>{draftToSentence(draft)}</Text>
      </View>

      <View style={{ marginTop: 12 }}>
        <Button kind="plain" label={more ? 'Fewer options' : 'More options'} onPress={() => setMore((m) => !m)} />
      </View>

      {more && (
        <View style={[styles.block, { backgroundColor: t.card, borderColor: t.border }]}>
          <Text style={[styles.blockTitle, { color: t.muted }]}>ONLY BETWEEN</Text>
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
              <Chip key={s} label={s < 60 ? `${s} s` : `${s / 60} min`} a11y={`Preset ${s} seconds`} selected={draft.cooldown_s === s} onPress={() => setDraft((d) => ({ ...d, cooldown_s: d.cooldown_s === s ? null : s }))} />
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

      <ErrorText text={error} />
      <View style={styles.buttons}>
        <Button kind="secondary" label="Cancel" onPress={onCancel} />
        <Button label="Save rule" onPress={save} disabled={busy} />
      </View>
    </View>
  );
}

/** The draft as the hub wants it. Kept here so the editor and the tests share one path. */
function buildArgs(draft: RuleDraft): { ok: true; value: Record<string, unknown> } | { ok: false; error: string } {
  return draftRequest(draft);
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
