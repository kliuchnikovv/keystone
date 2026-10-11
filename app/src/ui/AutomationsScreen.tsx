import { useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { describeRule, describeScene, describeSchedule, describeTimer } from '../model/automation';
import type { Commands } from '../hooks/useCommands';
import { useCountdown } from '../hooks/useCountdown';
import type { HomeState } from '../protocol/store';
import { RuleFormView, SceneForm, ScheduleForm, TimerForm } from './AutomationForms';
import { Button, Chip, ChipRow, ConfirmDelete } from './controls';
import { useTheme } from './theme';

type Section = 'scenes' | 'schedules' | 'timers' | 'rules';

export function AutomationsScreen({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [section, setSection] = useState<Section>('scenes');
  const [adding, setAdding] = useState(false);
  const left = useCountdown(home.timers);

  const switchTo = (s: Section) => {
    setSection(s);
    setAdding(false);
  };
  const add = (op: string) => async (args: Record<string, unknown>) => {
    const ok = (await commands.request(op, args)) !== null;
    if (ok) setAdding(false);
    return ok;
  };
  const del = (op: string, index: number) => () => void commands.request(op, { index });

  const empty = (text: string) => <Text style={{ color: t.muted, paddingVertical: 24, textAlign: 'center' }}>{text}</Text>;
  const card = (key: string, title: string, detail: string, onDelete: () => void, label: string) => (
    <View key={key} accessibilityLabel={label} style={[styles.item, { backgroundColor: t.card, borderColor: t.border }]}>
      <View style={{ flex: 1 }}>
        <Text style={[styles.itemTitle, { color: t.text }]}>{title}</Text>
        <Text style={{ color: t.muted, marginTop: 2 }}>{detail}</Text>
      </View>
      <ConfirmDelete a11y={`Delete ${label}`} onConfirm={onDelete} />
    </View>
  );

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ChipRow>
        <Chip label={`Scenes (${home.scenes.length})`} a11y="Scenes" selected={section === 'scenes'} onPress={() => switchTo('scenes')} />
        <Chip label={`Schedules (${home.schedules.length})`} a11y="Schedules" selected={section === 'schedules'} onPress={() => switchTo('schedules')} />
        <Chip label={`Timers (${home.timers.length})`} a11y="Timers" selected={section === 'timers'} onPress={() => switchTo('timers')} />
        <Chip label={`Rules (${home.rules.length})`} a11y="Rules" selected={section === 'rules'} onPress={() => switchTo('rules')} />
      </ChipRow>

      <View style={{ marginTop: 14 }}>
        {section === 'scenes' && (
          <>
            {adding ? (
              <SceneForm entities={home.entities} busy={commands.busy} onSubmit={add('scene.put')} onCancel={() => setAdding(false)} />
            ) : (
              <Button label="Add scene" onPress={() => setAdding(true)} />
            )}
            <View style={{ marginTop: 12 }}>
              {home.scenes.length === 0 && !adding && empty('No scenes. A scene sets several devices at once: "evening" = lamp on, plug off.')}
              {home.scenes.map((sc) => {
                const d = describeScene(sc, home.entities);
                return card(`sc${sc.name}`, d.title, d.detail, () => void commands.request('scene.del', { name: sc.name }), `scene ${sc.name}`);
              })}
            </View>
          </>
        )}

        {section === 'schedules' && (
          <>
            {adding ? (
              <ScheduleForm entities={home.entities} scenes={home.scenes} hasLocation={!!home.hub?.location} busy={commands.busy} onSubmit={add('schedule.add')} onCancel={() => setAdding(false)} />
            ) : (
              <Button label="Add schedule" onPress={() => setAdding(true)} />
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
            {adding ? (
              <TimerForm entities={home.entities} scenes={home.scenes} busy={commands.busy} onSubmit={add('timer.add')} onCancel={() => setAdding(false)} />
            ) : (
              <Button label="Add timer" onPress={() => setAdding(true)} />
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
            {adding ? (
              <RuleFormView entities={home.entities} scenes={home.scenes} hasLocation={!!home.hub?.location} busy={commands.busy} onSubmit={add('rule.add')} onCancel={() => setAdding(false)} />
            ) : (
              <Button label="Add rule" onPress={() => setAdding(true)} />
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
  page: { padding: 16 },
  item: { flexDirection: 'row', alignItems: 'center', borderWidth: 1, borderRadius: 14, padding: 14, marginBottom: 10, gap: 10 },
  itemTitle: { fontSize: 15, fontWeight: '600' },
});
