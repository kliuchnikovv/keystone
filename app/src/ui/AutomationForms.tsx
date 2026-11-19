import { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import {
  addableSettings, captureScene, describeScene, draftsFromCurrentState, draftsRequest, sceneDrafts, inputUnit, sceneRequest,
  type AttrOption,
  type SceneDraft,
} from '../model/automation';
import { deviceTitle } from '../model/devices';
import type { Entity, Scene } from '../protocol/messages';
import { Button, Chip, ChipRow, ErrorText, Field, Input, Picker } from './controls';
import { useTheme } from './theme';

type Submit = (args: Record<string, unknown>) => Promise<boolean>;

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

// ---- editing a scene -------------------------------------------------------------------------

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
  setting: { marginTop: 10, gap: 6 },
});
