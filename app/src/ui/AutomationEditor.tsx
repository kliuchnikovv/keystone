// Creating an automation: the chooser and the three forms.
//
// The Automations tab and the «+» menu both come here — the tab opens a form
// straight away, the menu asks what kind first.
import { useState } from 'react';
import type { Commands } from '../hooks/useCommands';
import { suggestionsFor, type Suggestion } from '../model/suggestions';
import type { RuleDraft } from '../model/automation';
import type { HomeState } from '../protocol/store';
import { AutomationChooser } from './AutomationChooser';
import { ScheduleForm, TimerForm } from './AutomationForms';
import { RuleEditor } from './RuleEditor';

export type AutomationKind = 'schedule' | 'timer' | 'rule';

export interface AutomationEditorProps {
  home: HomeState;
  commands: Commands;
  /** Which form to open straight away; null asks first. */
  initial?: AutomationKind | null;
  /** The rule a suggestion brought with it. */
  initialDraft?: RuleDraft | null;
  onClose(): void;
}

export function AutomationEditor({ home, commands, initial = null, initialDraft = null, onClose }: AutomationEditorProps) {
  const [kind, setKind] = useState<AutomationKind | null>(initial);
  const [draft, setDraft] = useState<RuleDraft | null>(initialDraft);

  const submit =
    (op: string, done: string) =>
    async (args: Record<string, unknown>): Promise<boolean> => {
      const ok = (await commands.request(op, args)) !== null;
      if (ok) {
        commands.notify(done);
        onClose();
      }
      return ok;
    };

  if (kind === 'schedule') {
    return (
      <ScheduleForm
        entities={home.entities}
        scenes={home.scenes}
        hasLocation={!!home.hub?.location}
        busy={commands.busy}
        onSubmit={submit('schedule.add', 'Schedule saved')}
        onCancel={onClose}
      />
    );
  }
  if (kind === 'timer') {
    return (
      <TimerForm
        entities={home.entities}
        scenes={home.scenes}
        busy={commands.busy}
        onSubmit={submit('timer.add', 'Timer saved')}
        onCancel={onClose}
      />
    );
  }
  if (kind === 'rule') {
    return (
      <RuleEditor
        {...(draft ? { initialDraft: draft } : {})}
        entities={home.entities}
        scenes={home.scenes}
        hasLocation={!!home.hub?.location}
        busy={commands.busy}
        onSubmit={submit('rule.add', 'Rule saved')}
        onCancel={onClose}
      />
    );
  }

  return (
    <AutomationChooser
      onBack={onClose}
      onSchedule={() => setKind('schedule')}
      onTimer={() => setKind('timer')}
      onRule={() => setKind('rule')}
      suggestions={suggestionsFor(home)}
      onPickSuggestion={(s: Suggestion) => {
        setDraft(s.draft);
        setKind('rule');
      }}
    />
  );
}
