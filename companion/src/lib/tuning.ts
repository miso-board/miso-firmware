// The one module that knows music theory, via meantonal.
//
// meantonal represents a pitch as a 2-vector (w, h) — whole steps and diatonic
// semitones above C₋₁ — where an octave is (5, 2) and a sharp is (1, −1).
// That is exactly the Miso board's axial basis (see layout.ts), so a board
// coordinate is a pitch vector plus a fixed anchor, and `pitch.accidental`
// gives the Bosanquet row directly.
//
// Two things are deliberately separate here:
//
//  - A LAYOUT is a basis change on the grid — which direction is a whole tone,
//    which is a fifth. It lives entirely in (w, h) pitch space and so is
//    independent of tuning: Bosanquet and Wicki-Hayden mean the same thing in
//    any EDO.
//  - A TUNING is one number, the width of the fifth in cents. Every tuning in
//    the meantone family is determined by it, and an EDO is just one way to
//    choose it. That is why this module needs no per-EDO step lattice.

import { Pitch, Map2D, MapVec, TuningMap, SPN, WICKI_FROM } from "meantonal";
import { cellKey } from "./layout";

export type LayoutId = "bosanquet" | "wicki-hayden";

export interface Layout {
  id: LayoutId;
  name: string;
  /** Board grid coordinates -> meantonal (w, h) pitch coordinates. */
  matrix: Map2D;
  /** Added after mapping, placing the layout at an absolute pitch. */
  anchor: { w: number; h: number };
  /** What one step along each grid axis means, for the UI. */
  axes: string;
}

/** Board axes are already meantonal's axes, so Bosanquet needs no basis change. */
const IDENTITY = new Map2D(1, 0, 0, 1);

export const LAYOUTS: Record<LayoutId, Layout> = {
  bosanquet: {
    id: "bosanquet",
    name: "Bosanquet",
    matrix: IDENTITY,
    // Puts D4 on the centre key (3,3) and reproduces the board's boot pattern.
    // This is the firmware default (TUNE_W0/TUNE_H0 in Core/Src/main.c).
    anchor: { w: 23, h: 7 },
    axes: "+x whole tone · +y diatonic semitone",
  },
  "wicki-hayden": {
    id: "wicki-hayden",
    name: "Wicki-Hayden",
    // meantonal's Wicki basis composed with a vertical flip of the board.
    //
    // WICKI_FROM alone puts fifths on the down-right diagonal, mirroring the
    // standard layout: a semitone then lands a major 7th out (verified on the
    // hardware). Flipping restores the convention — whole tone to the right,
    // fifth up-right, fourth up-left.
    //
    // In a skewed axial basis a vertical flip is (x, y) -> (x + y, -y), NOT
    // (x, -y): plain negation would break hex adjacency, turning one diagonal
    // into a major 6th. Composed this is w = x - 2y, h = -y — i.e. the matrix
    // fields read (1, -2, 0, -1), which is why they must travel as SIGNED
    // int16 in the `P` frame.
    matrix: WICKI_FROM.compose(new Map2D(1, 1, 0, -1)),
    anchor: { w: 29, h: 13 }, // also D4 on the centre key
    axes: "+x whole tone · fifth up-right · fourth up-left",
  },
};

/** The board's centre key — the natural place to pin an anchor note. */
export const CENTRE_KEY: readonly [number, number] = [3, 3];

export function pitchAt(
  layout: Layout,
  x: number,
  y: number,
  offset: readonly [number, number] = [0, 0],
): Pitch {
  const v = layout.matrix.map(new MapVec(x + offset[0], y + offset[1]));
  return new Pitch(v.x + layout.anchor.w, v.y + layout.anchor.h);
}

function glyphs(n: number): string {
  if (n === 0) return "";
  const g = n > 0 ? "♯" : "♭";
  const count = Math.abs(n);
  return count <= 3 ? g.repeat(count) : `${g}×${count}`;
}

/** Row label for an accidental: ♭♭, ♭, ♮, ♯, ♯♯, ♯×4 … */
export function accidentalLabel(n: number): string {
  return n === 0 ? "♮" : glyphs(n);
}

/** Display name of a pitch, e.g. "C♯5". */
export function noteName(p: Pitch): string {
  return `${p.letter}${glyphs(p.accidental)}${p.octave}`;
}

// --- Tuning -----------------------------------------------------------------

/**
 * A tuning, however the user chose to say it.
 *
 * Both forms reduce to a single fifth width in cents. The `edo` form is the
 * familiar one; the `fifth` form is the escape hatch, because the historical
 * temperaments are not EDOs — 1/4-comma meantone's 696.578¢ fifth gives a pure
 * 5:4 major third, and no equal division does.
 */
export type Tuning =
  | { kind: "edo"; edo: number }
  | { kind: "fifth"; cents: number };

/**
 * meantonal requires a fifth that produces a well-defined diatonic scale:
 * between 4/7 and 3/5 of an octave. Outside this range `TuningMap` throws, so
 * every tuning must pass `tuningError` before it reaches the library.
 */
export const FIFTH_MIN = 4800 / 7; // 685.714…
export const FIFTH_MAX = 720;

export const DEFAULT_TUNING: Tuning = { kind: "edo", edo: 31 };

/** Steps of `edo` in its best fifth. The rounding here is the whole algorithm. */
function fifthStepsOf(edo: number): number {
  return Math.round(Math.log2(1.5) * edo);
}

/** The one number a tuning reduces to. */
export function fifthCents(t: Tuning): number {
  return t.kind === "edo" ? (fifthStepsOf(t.edo) * 1200) / t.edo : t.cents;
}

/** EDOs whose best fifth is diatonic, i.e. the ones meantonal will accept. */
export const VALID_EDOS: readonly number[] = (() => {
  const out: number[] = [];
  for (let edo = 5; edo <= 96; edo++) {
    const f = (fifthStepsOf(edo) * 1200) / edo;
    if (f >= FIFTH_MIN && f <= FIFTH_MAX) out.push(edo);
  }
  return out;
})();

/**
 * Why this tuning cannot be used at all, or null if it can.
 *
 * Checked before constructing a `TuningMap`, so a half-typed number in an input
 * field surfaces as a message rather than an exception inside a `$derived`.
 */
export function tuningError(t: Tuning): string | null {
  if (t.kind === "edo") {
    if (!Number.isInteger(t.edo) || t.edo < 5) return "EDO must be a whole number of 5 or more.";
    if (!VALID_EDOS.includes(t.edo)) {
      return `${t.edo}-EDO has no diatonic fifth, so it has no Bosanquet or Wicki-Hayden layout.`;
    }
    return null;
  }
  if (!Number.isFinite(t.cents)) return "Enter a fifth in cents.";
  if (t.cents < FIFTH_MIN || t.cents > FIFTH_MAX) {
    return `A fifth must be between ${FIFTH_MIN.toFixed(3)}¢ and ${FIFTH_MAX}¢ to give a diatonic scale.`;
  }
  return null;
}

/**
 * Why this tuning is usable but strange on this board, or null if it is not.
 *
 * Both families below are legal tunings that meantonal accepts; they are called
 * out because of what they do to a hex layout specifically.
 */
export function tuningWarning(t: Tuning): string | null {
  const fifth = fifthCents(t);
  // In the (w, h) basis a diatonic semitone is (0, 1) and a whole tone is
  // (1, 0), so cents(w,h) = fifth·(2w − 5h) + 1200·(3h − w) gives both. Each
  // degenerate case sits exactly at one end of the legal fifth range: the
  // semitone vanishes at 720¢, and the two coincide at 4800/7 = 685.714¢.
  const semitone = 3600 - 5 * fifth;
  const wholeTone = 2 * fifth - 1200;
  if (Math.abs(semitone) < 1e-9) {
    return "The diatonic semitone vanishes in this tuning, so every key in a Bosanquet row sounds identical.";
  }
  if (Math.abs(semitone - wholeTone) < 1e-9) {
    return "Whole tone and diatonic semitone are the same width in this tuning, so the layout loses its diatonic shape.";
  }
  return null;
}

export function tuningLabel(t: Tuning): string {
  const fifth = fifthCents(t);
  return t.kind === "edo"
    ? `${t.edo}-EDO · fifth ${fifth.toFixed(3)}¢`
    : `fifth ${fifth.toFixed(3)}¢`;
}

/**
 * meantonal's `TuningMap` for a tuning. Throws for a tuning that `tuningError`
 * rejects, so callers validate first.
 *
 * The reference is `TuningMap`'s own default, C4 = 261.6255653 Hz, which is
 * exactly MIDI 60 — the same anchor the firmware's millicents-above-C₋₁ use.
 * A user-facing reference control can be added later; it changes only the Hz
 * readout, never note numbers or bends.
 */
export function tuningMap(t: Tuning): TuningMap {
  return t.kind === "edo" ? TuningMap.fromEDO(t.edo) : new TuningMap(t.cents);
}

const REF_PITCH = SPN.toPitch("C4");
const REF_MIDI = 60;

/** Frequency of a pitch in a tuning, in Hz. */
export function hzAt(t: Tuning, p: Pitch): number {
  return tuningMap(t).toHz(p);
}

// --- MIDI / MPE -------------------------------------------------------------

export const MPE_BEND_SEMITONES = 48;

export interface MidiPitch {
  /** Nearest 12-EDO MIDI note. */
  note: number;
  /** 14-bit pitch bend carrying the remainder. */
  bend: number;
  /** That remainder in cents (negative = flat of the MIDI note). */
  cents: number;
}

/**
 * What a pitch should sound as MPE: a MIDI note plus a bend for the remainder.
 *
 * Deliberately routed through meantonal's own `toCents` rather than through a
 * copy of the firmware's arithmetic. The firmware derives the same answer from
 * int32 millicents (`pitch_for_xy` in Core/Src/main.c), and the ONLY artefact
 * the two implementations share is the 16 bytes of the `P` frame — so when the
 * MIDI monitor finds them agreeing, that means the push arrived and was applied,
 * which is the thing actually worth checking. A JS mirror of the integer path
 * belongs in tests, never here.
 */
export function midiFor(
  p: Pitch,
  t: Tuning,
  bendSemitones = MPE_BEND_SEMITONES,
): MidiPitch {
  const midiFloat = REF_MIDI + tuningMap(t).toCents(REF_PITCH.intervalTo(p)) / 100;
  const note = Math.round(midiFloat);
  const cents = (midiFloat - note) * 100;
  const bend = 8192 + Math.round((cents / (bendSemitones * 100)) * 8192);
  return { note, bend, cents };
}

/** Cents a received bend value represents, for decoding monitored messages. */
export function centsFromBend(bend: number, bendSemitones = MPE_BEND_SEMITONES): number {
  return ((bend - 8192) / 8192) * bendSemitones * 100;
}

/** Cents carried by one step of the 14-bit bend: 0.586¢ at the default ±48. */
export function bendLsbCents(bendSemitones = MPE_BEND_SEMITONES): number {
  return (bendSemitones * 100) / 8192;
}

/**
 * How far a monitored note may sit from its expected pitch and still count as
 * a match.
 *
 * The budget, measured rather than assumed:
 *
 *  - One bend LSB, 0.586¢ at ±48 semitones. The app rounds a float to the
 *    nearest bend step and the firmware rounds int32 millicents to the nearest
 *    bend step; where the true value sits near a midpoint the two can land on
 *    ADJACENT steps, so the error is a whole LSB, not half of one.
 *  - 0.013¢ from quantising the fifth to millicents on the wire, worst case
 *    over a four-board grid at the extremes of the legal fifth range.
 *
 * That is 0.599¢, so 0.05¢ of margin is plenty. Still 24x tighter than one step
 * of 72-EDO, the finest tuning offered, so a wrong key or a stale push is never
 * mistaken for a rounding difference.
 */
export function matchToleranceCents(bendSemitones = MPE_BEND_SEMITONES): number {
  return bendLsbCents(bendSemitones) + 0.05;
}

// --- Pitch mappings ---------------------------------------------------------

/**
 * A procedural pitch mapping: a layout over a tuning, pinned by one note on one
 * key.
 *
 * Note there is no `offset` here, unlike the colour generator. `pitchAt` applies
 * an offset BEFORE the matrix, so `M·(v + o) + a` means an offset and an anchor
 * are not interchangeable — under Wicki-Hayden `M·(0,1)` is `(−2,−1)`, not
 * `(0,1)`. Placement is expressed once, as `anchorNote` on `anchorKey`, which
 * folds the multiply in correctly and is what a player actually wants to say.
 */
export interface PitchGenerator {
  layout: LayoutId;
  tuning: Tuning;
  /** SPN, e.g. "D4". */
  anchorNote: string;
  /** Which grid coordinate that note sits on. */
  anchorKey: [number, number];
  /** MPE bend range the note+bend encoding assumes. */
  bendSemitones: number;
}

/**
 * One deliberately reassigned key, as a meantonal (w, h) vector.
 *
 * Stored as a vector rather than a note name so it is tuning-independent: an
 * override survives a change of EDO or fifth, which a frequency or a step
 * number would not.
 */
export interface PitchOverride {
  w: number;
  h: number;
}

/**
 * A preset's pitch mapping.
 *
 * `generator` is MANDATORY and is never removed — the opposite of
 * `ColorLayer.generator`. Two reasons, both load-bearing:
 *
 *  - The mapping has to answer for coordinates nobody has seen yet, because a
 *    board can be attached at any time. Freezing it over the current grid (the
 *    way `materialise()` does for colours) would leave a later board with no
 *    pitch at all — silent keys rather than a wrong colour.
 *  - The `P` frame carries PARAMETERS, not a table. Delete the generator and
 *    there is nothing left to send.
 *
 * `overrides` are therefore additive exceptions layered on top, and editing the
 * generator must NOT clear them: an override is a decision, not a stale cache.
 */
export interface PitchMap {
  generator: PitchGenerator;
  overrides: Record<string, PitchOverride>;
}

export const DEFAULT_PITCH_GENERATOR: PitchGenerator = {
  layout: "bosanquet",
  tuning: DEFAULT_TUNING,
  anchorNote: "D4",
  anchorKey: [...CENTRE_KEY] as [number, number],
  bendSemitones: MPE_BEND_SEMITONES,
};

export function defaultPitchMap(): PitchMap {
  return { generator: { ...DEFAULT_PITCH_GENERATOR }, overrides: {} };
}

/**
 * The anchor vector a generator implies: the pitch offset that puts
 * `anchorNote` on `anchorKey`.
 *
 * anchor = SPN(note) − M·key. With the defaults this reproduces the firmware's
 * compiled-in constants exactly — Bosanquet D4 on (3,3) gives (23, 7), and
 * Wicki-Hayden gives (29, 13), which are the literals in `LAYOUTS` above.
 */
export function anchorVec(g: PitchGenerator): { w: number; h: number } {
  const layout = LAYOUTS[g.layout];
  let note: Pitch;
  try {
    note = SPN.toPitch(g.anchorNote);
  } catch {
    note = SPN.toPitch(DEFAULT_PITCH_GENERATOR.anchorNote);
  }
  const v = layout.matrix.map(new MapVec(g.anchorKey[0], g.anchorKey[1]));
  return { w: note.w - v.x, h: note.h - v.y };
}

/** Pitch a generator puts on a coordinate. */
export function generatePitchAt(g: PitchGenerator, x: number, y: number): Pitch {
  const v = LAYOUTS[g.layout].matrix.map(new MapVec(x, y));
  const a = anchorVec(g);
  return new Pitch(v.x + a.w, v.y + a.h);
}

/**
 * Pitch of a coordinate under a mapping: an explicit override if there is one,
 * otherwise the generator.
 *
 * Mirrors `colorAt` — generated pitches are evaluated lazily rather than
 * materialised, so attaching another board needs no regeneration.
 */
export function pitchOn(m: PitchMap, x: number, y: number): Pitch {
  const o = m.overrides[cellKey(x, y)];
  if (o !== undefined) return new Pitch(o.w, o.h);
  return generatePitchAt(m.generator, x, y);
}

/** True if this coordinate has been reassigned by hand. */
export function isOverridden(m: PitchMap, x: number, y: number): boolean {
  return m.overrides[cellKey(x, y)] !== undefined;
}

// --- The `P` wire frame -----------------------------------------------------

export const PITCH_FRAME_LEN = 17; // 'P' + 16 payload bytes

/**
 * The tuning parameters, as the firmware wants them.
 *
 * 'P' · int32 fifth millicents · int16 m00, m01, m10, m11 · int16 aw, ah,
 * little-endian. Seventeen bytes replace what would otherwise be a per-key
 * table, and the firmware evaluates them at absolute coordinates so one frame
 * covers every board in the mesh.
 *
 * Note what is NOT computed here: no note numbers, no bends. This function
 * rounds a fifth and copies six matrix/anchor integers, and that is all — which
 * is what keeps the app's expected-pitch path independent of the firmware's.
 */
export function pitchFrame(m: PitchMap): Uint8Array {
  const g = m.generator;
  const matrix = LAYOUTS[g.layout].matrix;
  const a = anchorVec(g);

  const frame = new Uint8Array(PITCH_FRAME_LEN);
  frame[0] = 0x50; // 'P'
  const dv = new DataView(frame.buffer);
  dv.setInt32(1, Math.round(fifthCents(g.tuning) * 1000), true);
  // Signed: Wicki-Hayden's matrix is (1, −2, 0, −1).
  dv.setInt16(5, matrix.m00, true);
  dv.setInt16(7, matrix.m01, true);
  dv.setInt16(9, matrix.m10, true);
  dv.setInt16(11, matrix.m11, true);
  dv.setInt16(13, a.w, true);
  dv.setInt16(15, a.h, true);
  return frame;
}
