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

import { MapVec } from "meantonal";
import { LED_POS, cellKey } from "./layout";
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

/** An LED-indexed array from the old storage format is a board at the origin. */
export function coloursFromLedArray(arr: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  arr.forEach((hex, led) => {
    const pos = LED_POS[led];
    if (pos) out[cellKey(pos[0], pos[1])] = String(hex);
  });
  return out;
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

// --- The `K` wire frame ------------------------------------------------------

/** Palette entries the firmware's frame can carry. */
export const COLORGEN_PAL_MAX = 8;

export const COLORGEN_FRAME_LEN = 41; // 'K' + 40 payload bytes

/**
 * The generator parameters, as the firmware wants them — the colour counterpart
 * of `pitchFrame`.
 *
 * 'K' · int16 m00, m01, m10, m11 · int16 aw, ah · int8 start accidental ·
 * u8 palette length · u8 flags · u8 brightness · 8 × RGB, little-endian.
 *
 * Forty bytes replace the 93-per-board table `L` sends, and because the firmware
 * evaluates them at absolute coordinates, one frame colours every board in the
 * mesh — including one attached later, which is the point: the parameters travel
 * down the tree, so a board hot-plugged with no host connected still comes up in
 * the right colours.
 *
 * Two things are folded on the way out. `offset` disappears into the anchor,
 * since `M·(v + o) + a` is `M·v + (M·o + a)` — so the wire carries the same six
 * integers of geometry `P` does. `brightness` does NOT fold into the palette,
 * because `lightenCDE` is applied per key BEFORE scaling and rounds on the way
 * (see `generateColorAt`), so pre-scaling would shift some channels by a count.
 *
 * Returns null when the scheme cannot be expressed as parameters: a layer with
 * no generator (hand-painted), or a palette longer than the frame can carry. The
 * caller falls back to per-board `L` frames.
 */
export function colorgenFrame(p: GeneratorParams): Uint8Array | null {
  if (p.palette.length < 1 || p.palette.length > COLORGEN_PAL_MAX) return null;

  const layout = LAYOUTS[p.layout];
  const o = layout.matrix.map(new MapVec(p.offset[0], p.offset[1]));

  const frame = new Uint8Array(COLORGEN_FRAME_LEN);
  frame[0] = 0x4b; // 'K'
  const dv = new DataView(frame.buffer);
  // Signed: Wicki-Hayden's matrix is (1, −2, 0, −1).
  dv.setInt16(1, layout.matrix.m00, true);
  dv.setInt16(3, layout.matrix.m01, true);
  dv.setInt16(5, layout.matrix.m10, true);
  dv.setInt16(7, layout.matrix.m11, true);
  dv.setInt16(9, o.x + layout.anchor.w, true);
  dv.setInt16(11, o.y + layout.anchor.h, true);
  dv.setInt8(13, p.startAccidental);
  frame[14] = p.palette.length;
  frame[15] = p.lightenCDE ? 0x01 : 0x00;
  frame[16] = Math.round(p.brightness);

  // At FULL brightness: the board scales, so that it can apply the C/D/E tint
  // first and round exactly where the host does.
  p.palette.forEach((hex, i) => {
    const [r, g, b] = parseHex(hex);
    frame[17 + i * 3] = r;
    frame[17 + i * 3 + 1] = g;
    frame[17 + i * 3 + 2] = b;
  });
  return frame;
}
