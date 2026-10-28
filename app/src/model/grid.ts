// The device grid: buttons of different shapes, laid out the way a CSS grid
// would. The base unit is small — a button with little to say takes one cell
// of a three-column grid, not half the screen. A wide button takes the whole
// row (the room mock's button), a square one a cell, a tall one a cell that
// reaches down.
import type { Entity } from '../protocol/messages';
import { primarySwitch, readingAttrs } from './devices';

export type DeviceShape = 'square' | 'wide' | 'tall';

/** How many cells fit across the screen. */
export const GRID_COLUMNS = 3;
export const GRID_GAP = 10;
/** The height of a wide button — one row, as in the room mock. */
export const GRID_ROW_HEIGHT = 66;
/** A tall button reaches this many cells down. */
export const TALL_RATIO = 1.35;

/** The shape of a device's button: a sensor reads as a square, a switchable
 * device as the mock's wide button, a device with several readings as a tall
 * one — it has more to say and the room to say it in. */
export function deviceShape(e: Entity): DeviceShape {
  const readings = readingAttrs(e);
  const count = (readings.first ? 1 : 0) + readings.rest.length;
  if (count >= 2) return 'tall';
  if (primarySwitch(e) === undefined) return 'square';
  return 'wide';
}

export interface GridItem {
  id: number;
  shape: DeviceShape;
}

export interface GridBox {
  id: number;
  shape: DeviceShape;
  x: number;
  y: number;
  width: number;
  height: number;
}

interface Placed {
  id: number;
  shape: DeviceShape;
  x: number;
  width: number;
  height: number;
}

interface Row {
  items: Placed[];
  height: number;
}

/** The width of one cell of the grid. */
export function gridColumn(width: number): number {
  return Math.max(0, (width - GRID_GAP * (GRID_COLUMNS - 1)) / GRID_COLUMNS);
}

/** Places the devices on the grid. A wide button starts a row of its own; the
 * others share the row, and the row is as tall as its tallest button. */
export function gridLayout(items: GridItem[], width: number): { boxes: GridBox[]; height: number } {
  const column = gridColumn(width);
  const rows: Row[] = [];
  let current: Row = { items: [], height: 0 };
  let cell = 0;

  const closeRow = () => {
    if (current.items.length > 0) rows.push(current);
    current = { items: [], height: 0 };
    cell = 0;
  };

  for (const item of items) {
    if (item.shape === 'wide') {
      closeRow();
      rows.push({ items: [{ id: item.id, shape: item.shape, x: 0, width, height: GRID_ROW_HEIGHT }], height: GRID_ROW_HEIGHT });
      continue;
    }
    if (cell >= GRID_COLUMNS) closeRow();
    const h = item.shape === 'square' ? column : Math.round(column * TALL_RATIO);
    current.items.push({ id: item.id, shape: item.shape, x: cell * (column + GRID_GAP), width: column, height: h });
    current.height = Math.max(current.height, h);
    cell += 1;
  }
  closeRow();

  const boxes: GridBox[] = [];
  let y = 0;
  for (const row of rows) {
    for (const it of row.items) boxes.push({ id: it.id, shape: it.shape, x: it.x, y, width: it.width, height: it.height });
    y += row.height + GRID_GAP;
  }
  return { boxes, height: Math.max(0, y - GRID_GAP) };
}
