// Canonical Miso board layout, traced from the physical board 2026-09-08.
//
// The board is a sparse 7x7 grid of AXIAL hex coordinates: (x, y) with
// x = column from left, y = row from top; occupied cells form the hexagonal
// board outline (2/4/6/7/6/4/2 keys per row). Axial means the square grid is
// sheared so integer coordinates land on hexagon centerpoints — each key has
// six neighbors: (±1, 0), (0, ±1), and one adjacent diagonal pair. Rendering
// the physical board therefore applies a shear transform, not row offsets.
//
// Three index spaces:
//  - sensor index: firmware scan order (M1 ch 0-15 -> 0-15, M2 ch 0-14 -> 16-30)
//  - LED index: position in the SK6812 chain (serpentine, column by column)
//  - grid coordinate: physical position, shared by a cell's sensor and LED

export const NUM_KEYS = 31;
export const GRID_W = 7;
export const GRID_H = 7;

/** Grid coordinate of each LED chain index. */
export const LED_POS: readonly (readonly [number, number])[] = [
  [0, 4], [0, 3], [1, 2], [1, 3], [1, 4], [1, 5], [2, 5], [2, 4],
  [2, 3], [2, 2], [2, 1], [2, 0], [3, 0], [3, 1], [3, 2], [3, 3],
  [3, 4], [3, 5], [3, 6], [4, 6], [4, 5], [4, 4], [4, 3], [4, 2],
  [4, 1], [5, 1], [5, 2], [5, 3], [5, 4], [6, 3], [6, 2],
];

/** LED chain index for each sensor index (mirrors led_for_sensor[] in main.c). */
export const LED_FOR_SENSOR: readonly number[] = [
  8, 0, 4, 15, 23, 24, 25, 14, 12, 13,
  9, 11, 10, 2, 1, 3, 18, 19, 21, 20,
  28, 29, 27, 30, 22, 26, 7, 5, 17, 16,
  6,
];

/** Sensor index for each LED chain index (inverse of LED_FOR_SENSOR). */
export const SENSOR_FOR_LED: readonly number[] = (() => {
  const inv = new Array<number>(NUM_KEYS);
  LED_FOR_SENSOR.forEach((led, sensor) => (inv[led] = sensor));
  return inv;
})();

/** Grid coordinate of each sensor index. */
export const SENSOR_POS: readonly (readonly [number, number])[] = LED_FOR_SENSOR.map(
  (led) => LED_POS[led],
);

// ---------------------------------------------------------------------------
// Screen geometry
// ---------------------------------------------------------------------------

/**
 * Octave direction in axial coordinates.
 *
 * Inferred from the Bosanquet boot pattern, whose accidental classes fall in
 * 3-wide bands of (x − 2y): that reads as (1,0) = whole tone = 5 steps of
 * 31-EDO and (0,1) = diatonic semitone = 3 steps, making (5,2) exactly 31
 * steps — one octave. Confirm this before the pitch mapping depends on it.
 */
export const OCTAVE_AXIAL: readonly [number, number] = [5, 2];

const SQ3 = Math.sqrt(3);

/** Unrotated pointy-top axial layout, in units of the hex radius. */
function axialRaw(x: number, y: number): [number, number] {
  return [SQ3 * (x + y / 2), 1.5 * y];
}

/**
 * Rotation (radians) that lays the octave direction on the horizontal — the
 * standard Bosanquet orientation. Applied counter-clockwise on screen, where
 * y grows downward. Derived from OCTAVE_AXIAL so it stays correct if the
 * underlying mapping ever changes.
 */
export const BOARD_ROTATION: number = (() => {
  const [px, py] = axialRaw(OCTAVE_AXIAL[0], OCTAVE_AXIAL[1]);
  return Math.atan2(py, px);
})();

/** Axial coordinate to rotated screen position, in units of the hex radius. */
export function axialToPixel(x: number, y: number, size = 1): [number, number] {
  const [px, py] = axialRaw(x, y);
  const c = Math.cos(BOARD_ROTATION);
  const s = Math.sin(BOARD_ROTATION);
  return [size * (px * c + py * s), size * (-px * s + py * c)];
}

/** Rotated screen position of each LED, in units of the hex radius. */
export const LED_PIXEL: readonly (readonly [number, number])[] = LED_POS.map(([x, y]) =>
  axialToPixel(x, y),
);
