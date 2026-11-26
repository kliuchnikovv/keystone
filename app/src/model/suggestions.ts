// The automations the home can already do.
//
// A suggestion is not a rule that was written by hand — it is a rule the
// devices in this house could run: a motion sensor and a light in the same
// room, a plug that reports its power, a lamp that is left on. Every
// suggestion is derived from what is really there; nothing is invented.
import { attrOf, deviceTitle, primarySwitch } from './devices';
import { roomOf } from './rooms';
import { emptyDraft, switchTargets, type ActionDraft, type ConditionDraft, type RuleDraft } from './automation';
import type { Attr, Entity } from '../protocol/messages';
import type { HomeState } from '../protocol/store';

export interface Suggestion {
  /** A stable key, so the app can tell one suggestion from another. */
  id: string;
  title: string;
  detail: string;
  /** The rule, already filled in — only Save is left to press. */
  draft: RuleDraft;
}

/** A condition on an attribute: "is true", "is above 60". */
function condition(entity: Entity, attr: Attr, value: string): ConditionDraft {
  return { option: { entity: entity.id, attr: attr.id, label: `${deviceTitle(entity)} · ${attr.name}`, name: attr.name, vt: attr.vt, writable: false }, op: '==', value };
}

/** An action on an attribute: "turn on", "set to 50". */
function action(entity: Entity, attr: Attr, value: string): ActionDraft {
  return { option: { entity: entity.id, attr: attr.id, label: `${deviceTitle(entity)} · ${attr.name}`, name: attr.name, vt: attr.vt, writable: true }, value };
}

/** The rule that turns a light on when there is motion. */
export function motionRule(motion: Entity, light: Entity): RuleDraft {
  const occupied = attrOf(motion, 'occupied');
  const sw = primarySwitch(light);
  const draft = emptyDraft();
  if (occupied !== undefined) draft.trigger = condition(motion, occupied, 'on');
  if (sw !== undefined) draft.action = action(light, sw, 'on');
  return draft;
}

/** Every suggestion this home can run, best first. */
export function suggestionsFor(home: HomeState): Suggestion[] {
  const entities = home.entities;
  const out: Suggestion[] = [];

  // A motion sensor and a light in the same room.
  for (const motion of entities) {
    if (motion.device !== 'occupancy') continue;
    const room = roomOf(motion);
    const light = switchTargets(entities).find(
      (e) => e.device === 'light' && (room === null || roomOf(e) === room),
    );
    if (!light) continue;
    const sw = primarySwitch(light);
    if (!sw) continue;
    out.push({
      id: `motion-${motion.id}-${light.id}`,
      title: `${deviceTitle(light)} on when there is motion`,
      detail: `You have a motion sensor and a light${room ? ` in the ${room}` : ''}.`,
      draft: motionRule(motion, light),
    });
    break;
  }

  // A plug that reports its power: a warning when it draws too much.
  const plug = entities.find((e) => e.device === 'switch' && attrOf(e, 'power_w') !== undefined);
  if (plug) {
    const watt = attrOf(plug, 'power_w');
    const sw = primarySwitch(plug);
    if (watt && sw) {
      const draft = emptyDraft();
      draft.trigger = condition(plug, watt, '1500');
      draft.action = action(plug, sw, 'off');
      out.push({
        id: `power-${plug.id}`,
        title: `${deviceTitle(plug)} off when it draws too much`,
        detail: 'Your plug reports its power.',
        draft,
      });
    }
  }

  // Lights and sockets that could be turned off at sunrise.
  const lamps = switchTargets(entities).filter((e) => e.device === 'light');
  if (lamps.length > 0) {
    const first = lamps[0]!;
    const sw = primarySwitch(first);
    if (sw) {
      const draft = emptyDraft();
      draft.trigger = null;
      draft.action = action(first, sw, 'off');
      out.push({
        id: 'sunrise-off',
        title: 'Everything off at sunrise',
        detail: `${lamps.length} light${lamps.length === 1 ? '' : 's'} you could switch off in the morning.`,
        draft,
      });
    }
  }

  return out;
}
