// The phone's presence: where its owner is right now.
//
// A phone is not a radio device on the hub — it has no chip and no Zigbee
// endpoint. But it can say "I am home" or "I am away", and a rule should react
// to that exactly as it reacts to a motion sensor.
//
// The trick lives on the hub: a phone is a VIRTUAL ENTITY there, with one
// boolean attribute ("home"), so it looks like a device that reports `occupied`.
// This module is the app's half: it decides when to say "home" and it turns the
// hub's answer into words.
//
// How the phone decides is deliberately NOT decided here — see
// `docs/automations.md` §5.2. The app can be told by a person ("I'm home"), by
// the native geofence watcher, or by the Wi-Fi it sees. This module only knows
// the vocabulary: the name the hub knows the phone by, and the value.

import type { Entity } from '../protocol/messages';
import { deviceTitle } from './devices';

/** The name the hub knows this phone by. It becomes the phone's entity name. */
export const PRESENCE_PHONE_NAME = 'My phone';

/**
 * The state of the phone's presence, as the hub sees it.
 *
 * `unknown` is a real state, not an error: the phone has not reported yet, or
 * it stopped reporting long enough that the hub stopped believing it. Showing
 * "Away" for a phone that is simply silent would switch the lights off.
 */
export type PresenceState = 'home' | 'away' | 'unknown';

export interface PhonePresence {
  /** The phone's entity id on the hub. */
  entity: number;
  /** The name the hub knows it by. */
  name: string | null;
  /** Whether the hub believes the answer. */
  known: boolean;
  /** Whether the owner is home. Only meaningful when `known`. */
  home: boolean;
}

/** The phone's entity in the hub's list, if there is one. */
export function phoneEntity(entities: Entity[]): Entity | undefined {
  return entities.find((e) => e.device === 'phone');
}

/**
 * What the phone's presence is right now, from the hub's entity list.
 *
 * The value comes from the entity's "home" attribute. A phone whose value is
 * null has not reported (or has gone quiet) — that is "unknown", not "away".
 */
export function presenceState(entities: Entity[]): PresenceState {
  const phone = phoneEntity(entities);
  if (!phone) return 'unknown';
  const attr = phone.attrs.find((a) => a.name === 'home');
  if (!attr || attr.value === null || attr.value === undefined) return 'unknown';
  return attr.value === true ? 'home' : 'away';
}

/** The presence as the hub reports it in the `presence` event, if it has one. */
export function presenceFromEvent(phones: PhonePresence[] | null | undefined): PhonePresence | null {
  if (!phones || phones.length === 0) return null;
  return phones[0] ?? null;
}

/** The presence in words, for a card or a line of text. */
export function presenceText(state: PresenceState): string {
  switch (state) {
    case 'home':
      return 'At home';
    case 'away':
      return 'Away';
    case 'unknown':
      return 'Not known yet';
  }
}

/**
 * The name to show for the phone: what the person named it, or a default.
 * The hub knows it by the name the app sent ("My phone") unless the person
 * renamed the device in the app.
 */
export function presenceTitle(entity: Entity | undefined): string {
  if (!entity) return PRESENCE_PHONE_NAME;
  return deviceTitle(entity);
}

/**
 * The arguments of `presence.report`: the name the hub knows the phone by and
 * whether the owner is home.
 *
 * The name is what makes each phone its own entity, so two phones in one house
 * are two entities and two rules.
 */
export function presenceReportArgs(name: string, home: boolean): Record<string, unknown> {
  return { name, home };
}
