// Rooms: where a device lives.
//
// The hub knows the room of a device (the person assigns it); the app falls
// back to the room its name suggests (`kitchen_plug`, `Living room lamp`) for
// the devices nobody assigned one to. An explicit room always wins.
import type { Entity } from '../protocol/messages';
import { primarySwitch } from './devices';

/** A leading word of a device name that names the room it lives in. */
const ROOM_WORDS: Record<string, string> = {
  living: 'Living room',
  lounge: 'Living room',
  sitting: 'Living room',
  kitchen: 'Kitchen',
  bedroom: 'Bedroom',
  bed: 'Bedroom',
  bath: 'Bathroom',
  bathroom: 'Bathroom',
  shower: 'Bathroom',
  toilet: 'Bathroom',
  hall: 'Hallway',
  hallway: 'Hallway',
  corridor: 'Hallway',
  dining: 'Dining room',
  office: 'Office',
  study: 'Office',
  garage: 'Garage',
  cellar: 'Cellar',
  basement: 'Basement',
  laundry: 'Laundry',
  nursery: 'Nursery',
  kids: 'Kids room',
  garden: 'Garden',
  balcony: 'Balcony',
  terrace: 'Terrace',
  porch: 'Porch',
  attic: 'Attic',
  home: 'Ground floor',
  ground: 'Ground floor',
};

/** Two-word room names, as a person would write them. */
const ROOM_PHRASES: Record<string, string> = {
  'living room': 'Living room',
  'dining room': 'Dining room',
  'kids room': 'Kids room',
  'guest room': 'Guest room',
  'utility room': 'Utility room',
};

/** The room a name suggests, or null when the name does not say. */
export function roomFromName(name: string | null): string | null {
  if (name === null) return null;
  const words = name.split(/[\s_]+/).filter((w) => w.length > 0);
  const head = words[0];
  if (head === undefined) return null;
  const second = words[1];
  if (second !== undefined) {
    const phrase = ROOM_PHRASES[`${head.toLowerCase()} ${second.toLowerCase()}`];
    if (phrase !== undefined) return phrase;
  }
  const one = ROOM_WORDS[head.toLowerCase()];
  if (one !== undefined) return one;
  return null;
}

/** The room a device is in: the one the person assigned, else the one its name suggests. */
export function roomOf(e: Entity): string | null {
  if (e.room !== null && e.room !== undefined && e.room !== '') return e.room;
  return roomFromName(e.name);
}

/** Whether the room was assigned (not guessed from the name). */
export function hasOwnRoom(e: Entity): boolean {
  return e.room !== null && e.room !== undefined && e.room !== '';
}

export interface Room {
  /** null for devices whose name does not name a room. */
  room: string | null;
  devices: Entity[];
}

/** Devices grouped by room, rooms alphabetically, unassigned devices last. */
export function groupByRoom(entities: Entity[]): Room[] {
  const byRoom = new Map<string, Entity[]>();
  for (const e of entities) {
    const key = roomOf(e) ?? '';
    const list = byRoom.get(key);
    if (list) list.push(e);
    else byRoom.set(key, [e]);
  }
  return [...byRoom.entries()]
    .sort((a, b) => (a[0] === '' ? 1 : b[0] === '' ? -1 : a[0].localeCompare(b[0])))
    .map(([room, devices]) => ({
      room: room === '' ? null : room,
      devices: [...devices].sort((a, b) => {
        const aSensor = primarySwitch(a) === undefined;
        const bSensor = primarySwitch(b) === undefined;
        if (aSensor !== bSensor) return aSensor ? -1 : 1;
        if (a.name !== null && b.name !== null) return a.name.localeCompare(b.name);
        if (a.name === null !== (b.name === null)) return a.name === null ? 1 : -1;
        return a.id - b.id;
      }),
    }));
}

/** How many rooms have at least one device. */
export function roomCount(entities: Entity[]): number {
  return new Set(entities.map(roomOf).filter((r): r is string => r !== null)).size;
}

/** Every room name the app knows how to read off a device name, sorted. */
export function allRooms(): string[] {
  return [...new Set(Object.values(ROOM_WORDS).concat(Object.values(ROOM_PHRASES)))].sort();
}

/** The part of a device name that is not the leading room, if any. */
export function stripRoomPrefix(name: string | null): string {
  if (!name) return '';
  const words = name.split(/[\s_]+/).filter((w) => w.length > 0);
  if (words.length === 0) return '';
  // Two-word room?
  if (words.length >= 2) {
    const first = words[0];
    const second = words[1];
    if (first !== undefined && second !== undefined) {
      const phrase = `${first.toLowerCase()} ${second.toLowerCase()}`;
      if (ROOM_PHRASES[phrase] !== undefined) return words.slice(2).join(' ');
    }
  }
  const head = words[0];
  if (head !== undefined && ROOM_WORDS[head.toLowerCase()] !== undefined) return words.slice(1).join(' ');
  return name;
}

/** Every room in the home, once each, alphabetically: what the picker offers.
 *
 * The rooms the devices are actually in come first-class; the ones the app only
 * knows from a name are offered too, so a person can move a device into a room
 * nobody uses yet without typing it.
 */
export function roomChoices(entities: Entity[]): string[] {
  const rooms = new Set<string>(allRooms());
  for (const e of entities) {
    const own = hasOwnRoom(e) ? e.room : null;
    if (own !== null) rooms.add(own);
    const guessed = roomFromName(e.name);
    if (guessed !== null) rooms.add(guessed);
  }
  return [...rooms].sort((a, b) => a.localeCompare(b));
}
