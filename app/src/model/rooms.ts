// Rooms. The hub protocol has no room field — an entity is just an id, a device type
// and a name — so a room is read off the name (`kitchen_plug`, `Living room lamp`).
// Anything we do not recognise stays unassigned rather than being guessed into a room.
import type { Entity } from '../protocol/messages';

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

/** The room an entity belongs to, or null when its name does not say. */
export function roomOf(e: Entity): string | null {
  if (e.name === null) return null;
  const words = e.name.split(/[\s_]+/).filter((w) => w.length > 0);
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
    .map(([room, devices]) => ({ room: room === '' ? null : room, devices }));
}

/** How many rooms have at least one device. */
export function roomCount(entities: Entity[]): number {
  return new Set(entities.map(roomOf).filter((r): r is string => r !== null)).size;
}