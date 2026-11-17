// The geometry of the small "2x2 boards" pictures used to stand for a layout or
// a preset: four boards tiled the way the hardware tiles, as hexagons ready to
// draw. What colours them is the caller's business.

import { LED_POS, axialToPixel, hexPoints } from "./layout";
import { RIGHT, DOWN } from "./mesh.svelte";
import type { LayoutId } from "./tuning";

const S = 10; // hex radius in icon units
const PAD = S * 1.2;

// One board, its right-hand neighbour, the one below, and the one diagonally
// across.
const BOARD_OFFSETS: readonly (readonly [number, number])[] = [
  [0, 0],
  RIGHT,
  DOWN,
  [RIGHT[0] + DOWN[0], RIGHT[1] + DOWN[1]],
];

export interface IconKey {
  /** Absolute grid coordinate. */
  x: number;
  y: number;
  /** SVG `points` for this key's hexagon. */
  points: string;
}

const placed = BOARD_OFFSETS.flatMap(([ox, oy]) =>
  LED_POS.map(([lx, ly]) => {
    const x = lx + ox;
    const y = ly + oy;
    const [px, py] = axialToPixel(x, y, S);
    return { x, y, px, py, points: hexPoints(px, py, S * 0.94) };
  }),
);

export const ICON_KEYS: readonly IconKey[] = placed.map(({ x, y, points }) => ({ x, y, points }));

export const ICON_VIEW_BOX: string = (() => {
  const xs = placed.map((k) => k.px);
  const ys = placed.map((k) => k.py);
  const minX = Math.min(...xs) - PAD;
  const minY = Math.min(...ys) - PAD;
  return `${minX} ${minY} ${Math.max(...xs) + PAD - minX} ${Math.max(...ys) + PAD - minY}`;
})();

/**
 * Which stretch of the pattern an icon shows under each layout, as a grid offset
 * from the boards' own coordinates. Purely a matter of framing: these two read
 * better moved (2, 1) along from where the anchor happens to put them, which
 * means each icon key samples the colour from (2, 1) back.
 */
export const ICON_SHIFT: Partial<Record<LayoutId, readonly [number, number]>> = {
  "wicki-hayden": [-2, -1],
  "harmonic-table": [-2, -1],
};
