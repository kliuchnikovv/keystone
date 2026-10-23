// Rooms. The hub protocol has no room field — an entity is just an id, a device type
// and a name — so a room is read off the name prefix (`kitchen_plug`, `hall_sensor`).
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

/** The room an entity belongs to, or null when its name does not say. */
export function roomOf(e: Entity): string | null {
  if (e.name === null) return null;
  const parts = e.name.split('_');
  const head = parts[0] ?? '';
  const one = ROOM_WORDS[head];
  if (one !== undefined) return one;
  if (parts.length > 1) {
    const two = ROOM_WORDS[`${head}_${parts[1]}`];
    if (two !== undefined) return two;
  }
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