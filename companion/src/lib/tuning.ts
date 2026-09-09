// The one module that knows music theory, via meantonal.
//
// meantonal represents a pitch as a 2-vector (w, h) — whole steps and diatonic
// semitones above C₋₁ — where an octave is (5, 2) and a sharp is (1, −1).
// That is exactly the Miso board's axial basis (see layout.ts), so a board
// coordinate is a pitch vector plus a fixed anchor, and `pitch.accidental`
// gives the Bosanquet row directly.

import { Pitch, Map2D, MapVec, WICKI_FROM } from "meantonal";

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
    // Puts D5 on the centre key (3,3) and reproduces the board's boot pattern.
    anchor: { w: 28, h: 9 },
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
    // into a major 6th. Composed this is w = x - 2y, h = -y.
    matrix: WICKI_FROM.compose(new Map2D(1, 1, 0, -1)),
    anchor: { w: 34, h: 15 }, // also D5 on the centre key
    axes: "+x whole tone · fifth up-right · fourth up-left",
  },
};

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

// --- MIDI / MPE ------------------------------------------------------------
// Mirrors the firmware's integer pitch table (midi_build_pitch_table() in
// Core/Src/main.c). Kept here independently so the MIDI monitor can check the
// board's output against meantonal rather than against a copy of itself.

export const EDO = 31;
export const EDO_WHOLE_TONE = 5;
export const EDO_DIATONIC_SEMITONE = 3;
export const MPE_BEND_SEMITONES = 48;

/** Step number of a pitch on the 31-EDO lattice. */
export function edoStep(p: Pitch): number {
  return EDO_WHOLE_TONE * p.w + EDO_DIATONIC_SEMITONE * p.h;
}

export interface MidiPitch {
  /** Nearest 12-EDO MIDI note. */
  note: number;
  /** 14-bit pitch bend carrying the remainder. */
  bend: number;
  /** That remainder in cents (negative = flat of the MIDI note). */
  cents: number;
}

export function midiFor(p: Pitch, bendSemitones = MPE_BEND_SEMITONES): MidiPitch {
  const totalCents = (edoStep(p) * 1200) / EDO;
  const note = Math.round(totalCents / 100);
  const cents = totalCents - note * 100;
  const bend = 8192 + Math.round((cents / (bendSemitones * 100)) * 8192);
  return { note, bend, cents };
}

/** Cents a received bend value represents, for decoding monitored messages. */
export function centsFromBend(bend: number, bendSemitones = MPE_BEND_SEMITONES): number {
  return ((bend - 8192) / 8192) * bendSemitones * 100;
}
