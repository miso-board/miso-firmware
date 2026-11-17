// A colour scheme worked out from the tuning itself.
//
// The width of the fifth picks a base colour, so a tuning has a look as well as
// a sound: red at the flat end, warming through orange to gold around the
// meantones, green where the fifth is pure, then blue and on to magenta as it
// widens. The rows of a scheme are then spread around it, in steps that look
// the same size whichever part of the colour wheel they fall in.
//
// Everything here is a pure function of the fifth and of which row is asked
// for, so the same tuning and the same scheme always give the same colours,
// however many rows there are.

import { parseHex, toHex } from "./ledColor";
import type { GeneratorParams, Partition, PianoColors } from "./colorMaps";

// --- the colour wheel, measured ----------------------------------------------
//
// Hue in degrees is not an even scale. Twenty degrees between orange and yellow
// is an obvious change; twenty degrees inside the greens is barely one — by the
// measure used here, a seventh as much. Spacing rows by hue angle therefore
// gives some tunings rows that are hard to tell apart.
//
// So the wheel is measured instead. Every vivid colour around it is placed in
// Oklab, a colour space built so that equal distances look equally different,
// and the wheel is re-marked by distance travelled rather than by angle. A
// position on it is "this far round, in units of visible difference", and
// stepping a fixed amount along it looks like the same size of step wherever
// it starts.

// Vivid, but not so pure a channel sits at zero: LEDs at a few percent
// brightness lose a colour's character when two of its channels are off.
const SATURATION = 0.92;
const VALUE = 0.98;

function hsvRgb(hueDeg: number): [number, number, number] {
  const h = (((hueDeg % 360) + 360) % 360) / 60;
  const c = VALUE * SATURATION;
  const x = c * (1 - Math.abs((h % 2) - 1));
  const m = VALUE - c;
  const [r, g, b] =
    h < 1 ? [c, x, 0] : h < 2 ? [x, c, 0] : h < 3 ? [0, c, x]
    : h < 4 ? [0, x, c] : h < 5 ? [x, 0, c] : [c, 0, x];
  return [r + m, g + m, b + m];
}

/** sRGB (0..1 per channel) to Oklab. Björn Ottosson's published matrices. */
function oklab([r8, g8, b8]: [number, number, number]): [number, number, number] {
  const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const r = lin(r8), g = lin(g8), b = lin(b8);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}

/** How different two colours look: their distance in Oklab. About 0.02 is just noticeable. */
export function colourDifference(hexA: string, hexB: string): number {
  const lab = (hex: string) => oklab(parseHex(hex).map((c) => c / 255) as [number, number, number]);
  const [a, b] = [lab(hexA), lab(hexB)];
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// The wheel in half-degree slices, with the distance travelled to reach each.
const SLICES = 720;
const TRAVELLED: number[] = (() => {
  const out = [0];
  let prev = oklab(hsvRgb(0));
  for (let i = 1; i <= SLICES; i++) {
    const here = oklab(hsvRgb((i * 360) / SLICES));
    out.push(out[i - 1] + Math.hypot(here[0] - prev[0], here[1] - prev[1], here[2] - prev[2]));
    prev = here;
  }
  return out;
})();

/** Once round the wheel, in Oklab distance — a little over 2. */
export const WHEEL_LENGTH: number = TRAVELLED[SLICES];

/** How far round the wheel a hue (degrees) is. */
function positionOfHue(hueDeg: number): number {
  const at = ((((hueDeg % 360) + 360) % 360) * SLICES) / 360;
  const i = Math.floor(at);
  return TRAVELLED[i] + (at - i) * (TRAVELLED[Math.min(SLICES, i + 1)] - TRAVELLED[i]);
}

/** The hue (degrees) that far round the wheel; positions wrap. */
function hueAtPosition(position: number): number {
  const p = ((position % WHEEL_LENGTH) + WHEEL_LENGTH) % WHEEL_LENGTH;
  let lo = 0, hi = SLICES;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (TRAVELLED[mid] <= p) lo = mid;
    else hi = mid;
  }
  const span = TRAVELLED[hi] - TRAVELLED[lo];
  return ((lo + (span > 0 ? (p - TRAVELLED[lo]) / span : 0)) * 360) / SLICES;
}

// --- the tuning's colour -----------------------------------------------------

/**
 * Fifth width (cents) to a colour, through fixed points named by their hue.
 * Between them the colour moves evenly along the measured wheel, so it never
 * jumps and never dawdles through a stretch that all looks alike.
 */
const STOPS: readonly (readonly [number, number])[] = (
  [
    [4800 / 7, 0], //    7-EDO                 red
    [13200 / 19, 22], // 19-EDO                deep orange
    [21600 / 31, 45], // 31-EDO                gold
    [1200 * Math.log2(1.5), 120], // pure 3:2  green
    [15600 / 22, 225], // 22-EDO               blue
    [720, 310], //       5-EDO                 magenta
  ] as const
).map(([cents, hue]) => [cents, positionOfHue(hue)] as const);

/** Where on the measured wheel a tuning's base colour sits. */
function tuningPosition(fifthCents: number): number {
  const first = STOPS[0];
  const last = STOPS[STOPS.length - 1];
  if (!(fifthCents > first[0])) return first[1];
  if (fifthCents >= last[0]) return last[1];
  for (let i = 1; i < STOPS.length; i++) {
    const [c1, p1] = STOPS[i];
    if (fifthCents <= c1) {
      const [c0, p0] = STOPS[i - 1];
      return p0 + ((fifthCents - c0) / (c1 - c0)) * (p1 - p0);
    }
  }
  return last[1];
}

/** The base hue, in degrees, of a tuning whose fifth is this many cents wide. */
export function tuningHue(fifthCents: number): number {
  return hueAtPosition(tuningPosition(fifthCents));
}

// --- telling rows apart ------------------------------------------------------
//
// The rows have to be told apart in two places, and they are not the same.
//
// On a SCREEN a colour is what sRGB says it is, and Oklab distance measures how
// different two look. On the LEDS it is not: an LED's light is proportional to
// the number it is sent, with none of a screen's gamma, and its three emitters
// are purer than a screen's. Halfway between two primaries on the wheel comes
// out much closer to their mixture than a screen shows, so colours bunch around
// yellow, cyan and magenta — and where the green emitter is doing most of the
// work, adding a little red or blue to it hardly shows at all. The eye also has
// nothing to compare brightness against on a lit key, so what separates two
// keys is hue far more than lightness.
//
// A row is therefore placed where it is far enough from the last by ALL of
// these: different enough on a screen, different enough as LED light once
// brightness is set aside, and far enough round in hue as LED light. Whichever
// is hardest to satisfy at that point on the wheel decides, which is what
// stretches the steps out through the greens, cyans and purples.

/** Rough chromaticity (CIE x, y) and relative luminance of an SK6812's emitters. */
const LED_PRIMARIES: readonly (readonly [number, number, number])[] = [
  [0.69, 0.31, 0.3], // red, ~623 nm
  [0.17, 0.7, 0.6], //  green, ~523 nm
  [0.13, 0.07, 0.1], // blue, ~468 nm
];

/** CIE XYZ to Oklab. */
function oklabFromXyz([x, y, z]: [number, number, number]): [number, number, number] {
  const l = Math.cbrt(0.8189330101 * x + 0.3618667424 * y - 0.1288597137 * z);
  const m = Math.cbrt(0.0329845436 * x + 0.9293118715 * y + 0.0361456387 * z);
  const s = Math.cbrt(0.0482003018 * x + 0.2643662691 * y + 0.633851707 * z);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}

/**
 * A colour as an LED emits it, in Oklab, scaled to a fixed brightness — so two
 * of these differ only in what colour the light is, not in how much there is.
 */
function ledLight(rgb: [number, number, number]): [number, number, number] {
  let x = 0, y = 0, z = 0;
  rgb.forEach((drive, i) => {
    const [px, py, lum] = LED_PRIMARIES[i];
    x += (drive * lum * px) / py;
    y += drive * lum;
    z += (drive * lum * (1 - px - py)) / py;
  });
  const k = 0.3 / y;
  return oklabFromXyz([x * k, y * k, z * k]);
}

/** How different two colours look as LED light: Oklab distance, and hue angle in degrees. */
export function ledDifference(hexA: string, hexB: string): { distance: number; hue: number } {
  const light = (hex: string) => ledLight(parseHex(hex).map((c) => c / 255) as [number, number, number]);
  return ledBetween(light(hexA), light(hexB));
}

function ledBetween(a: [number, number, number], b: [number, number, number]) {
  let hue = (Math.abs(Math.atan2(a[2], a[1]) - Math.atan2(b[2], b[1])) * 180) / Math.PI;
  if (hue > 180) hue = 360 - hue;
  return { distance: Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]), hue };
}

/** What neighbouring rows must differ by, for a scheme of `rows` rows. */
export interface RowSpacing {
  /** Oklab distance as a screen shows the colours. */
  screen: number;
  /** Oklab distance as LED light, at equal brightness. */
  led: number;
  /** Hue angle as LED light, in degrees. */
  ledHue: number;
}

/**
 * How far apart neighbouring rows sit, for a scheme of `rows` rows.
 *
 * Comfortably more than "just noticeable" even at its smallest, and the more
 * rows there are the closer they pack, so the scheme as a whole still reads as
 * the tuning's colour rather than as a rainbow.
 */
export function rowSpacing(rows: number): RowSpacing {
  // 1 at five rows; a little more room with fewer, down to 0.6 with eight or more.
  const scale = Math.min(1.2, Math.max(0.6, 4 / Math.max(1, rows - 1)));
  return { screen: 0.08 * scale, led: 0.046 * scale, ledHue: 13.5 * scale };
}

/**
 * The colour of one row.
 *
 * `offset` is how many rows it sits from the scheme's home row — the naturals,
 * or whichever partition the anchor names. Home takes the tuning's colour
 * itself; rows on the sharp side step one way round the wheel and rows on the
 * flat side the other.
 */
export function autoColor(fifthCents: number, offset: number, rows: number): string {
  const spacing = rowSpacing(rows);
  const direction = Math.sign(offset);
  let position = tuningPosition(fifthCents);
  // One row at a time, each far enough from the last to be told apart.
  for (let k = 0; k < Math.abs(offset); k++) {
    position = stepAlong(position, direction, spacing);
  }
  return colourAt(position);
}

function colourAt(position: number): string {
  const [r, g, b] = hsvRgb(hueAtPosition(position));
  return toHex([r * 255, g * 255, b * 255]);
}

/** The nearest position, going `direction` round the wheel, that is far enough away by every measure. */
function stepAlong(from: number, direction: number, spacing: RowSpacing): number {
  const rgbAt = (position: number) => hsvRgb(hueAtPosition(position));
  const originScreen = oklab(rgbAt(from));
  const originLed = ledLight(rgbAt(from));
  const farEnough = (position: number) => {
    const rgb = rgbAt(position);
    const screen = oklab(rgb);
    if (Math.hypot(screen[0] - originScreen[0], screen[1] - originScreen[1], screen[2] - originScreen[2]) < spacing.screen) {
      return false;
    }
    const led = ledBetween(ledLight(rgb), originLed);
    return led.distance >= spacing.led && led.hue >= spacing.ledHue;
  };
  // Creep outward to the first point that qualifies, then close in on it. No
  // single step may take more than a third of the wheel.
  const creep = spacing.screen / 8;
  let near = 0;
  let far = creep;
  while (!farEnough(from + direction * far) && far < WHEEL_LENGTH / 3) {
    near = far;
    far += creep;
  }
  for (let i = 0; i < 24; i++) {
    const mid = (near + far) / 2;
    if (farEnough(from + direction * mid)) far = mid;
    else near = mid;
  }
  return from + direction * far;
}

/** Row `i` of `n`, as a signed distance from row `home`, wrapping the short way round. */
function signedOffset(i: number, home: number, n: number): number {
  const below = Math.floor((n - 1) / 2);
  return ((((i - home + below) % n) + n) % n) - below;
}

/**
 * The generated colours for every scheme a generator holds, leaving everything
 * else about it — how many rows, their widths, the anchor — as it is.
 *
 * All three schemes are recoloured together, so switching between them while
 * the colours follow the tuning never shows a stale palette.
 */
export function autoColorsFor(
  g: GeneratorParams,
  fifthCents: number,
): Pick<GeneratorParams, "palette" | "piano" | "partitions"> {
  // A scheme of only two colours has room to put them further apart, and needs
  // it: two neighbouring rows are the closest pair a bigger scheme allows, which
  // is thin when they are all there is. So a pair is spaced like the flats and
  // sharps of a five-row scheme — two of its steps, with a row's width between.
  const PAIR_ROWS = 5;
  const row = (offset: number, rows: number) =>
    rows === 2 ? autoColor(fifthCents, offset * 2, PAIR_ROWS) : autoColor(fifthCents, offset, rows);

  // Accidental rows: the naturals are home, and a row's offset is its accidental.
  // Symmetric rows are ♮, ♯, ♯♯ … and stand for twice as many less one.
  const n = g.palette.length;
  const palette = g.symmetricRows
    ? g.palette.map((_, i) => row(i, 2 * n - 1))
    : g.palette.map((_, i) => row(g.startAccidental + i, n));

  // Piano: naturals at home, sharps and flats a pair's distance to the FLAT
  // side — the colours an accidental scheme of just ♭ and ♮ rows gets, which is
  // the pairing that looks best. The two centre keys each sit one step beyond
  // their own group, on the side away from the other, so neither is mistaken
  // for its neighbours — and switching them on does not move the two main
  // colours.
  const piano: PianoColors = {
    ...g.piano,
    natural: autoColor(fifthCents, 0, PAIR_ROWS),
    accidental: autoColor(fifthCents, -2, PAIR_ROWS),
    centreNatural: autoColor(fifthCents, 1, PAIR_ROWS),
    centreAccidental: autoColor(fifthCents, -3, PAIR_ROWS),
  };

  // Partitions: home is the one the anchor names.
  const count = g.partitions.length;
  // Symmetric partitions are centre-first, so the centre is home and the rest
  // step outward from it.
  const partitions: Partition[] = g.symmetricPartitions
    ? g.partitions.map((part, i) => ({ ...part, color: row(i, 2 * count - 1) }))
    : g.partitions.map((part, i) => ({
    ...part,
    // A pair of partitions is coloured as the piano pair is: the other one sits
    // to the flat side of home.
    color: row(count === 2 ? -Math.abs(signedOffset(i, g.anchorPartition ?? 0, count))
                           : signedOffset(i, g.anchorPartition ?? 0, count), count),
    }));

  return { palette, piano, partitions };
}

/** Hue of a stored colour in degrees, for tests and diagnostics. */
export function hueOf(hex: string): number {
  const [r, g, b] = parseHex(hex).map((c) => c / 255);
  const max = Math.max(r, g, b);
  const d = max - Math.min(r, g, b);
  if (d === 0) return 0;
  const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return (h * 60 + 360) % 360;
}

// --- the default palette, extended -------------------------------------------
//
// The default rows already walk round the colour wheel: red (♭♭), orange (♭),
// yellow (♮), green (♯), blue (♯♯). More accidentals continue that walk from
// both ends into the stretch the five leave unused — on from blue through
// violet, back from red through rose — spaced evenly by how different they
// look, so the two sides meet in magenta without any two colours crowding.
// That gives nine distinct hues, ♭⁴ to ♯⁴. Further out the nine come round
// again, paler with each lap.

const DEFAULT_ROWS: Readonly<Record<number, string>> = {
  [-2]: "#fa1414",
  [-1]: "#fa6414",
  0: "#c8c814",
  1: "#32fa32",
  2: "#3232fa",
};

// The stretch of the wheel from the default blue round to its red, and the
// four new hues spaced along it: ♯♯♯, ♯⁴, then ♭⁴, ♭♭♭ coming back to red.
const BLUE_POSITION = positionOfHue(hueOf(DEFAULT_ROWS[2]));
const RED_POSITION = WHEEL_LENGTH + positionOfHue(hueOf(DEFAULT_ROWS[-2]));
const EXTRA_ROWS: Readonly<Record<number, string>> = Object.fromEntries(
  ([[3, 1], [4, 2], [-4, 3], [-3, 4]] as const).map(([accidental, step]) => [
    accidental,
    colourAt(BLUE_POSITION + ((RED_POSITION - BLUE_POSITION) * step) / 5),
  ]),
);

/** How far each further lap of the nine moves toward white. */
const LAP_PALER = 0.35;

/**
 * The colour the default palette gives an accidental row, for any accidental:
 * the five defaults, four more hues to ♭⁴ and ♯⁴, and beyond that the nine
 * again, paler on each lap out.
 */
export function defaultRowColor(accidental: number): string {
  const base = (((accidental + 4) % 9) + 9) % 9 - 4;
  const lap = Math.floor((accidental + 4) / 9);
  const hex = DEFAULT_ROWS[base] ?? EXTRA_ROWS[base];
  if (lap === 0) return hex;
  const k = Math.min(0.75, LAP_PALER * Math.abs(lap));
  const [r, g, b] = parseHex(hex);
  return toHex([r + (255 - r) * k, g + (255 - g) * k, b + (255 - b) * k]);
}

/** The most rows the boards' colour frame can carry. Mirrors COLORGEN_PAL_MAX. */
const BOARD_ROWS_MAX = 8;

/**
 * Accidental rows grown to give every accidental an EDO wraps to a colour of
 * its own, or null if they already do.
 *
 * Wrapped, an EDO uses a fixed set of accidentals — ♭ to ♯ in 12, ♭♭♭ to ♯♯♯
 * in 41 — and rows short of that would cycle, giving a remote sharp a flat's
 * colour. So the missing rows are added from the default sequence. Nothing is
 * ever removed or recoloured, and the rows stop at what the boards can carry:
 * past that (53-EDO and up), the outermost flat shares the outermost sharp's
 * colour — the two runs that meet where the EDO wraps round.
 */
export function rowsForWrap(
  g: Pick<GeneratorParams, "palette" | "startAccidental" | "symmetricRows">,
  accidentals: readonly number[],
): Pick<GeneratorParams, "palette" | "startAccidental"> | null {
  const lo = Math.min(...accidentals);
  const hi = Math.max(...accidentals);
  const n = g.palette.length;

  if (g.symmetricRows) {
    // Centre-first: ♮, ♯, ♯♯ … mirrored, so the furthest accidental either
    // way is what is needed — within what the mirrored list can fit.
    const want = Math.min(Math.max(-lo, hi) + 1, Math.floor((BOARD_ROWS_MAX + 1) / 2));
    if (want <= n) return null;
    return {
      palette: [...g.palette, ...Array.from({ length: want - n }, (_, i) => defaultRowColor(n + i))],
      startAccidental: g.startAccidental,
    };
  }

  const start = g.startAccidental;
  const end = start + n - 1;
  let from = Math.min(start, lo);
  let to = Math.max(end, hi);
  // Keep within the boards' limit, giving up the flat side's extra row first.
  while (to - from + 1 > Math.max(BOARD_ROWS_MAX, n)) {
    if (from < start) from++;
    else if (to > end) to--;
    else break;
  }
  if (from === start && to === end) return null;
  return {
    palette: Array.from({ length: to - from + 1 }, (_, i) => {
      const a = from + i;
      return a >= start && a <= end ? g.palette[a - start] : defaultRowColor(a);
    }),
    startAccidental: from,
  };
}
