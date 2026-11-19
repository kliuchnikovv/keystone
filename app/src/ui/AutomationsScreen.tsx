import { useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { Platform } from 'react-native';
import { describeRule, describeScene, describeSchedule, describeTimer } from '../model/automation';
import type { Commands } from '../hooks/useCommands';
import { useCountdown } from '../hooks/useCountdown';
import type { HomeState } from '../protocol/store';
import { SceneEditor, SceneForm } from './AutomationForms';
import { NewAutomation } from './NewAutomation';
import { Button, Chip, ChipRow, ConfirmDelete } from './controls';
import { ScreenHeader } from './ScreenHeader';
import { useTheme } from './theme';

type Section = 'automations' | 'scenes';

/** Which form is open. The id tells the forms apart: a late answer to one form
 * must not close another one that has been opened in the meantime. */
interface OpenForm {
  section: Section;
  id: number;
}

/**
 * The automations of the home, in one list.
 *
 * The hub keeps three kinds of them (rules, schedules, timers), but a person does not
 * think in kinds: they think "when this, do that". So they all stand in one list, each
 * row saying in words when it happens and what it does. Scenes are a different thing —
 * a reusable block of settings, not something that happens — so they keep their own
 * section.
 */
export function AutomationsScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [section, setSection] = useState<Section>('automations');
  const [adding, setAdding] = useState<OpenForm | null>(null);
  const [editing, setEditing] = useState<string | null>(null); // the name of the scene being edited
  const lastId = useRef(0);
  const left = useCountdown(home.timers);

  const switchTo = (s: Section) => {
    setSection(s);
    setAdding(null);
    setEditing(null);
  };
  const open = (s: Section) => {
    lastId.current += 1;
    setAdding({ section: s, id: lastId.current });
  };
  /** Close only the form that asked — another one may be open by now. */
  const close = (mine: OpenForm | null) =>
    setAdding((cur) => (cur && mine && cur.id === mine.id ? null : cur));

  // The model already decided which of the hub's three calls the new automation is,
  // so the screen only sends what it was given.
  const addAutomation = async (op: string, args: Record<string, unknown>) => {
    const mine = adding;
    const ok = (await commands.request(op, args)) !== null;
    if (ok) {
      close(mine);
      commands.notify('Automation saved');
    }
    return ok;
  };
  const addScene = async (args: Record<string, unknown>) => {
    const mine = adding;
    const ok = (await commands.request('scene.put', args)) !== null;
    if (ok) {
      close(mine);
      commands.notify('Scene saved');
    }
    return ok;
  };
  const del = (op: string, index: number) => () => void commands.request(op, { index });

  const empty = (text: string) => <Text style={{ color: t.muted, paddingVertical: 24, textAlign: 'center' }}>{text}</Text>;
  const card = (
    key: string,
    title: string,
    detail: string,
    onDelete: () => void,
    label: string,
    onEdit?: () => void,
    onApply?: () => void,
  ) => (
    <View key={key} accessibilityLabel={label} style={[styles.item, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <View style={{ flex: 1 }}>
        <Text style={[styles.itemTitle, { color: t.text }]}>{title}</Text>
        <Text style={[styles.itemDetail, { color: t.muted }]}>{detail}</Text>
      </View>
      {onApply && <Button kind="plain" size="mini" label="Apply" a11y={`Apply ${label}`} onPress={onApply} />}
      {onEdit && <Button kind="plain" size="mini" label="Edit" a11y={`Edit ${label}`} onPress={onEdit} />}
      <ConfirmDelete size="mini" a11y={`Delete ${label}`} onConfirm={onDelete} />
    </View>
  );

  // One row per automation, whatever kind it is inside the hub. The wording is the one
  // each kind already had: a rule reads as a sentence, a schedule as a time, a timer as
  // a countdown.
  const rows = [
    ...home.rules.map((r) => {
      const d = describeRule(r, home.entities);
      return {
        key: `r${r.index}`,
        title: `${d.when}, ${d.then}`,
        detail: d.extras.join(' · '),
        label: `rule ${r.index + 1}`,
        onDelete: del('rule.del', r.index),
      };
    }),
    ...home.schedules.map((s) => {
      const d = describeSchedule(s, home.entities, { hasLocation: !!home.hub?.location });
      return { key: `s${s.index}`, title: d.title, detail: d.detail, label: `schedule ${d.title}`, onDelete: del('schedule.del', s.index) };
    }),
    ...home.timers.map((tm) => {
      const d = describeTimer(tm, home.entities, left(tm));
      return { key: `t${tm.index}`, title: d.title, detail: d.detail, label: `timer ${d.title}`, onDelete: del('timer.cancel', tm.index) };
    }),
  ];

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader title="Automations" />
      <ChipRow>
        <Chip label={`Automations (${rows.length})`} a11y="All automations" selected={section === 'automations'} onPress={() => switchTo('automations')} />
        <Chip label={`Scenes (${home.scenes.length})`} a11y="Scenes" selected={section === 'scenes'} onPress={() => switchTo('scenes')} />
      </ChipRow>

      <View style={{ marginTop: 14 }}>
        {section === 'automations' && (
          <>
            {adding?.section === 'automations' ? (
              <NewAutomation
                entities={home.entities}
                scenes={home.scenes}
                hasLocation={!!home.hub?.location}
                busy={commands.busy}
                onSubmit={addAutomation}
                onCancel={() => setAdding(null)}
              />
            ) : (
              <Button label="Create automation" onPress={() => open('automations')} />
            )}
            <View style={{ marginTop: 12 }}>
              {rows.length === 0 && !adding && empty('No automations yet. One of them says when something happens and what happens then.')}
              {rows.map((r) => card(r.key, r.title, r.detail, r.onDelete, r.label))}
            </View>
          </>
        )}

        {section === 'scenes' && (
          <>
            {adding?.section === 'scenes' ? (
              <SceneForm entities={home.entities} busy={commands.busy} onSubmit={addScene} onCancel={() => setAdding(null)} />
            ) : (
              <Button label="Add scene" onPress={() => open('scenes')} />
            )}
            <View style={{ marginTop: 12 }}>
              {home.scenes.length === 0 && !adding && empty('No scenes. A scene sets several devices at once: "evening" = lamp on, plug off.')}
              {home.scenes.map((sc) => {
                if (editing === sc.name) {
                  return (
                    <SceneEditor
                      key={`edit${sc.name}`}
                      scene={sc}
                      entities={home.entities}
                      busy={commands.busy}
                      onSubmit={async (args) => {
                        const ok = (await commands.request('scene.put', args)) !== null;
                        if (ok) {
                          setEditing(null);
                          commands.notify('Scene saved');
                        }
                        return ok;
                      }}
                      onCancel={() => setEditing(null)}
                    />
                  );
                }
                const d = describeScene(sc, home.entities);
                return card(
                  `sc${sc.name}`,
                  d.title,
                  d.detail,
                  () => void commands.request('scene.del', { name: sc.name }),
                  `scene ${sc.name}`,
                  () => { setAdding(null); setEditing(sc.name); },
                  async () => {
                    const res = await commands.request('scene.apply', { name: sc.name });
                    if (res === null) return;
                    const sent = Number(res.sent ?? 0);
                    const skipped = Number(res.skipped ?? 0);
                    commands.notify(skipped > 0 ? `Scene '${sc.name}' applied: ${sent} sent, ${skipped} skipped` : `Scene '${sc.name}' applied`);
                  },
                );
              })}
            </View>
          </>
        )}
      </View>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: Platform.OS === 'ios' ? 110 : 98 },
  item: { flexDirection: 'row', alignItems: 'center', borderWidth: 1,
          paddingVertical: 12, paddingHorizontal: 13, marginBottom: 8, gap: 10 },
  itemTitle: { fontSize: 14, fontWeight: '600' },
  itemDetail: { fontSize: 11.5, marginTop: 1 },
});
