import { describe, expect, it } from 'vitest';
import { allRooms, groupByRoom, hasOwnRoom, roomChoices, roomCount, roomFromName, roomOf } from '../src/model/rooms';
import type { Entity } from '../src/protocol/messages';

function device(name: string | null, id = 0x0100, room: string | null = null): Entity {
  return { id, chip: 1, device: 'light', name, room, attrs: [] };
}

describe('rooms come from the device name', () => {
  it('reads the leading word as the room', () => {
    expect(roomOf(device('kitchen_plug'))).toBe('Kitchen');
    expect(roomOf(device('bedroom_lamp'))).toBe('Bedroom');
    expect(roomOf(device('hall_sensor'))).toBe('Hallway');
    expect(roomOf(device('garage_door'))).toBe('Garage');
  });

  it('groups a two-word room', () => {
    expect(roomOf(device('living_room_plug'))).toBe('Living room');
    expect(roomOf(device('dining_room_light'))).toBe('Dining room');
  });

  it('reads a room out of a name a person wrote', () => {
    expect(roomOf(device('Living room lamp'))).toBe('Living room');
    expect(roomOf(device('Kitchen plug'))).toBe('Kitchen');
    expect(roomOf(device('Bedroom light'))).toBe('Bedroom');
    expect(roomOf(device('Dining room light'))).toBe('Dining room');
    expect(roomOf(device('living room plug'))).toBe('Living room');
  });

  it('leaves a name that does not name a room alone', () => {
    // `front_door` starts with "front", which is not a room — better unassigned than wrong.
    expect(roomOf(device('front_door'))).toBeNull();
    expect(roomOf(device('lamp'))).toBeNull();
    expect(roomOf(device('plug1'))).toBeNull();
    expect(roomOf(device(null))).toBeNull();
  });

  it('groups devices by room, unassigned last, keeping the given order', () => {
    const rooms = groupByRoom([device('plug1', 1), device('kitchen_plug', 2), device('bedroom_lamp', 3)]);
    expect(rooms.map((r) => r.room)).toEqual(['Bedroom', 'Kitchen', null]);
    expect(rooms[1]?.devices.map((d) => d.id)).toEqual([2]);
    expect(rooms[2]?.devices.map((d) => d.id)).toEqual([1]);
  });

  it('counts only rooms that have a device', () => {
    expect(roomCount([device('kitchen_plug', 1), device('kitchen_lamp', 2), device('strip', 3)])).toBe(1);
    expect(roomCount([])).toBe(0);
  });
});

describe('the room the person assigned', () => {
  it('wins over the one the name suggests', () => {
    expect(roomOf(device('kitchen_plug', 1, 'Office'))).toBe('Office');
    expect(hasOwnRoom(device('kitchen_plug', 1, 'Office'))).toBe(true);
    expect(hasOwnRoom(device('kitchen_plug', 1))).toBe(false);
  });

  it('groups by the assigned room, even when the name says another', () => {
    const rooms = groupByRoom([device('kitchen_plug', 1, 'Office'), device('kitchen_lamp', 2)]);
    expect(rooms.map((r) => r.room)).toEqual(['Kitchen', 'Office']);
    expect(rooms[1]?.devices.map((d) => d.id)).toEqual([1]);
  });

  it('a device with a room but no name still has a room', () => {
    expect(roomOf(device(null, 1, 'Cellar'))).toBe('Cellar');
    expect(roomFromName(null)).toBeNull();
  });

  it('the picker offers the rooms in the home and the ones a name can name', () => {
    const choices = roomChoices([device('kitchen_plug', 1), device('strip', 2, 'Studio')]);
    expect(choices).toContain('Kitchen');
    expect(choices).toContain('Studio');
    for (const room of allRooms()) expect(choices).toContain(room);
    expect([...choices].sort()).toEqual(choices); // alphabetical
    expect(new Set(choices).size).toBe(choices.length); // once each
  });
});