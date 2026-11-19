// The way into a new automation: examples first, then the builder.
//
// An empty form is frightening, so the flow starts with a handful of ready-made
// automations, described in words. Picking one opens the builder with everything that
// does not depend on this home already filled in; "from scratch" asks what starts the
// automation and opens the same builder empty.

import { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { PRESET_AUTOMATIONS, emptyAutomationDraft, type AutomationDraft } from '../model/automation';
import type { Entity, Scene } from '../protocol/messages';
import { AutomationBuilder, TRIGGER_KINDS } from './AutomationBuilder';
import { Button, Choice, ChipRow } from './controls';
import { useTheme } from './theme';

export interface NewAutomationProps {
  entities: Entity[];
  scenes: Scene[];
  hasLocation: boolean;
  busy: boolean;
  onSubmit(op: string, args: Record<string, unknown>): Promise<boolean>;
  onCancel(): void;
}

/** One example in the list: its icon and what it does, in words. */
function ExampleRow({ icon, label, onPress }: { icon: string; label: string; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      onPress={onPress}
      style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}
    >
      <Text style={{ fontSize: 20 }}>{icon}</Text>
      <Text style={{ color: t.text, fontSize: 14, fontWeight: '600', flex: 1 }}>{label}</Text>
      <Text style={{ color: t.muted, fontSize: 16 }}>›</Text>
    </Pressable>
  );
}

export function NewAutomation({ entities, scenes, hasLocation, busy, onSubmit, onCancel }: NewAutomationProps) {
  const t = useTheme();
  // null = the examples; a draft = the builder, with that draft in it.
  const [draft, setDraft] = useState<AutomationDraft | null>(null);
  const [askingKind, setAskingKind] = useState(false);

  if (draft) {
    return (
      <AutomationBuilder
        entities={entities}
        scenes={scenes}
        hasLocation={hasLocation}
        initial={draft}
        busy={busy}
        onSubmit={onSubmit}
        onCancel={onCancel}
      />
    );
  }

  if (askingKind) {
    return (
      <View>
        <Text style={[styles.title, { color: t.text }]}>What starts it?</Text>
        <Text style={{ color: t.muted, fontSize: 12.5, marginTop: 4, marginBottom: 12 }}>
          A device that does something, a time of day, a date, or simply a wait.
        </Text>
        <ChipRow>
          <Choice
            label="When: what starts it"
            placeholder="Choose what starts it"
            options={TRIGGER_KINDS}
            selected={null}
            onSelect={(k) => setDraft(emptyAutomationDraft(k.kind))}
            keyOf={(k) => k.kind}
            textOf={(k) => `${k.icon} ${k.label}`}
            optionLabel={(k) => `Choose ${k.label}`}
          />
        </ChipRow>
        <View style={styles.buttons}>
          <Button kind="secondary" label="Cancel" onPress={onCancel} />
        </View>
      </View>
    );
  }

  return (
    <View>
      <Text style={[styles.title, { color: t.text }]}>What should happen?</Text>
      <Text style={{ color: t.muted, fontSize: 12.5, marginTop: 4, marginBottom: 12 }}>
        Pick an example and change what it says, or start from nothing.
      </Text>
      {PRESET_AUTOMATIONS.map((p) => (
        <ExampleRow key={p.id} icon={p.icon} label={p.label} onPress={() => setDraft(p.make())} />
      ))}
      <ExampleRow icon="✎" label="From scratch" onPress={() => setAskingKind(true)} />
      <View style={styles.buttons}>
        <Button kind="secondary" label="Cancel" onPress={onCancel} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  title: { fontSize: 17, fontWeight: '700' },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, borderWidth: 1, borderRadius: 14, paddingHorizontal: 13, paddingVertical: 12, marginBottom: 8 },
  buttons: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 16 },
});
