import { describe, expect, it } from 'vitest';
import { groupByRoom, roomCount, roomOf } from '../src/model/rooms';
import type { Entity } from '../src/protocol/messages';

function device(name: string | null, id = 0x0100): Entity {
  return { id, chip: 1, device: 'light', name, attrs: [] };
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