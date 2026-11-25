// Scala output: the board sends each key as a plain MIDI note, and a Scala-aware
// synth — given the .scl and .kbm files made here — tunes each note.
//
// Only for EDOs. In an EDO every pitch is a whole number of steps, and every
// layout is linear, so a key's step count is linear in its grid coordinate:
//
//   v = a·x + b·y + c      with C4 at 60
//
// which is all the firmware needs (the 'O' frame). v is a MIDI note number when
// it is in 0..127. Beyond that, keys spill onto further channels in banks of
// 128 — bank 0 on channel 1, then +1 on 2, −1 on 3, +2 on 4 and so on outward —
// each played by its own synth instance with its own .kbm.
//
// The convention is fixed (C4 on note 60 of channel 1), so the files depend
// only on the tuning and concert pitch, never on which boards are attached.

import { SPN } from "meantonal";
import {
  anchorVec, edoOf, fifthCents, hzAt, layoutMatrix, referenceOf, type PitchGenerator,
} from "./tuning";

/** How each key's note is worked out in Scala mode. */
export interface ScalaMap {
  /** The EDO, as played: 24-EDO plays, and maps, as 12. */
  edo: number;
  a: number;
  b: number;
  c: number;
}

const C4 = SPN.toPitch("C4");

/** Steps of an EDO in a whole tone and a diatonic semitone — the (w, h) basis. */
function stepsPerAxis(edo: number): { w: number; h: number } {
  const fifth = Math.round((fifthCents({ kind: "edo", edo }) * edo) / 1200);
  return { w: 2 * fifth - edo, h: 3 * edo - 5 * fifth };
}

/** The Scala mapping for a pitch generator, or null when its tuning is not an EDO. */
export function scalaMap(g: PitchGenerator): ScalaMap | null {
  const edo = edoOf(g.tuning);
  if (edo === null) return null;
  const s = stepsPerAxis(edo);
  const m = layoutMatrix(g);
  const anchor = anchorVec(g);
  return {
    edo,
    a: s.w * m.m00 + s.h * m.m10,
    b: s.w * m.m01 + s.h * m.m11,
    c: 60 + s.w * (anchor.w - C4.w) + s.h * (anchor.h - C4.h),
  };
}

/** A key's note before it is split into banks: EDO steps from C4, plus 60. */
export function scalaValue(map: ScalaMap, x: number, y: number): number {
  return map.a * x + map.b * y + map.c;
}

/** Bank of 128 notes a value falls in. */
export function bankOf(v: number): number {
  return Math.floor(v / 128);
}

/** MIDI channel (1–16) for a bank, or null beyond the sixteen. Mirrors mapped_channel(). */
export function channelOfBank(bank: number): number | null {
  const index = bank > 0 ? 2 * bank - 1 : -2 * bank;
  return index < 16 ? index + 1 : null;
}

/** Bank for a MIDI channel (1–16). The inverse of `channelOfBank`. */
export function bankOfChannel(channel: number): number {
  const index = channel - 1;
  return index % 2 === 1 ? (index + 1) / 2 : -index / 2;
}

/** Channel and note a key sends, or null if out of range. */
export function scalaNote(map: ScalaMap, x: number, y: number): { channel: number; note: number } | null {
  const v = scalaValue(map, x, y);
  const bank = bankOf(v);
  const channel = channelOfBank(bank);
  return channel === null ? null : { channel, note: v - 128 * bank };
}

/** The banks a set of keys uses, in channel order. */
export function banksFor(map: ScalaMap, keys: readonly { x: number; y: number }[]): number[] {
  const banks = new Set(keys.map((k) => bankOf(scalaValue(map, k.x, k.y))));
  return [...banks]
    .filter((b) => channelOfBank(b) !== null)
    .sort((p, q) => (channelOfBank(p) ?? 0) - (channelOfBank(q) ?? 0));
}

/** The frequency of a value under a pitch generator, with its concert pitch. */
function hzOfValue(g: PitchGenerator, edo: number, v: number): number {
  const c4 = hzAt(g.tuning, C4, referenceOf(g));
  return c4 * 2 ** ((v - 60) / edo);
}

/** A file name for this tuning, e.g. "Miso 31-EDO". */
export function scalaName(map: ScalaMap): string {
  return `Miso ${map.edo}-EDO`;
}

/**
 * The .scl file: the EDO's steps in cents, the octave as 2/1.
 *
 * Cents values carry a decimal point, which is how the format tells them from
 * ratios; 1/1 is implicit and not written.
 */
export function sclFile(map: ScalaMap): string {
  const lines = [
    `! ${scalaName(map)}.scl`,
    "!",
    `${map.edo} equal divisions of the octave, from the Miso companion`,
    ` ${map.edo}`,
    "!",
  ];
  for (let k = 1; k < map.edo; k++) lines.push(` ${((1200 * k) / map.edo).toFixed(6)}`);
  lines.push(" 2/1");
  return lines.join("\n") + "\n";
}

/**
 * The .kbm file for one bank: a linear mapping of that channel's 128 notes onto
 * the scale, with a frequency that puts them where the board means them.
 *
 * The middle note (where scale degree 0, a C, sits) and the reference note are
 * kept within 0..127: the format does not say they may lie outside, so this does
 * not ask that of any synth. The reference is the middle note itself, at the
 * frequency the tuning and concert pitch give it. "Formal octave" 0 means the
 * scale's last pitch, 2/1.
 */
export function kbmFile(map: ScalaMap, g: PitchGenerator, bank: number): string {
  // Notes where a C falls are those with (128·bank + n − 60) a multiple of the
  // EDO; take the one nearest the middle of the channel.
  let middle = (((60 - 128 * bank) % map.edo) + map.edo) % map.edo;
  while (middle + map.edo <= 127 && Math.abs(middle + map.edo - 60) < Math.abs(middle - 60)) {
    middle += map.edo;
  }
  const hz = hzOfValue(g, map.edo, 128 * bank + middle);
  const channel = channelOfBank(bank);
  return [
    `! ${scalaName(map)} channel ${channel}.kbm`,
    `! Keys on MIDI channel ${channel} of the Miso, from the Miso companion.`,
    "! Size of map (0: linear):",
    "0",
    "! First MIDI note number to retune:",
    "0",
    "! Last MIDI note number to retune:",
    "127",
    "! Middle note (where the first entry of the mapping is mapped to):",
    String(middle),
    "! Reference note for which frequency is given:",
    String(middle),
    "! Frequency to tune the above note to:",
    // Twelve significant figures: a bank far from the middle can put its middle
    // note well below 1 Hz, where six decimal places would be a rounding error.
    (() => { const t = hz.toPrecision(12); return t.includes("e") ? hz.toFixed(15) : t; })(),
    "! Scale degree to consider as formal octave (0: the scale's last pitch):",
    "0",
    "! Mapping (none: linear)",
  ].join("\n") + "\n";
}

/** The frequency a channel's note sounds at, by the files — for checking them. */
export function kbmHz(map: ScalaMap, g: PitchGenerator, channel: number, note: number): number {
  return hzOfValue(g, map.edo, 128 * bankOfChannel(channel) + note);
}

// --- the 'O' wire frame -----------------------------------------------------

export const OUTPUT_FRAME_LEN = 8; // 'O' + 7 payload bytes

/** 'O' · u8 mode (0 MPE, 1 mapped) · int16 a, b, c, little-endian. MPE when `map` is null. */
export function outputFrame(map: ScalaMap | null): Uint8Array {
  const frame = new Uint8Array(OUTPUT_FRAME_LEN);
  frame[0] = 0x4f; // 'O'
  if (!map) return frame;
  const dv = new DataView(frame.buffer);
  frame[1] = 1;
  dv.setInt16(2, map.a, true);
  dv.setInt16(4, map.b, true);
  dv.setInt16(6, map.c, true);
  return frame;
}
