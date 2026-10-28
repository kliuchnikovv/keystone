import { useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { Platform } from 'react-native';
import { describeRule, describeScene, describeSchedule, describeTimer } from '../model/automation';
import type { Commands } from '../hooks/useCommands';
import { useCountdown } from '../hooks/useCountdown';
import type { HomeState } from '../protocol/store';
import { RuleFormView, SceneEditor, SceneForm, ScheduleForm, TimerForm } from './AutomationForms';
import { Button, Chip, ChipRow, ConfirmDelete } from './controls';
import { ScreenHeader } from './ScreenHeader';
import { useTheme } from './theme';

type Section = 'scenes' | 'schedules' | 'timers' | 'rules';

/** Which form is open. The id tells the forms apart: a late answer to one form
 * must not close another one that has been opened in the meantime. */
interface OpenForm {
  section: Section;
  id: number;
}

export function AutomationsScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [section, setSection] = useState<Section>('scenes');
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
  const add = (op: string, done?: string) => async (args: Record<string, unknown>) => {
    const mine = adding;
    const ok = (await commands.request(op, args)) !== null;
    if (ok) {
      // Close only the form that asked — another one may be open by now.
      setAdding((cur) => (cur && mine && cur.id === mine.id ? null : cur));
      if (done) commands.notify(done);
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

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader title="Automations" />
      <ChipRow>
        <Chip label={`Scenes (${home.scenes.length})`} a11y="Scenes" selected={section === 'scenes'} onPress={() => switchTo('scenes')} />
        <Chip label={`Schedules (${home.schedules.length})`} a11y="Schedules" selected={section === 'schedules'} onPress={() => switchTo('schedules')} />
        <Chip label={`Timers (${home.timers.length})`} a11y="Timers" selected={section === 'timers'} onPress={() => switchTo('timers')} />
        <Chip label={`Rules (${home.rules.length})`} a11y="Rules" selected={section === 'rules'} onPress={() => switchTo('rules')} />
      </ChipRow>

      <View style={{ marginTop: 14 }}>
        {section === 'scenes' && (
          <>
            {adding?.section === 'scenes' ? (
              <SceneForm entities={home.entities} busy={commands.busy} onSubmit={add('scene.put', 'Scene saved')} onCancel={() => setAdding(null)} />
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

        {section === 'schedules' && (
          <>
            {adding?.section === 'schedules' ? (
              <ScheduleForm entities={home.entities} scenes={home.scenes} hasLocation={!!home.hub?.location} busy={commands.busy} onSubmit={add('schedule.add', 'Schedule saved')} onCancel={() => setAdding(null)} />
            ) : (
              <Button label="Add schedule" onPress={() => open('schedules')} />
            )}
            <View style={{ marginTop: 12 }}>
              {home.schedules.length === 0 && !adding && empty('No schedules. A schedule runs every day (or on chosen days) at a set time.')}
              {home.schedules.map((s) => {
                const d = describeSchedule(s, home.entities, { hasLocation: !!home.hub?.location });
                return card(`s${s.index}`, d.title, d.detail, del('schedule.del', s.index), `schedule ${d.title}`);
              })}
            </View>
          </>
        )}

        {section === 'timers' && (
          <>
            {adding?.section === 'timers' ? (
              <TimerForm entities={home.entities} scenes={home.scenes} busy={commands.busy} onSubmit={add('timer.add', 'Timer saved')} onCancel={() => setAdding(null)} />
            ) : (
              <Button label="Add timer" onPress={() => open('timers')} />
            )}
            <View style={{ marginTop: 12 }}>
              {home.timers.length === 0 && !adding && empty('No timers running.')}
              {home.timers.map((tm) => {
                const d = describeTimer(tm, home.entities, left(tm));
                return card(`t${tm.index}`, d.title, d.detail, del('timer.cancel', tm.index), `timer ${d.title}`);
              })}
            </View>
          </>
        )}

        {section === 'rules' && (
          <>
            {adding?.section === 'rules' ? (
              <RuleFormView entities={home.entities} scenes={home.scenes} hasLocation={!!home.hub?.location} busy={commands.busy} onSubmit={add('rule.add', 'Rule saved')} onCancel={() => setAdding(null)} />
            ) : (
              <Button label="Add rule" onPress={() => open('rules')} />
            )}
            <View style={{ marginTop: 12 }}>
              {home.rules.length === 0 && !adding && empty('No rules. A rule reacts to a change on one device by setting another.')}
              {home.rules.map((r) => {
                const d = describeRule(r, home.entities);
                return card(
                  `r${r.index}`,
                  `${d.when}, ${d.then}`,
                  d.extras.join(' · '),
                  del('rule.del', r.index),
                  `rule ${r.index + 1}`,
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
