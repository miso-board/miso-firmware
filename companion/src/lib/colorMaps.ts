// Colour semantics for one preset's LED mapping: what colour a coordinate gets,
// and how a board's 93 RGB bytes are laid out.
//
// Pure — no reactive state, no storage, no transport. Those live in
// presets.svelte.ts, which owns the document this module interprets.
//
// A mapping is keyed by ABSOLUTE grid coordinate ("x,y"), not by LED index, so
// one scheme covers the whole grid however many boards are attached and
// survives boards being added, removed or rearranged. That also matches how the
// procedural generator already worked: it colours a key by the accidental of
// the pitch at its coordinate, which is a function of absolute position.

import { NUM_KEYS, LED_POS, cellKey } from "./layout";
import { LAYOUTS, pitchAt, type LayoutId } from "./tuning";
import { scaleHex, mixWhite, parseHex } from "./ledColor";

/** Parameters of a procedurally generated (row-coloured) scheme. */
export interface GeneratorParams {
  layout: LayoutId;
  /** Row colours at full brightness; `brightness` dims them on write. */
  palette: string[];
  /** The accidental that receives palette[0]. */
  startAccidental: number;
  /** Added to every board coordinate before the pitch is computed. */
  offset: [number, number];
  /** Percent of full brightness written to the LEDs. */
  brightness: number;
  /** Tint the letters C, D and E paler, as an orientation landmark. */
  lightenCDE?: boolean;
}

/** How far toward white a C/D/E key is mixed. Paler, not brighter, so it
 *  stays distinct from the firmware's velocity glow (which brightens a key's
 *  own colour while held). */
const CDE_LIGHTEN = 0.35;

/**
 * A preset's LED mapping.
 *
 * `generator` is optional here — unlike a pitch map's, which is mandatory. A
 * hand-painted colour layer has no parameters, and a coordinate nobody painted
 * can safely be black. See `PitchMap` in tuning.ts for why pitch cannot work
 * that way.
 */
export interface ColorLayer {
  /** Explicit colours by "x,y". A generated layer fills the gaps from `generator`. */
  colors: Record<string, string>;
  /** Present when the mapping is procedural rather than hand-painted. */
  generator?: GeneratorParams;
}

// Chosen so that at 10% brightness these reproduce the board's boot pattern
// byte for byte (250 -> 25, 100 -> 10, 20 -> 2 …).
export const DEFAULT_GENERATOR: GeneratorParams = {
  layout: "bosanquet",
  palette: ["#fa1414", "#fa6414", "#c8c814", "#32fa32", "#3232fa"],
  startAccidental: -2,
  offset: [0, 0],
  brightness: 10,
  lightenCDE: false,
};

/** Colour for one coordinate under a procedural scheme. */
export function generateColorAt(p: GeneratorParams, x: number, y: number): string {
  const layout = LAYOUTS[p.layout];
  const n = Math.max(1, p.palette.length);
  const pitch = pitchAt(layout, x, y, p.offset);
  // Rows outside the palette wrap back through it.
  const idx = (((pitch.accidental - p.startAccidental) % n) + n) % n;
  let hex = p.palette[idx] ?? "#000000";
  if (p.lightenCDE && pitch.pc7 < 3) hex = mixWhite(hex, CDE_LIGHTEN);
  return scaleHex(hex, p.brightness);
}

/**
 * Colour of a coordinate under a mapping. Generated schemes are evaluated
 * lazily rather than materialised, so attaching another board needs no
 * regeneration — its keys simply resolve.
 */
export function colorAt(m: ColorLayer, x: number, y: number): string {
  const explicit = m.colors[cellKey(x, y)];
  if (explicit !== undefined) return explicit;
  if (m.generator) return generateColorAt(m.generator, x, y);
  return "#000000";
}

// Mirrors the firmware boot pattern (fill_bosanquet groups by LED index),
// so a fresh install starts from what the board already shows.
const BOSANQUET_GROUPS: Record<string, number[]> = {
  "#190202": [0, 5, 6, 18, 19], // double-flat
  "#190a02": [1, 3, 4, 7, 16, 17, 20], // flat
  "#141402": [2, 8, 9, 15, 21, 22, 28], // natural
  "#051905": [10, 13, 14, 23, 26, 27, 29], // sharp
  "#050519": [11, 12, 24, 25, 30], // double-sharp
};

/** An LED-indexed array from the old storage format is a board at the origin. */
export function coloursFromLedArray(arr: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  arr.forEach((hex, led) => {
    const pos = LED_POS[led];
    if (pos) out[cellKey(pos[0], pos[1])] = String(hex);
  });
  return out;
}

/** The board's boot colours, as a hand-painted layer. */
export function bosanquetDefaultColors(): ColorLayer {
  const arr = Array(NUM_KEYS).fill("#000000");
  for (const [hex, leds] of Object.entries(BOSANQUET_GROUPS)) {
    for (const l of leds) arr[l] = hex;
  }
  return { colors: coloursFromLedArray(arr) };
}

/** 93 RGB bytes for one board, in LED-chain order, from absolute coordinates. */
export function rgbForBoard(
  m: ColorLayer,
  ox: number,
  oy: number,
  into: Uint8Array,
  at: number,
): void {
  LED_POS.forEach(([lx, ly], led) => {
    const [r, g, b] = parseHex(colorAt(m, lx + ox, ly + oy));
    into[at + led * 3] = r;
    into[at + led * 3 + 1] = g;
    into[at + led * 3 + 2] = b;
  });
}
