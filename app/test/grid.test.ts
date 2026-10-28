// The device grid: a shape per device and a layout that gives every button its place.
import { describe, expect, it } from 'vitest';
import { GRID_COLUMNS, GRID_GAP, GRID_ROW_HEIGHT, deviceShape, gridColumn, gridLayout } from '../src/model/grid';
import type { Entity } from '../src/protocol/messages';

function device(id: number, kind: 'switch' | 'sensor' | 'climate', name: string | null = null): Entity {
  if (kind === 'switch') {
    return { id, chip: 1, device: 'switch', name, attrs: [{ id: 0x01, name: 'on_off', vt: 'bool', flags: ['r', 'w'], value: true }] };
  }
  if (kind === 'sensor') {
    return { id, chip: 1, device: 'sensor', name, attrs: [{ id: 0x50, name: 'temperature', vt: 'i16', flags: ['r'], value: 21 }] };
  }
  return {
    id,
    chip: 1,
    device: 'climate',
    name,
    attrs: [
      { id: 0x20, name: 'temperature', vt: 'i16', flags: ['r'], value: 21 },
      { id: 0x21, name: 'humidity', vt: 'u8', flags: ['r'], value: 48 },
    ],
  };
}

describe('the shape of a device button', () => {
  it('a switchable device is the wide button of the room mock', () => {
    expect(deviceShape(device(1, 'switch'))).toBe('wide');
  });

  it('a sensor with one reading is a square', () => {
    expect(deviceShape(device(2, 'sensor'))).toBe('square');
  });

  it('a device with more to say is tall', () => {
    expect(deviceShape(device(3, 'climate'))).toBe('tall');
  });
});

describe('the grid', () => {
  const width = 358;  // a phone without the screen's margins
  const column = gridColumn(width);

  it('a cell is a third of the width — the button is compact', () => {
    expect(GRID_COLUMNS).toBe(3);
    expect(column).toBeCloseTo((width - GRID_GAP * 2) / 3);
    expect(column).toBeLessThan(width / 2);
  });

  it('a wide button takes the whole row', () => {
    const { boxes, height } = gridLayout([{ id: 1, shape: 'wide' }], width);
    expect(boxes).toHaveLength(1);
    expect(boxes[0]).toMatchObject({ id: 1, x: 0, y: 0, width, height: GRID_ROW_HEIGHT });
    expect(height).toBe(GRID_ROW_HEIGHT);
  });

  it('three squares share a row', () => {
    const { boxes, height } = gridLayout(
      [{ id: 1, shape: 'square' }, { id: 2, shape: 'square' }, { id: 3, shape: 'square' }],
      width,
    );
    expect(boxes[0]).toMatchObject({ x: 0, y: 0, width: column, height: column });
    expect(boxes[1]).toMatchObject({ x: column + GRID_GAP, y: 0 });
    expect(boxes[2]).toMatchObject({ x: (column + GRID_GAP) * 2, y: 0 });
    expect(height).toBe(column);
  });

  it('a fourth square starts the next row', () => {
    const { boxes } = gridLayout(
      [{ id: 1, shape: 'square' }, { id: 2, shape: 'square' }, { id: 3, shape: 'square' }, { id: 4, shape: 'square' }],
      width,
    );
    expect(boxes[3]).toMatchObject({ x: 0, y: column + GRID_GAP });
  });

  it('a wide button starts its own row', () => {
    const { boxes } = gridLayout(
      [{ id: 1, shape: 'square' }, { id: 2, shape: 'wide' }, { id: 3, shape: 'square' }],
      width,
    );
    expect(boxes[0]).toMatchObject({ id: 1, y: 0 });
    expect(boxes[1]).toMatchObject({ id: 2, y: column + GRID_GAP, width });
    // The third one waits for the wide button's row.
    expect(boxes[2]).toMatchObject({ id: 3, y: column + GRID_GAP + GRID_ROW_HEIGHT + GRID_GAP });
  });

  it('a tall button makes its row taller', () => {
    const { boxes, height } = gridLayout(
      [{ id: 1, shape: 'tall' }, { id: 2, shape: 'square' }],
      width,
    );
    const tall = Math.round(column * 1.35);
    expect(boxes[0]).toMatchObject({ id: 1, height: tall, y: 0 });
    expect(boxes[1]).toMatchObject({ id: 2, height: column, y: 0 });
    expect(height).toBe(tall);
  });

  it('an empty grid is empty', () => {
    expect(gridLayout([], width)).toEqual({ boxes: [], height: 0 });
  });
});
