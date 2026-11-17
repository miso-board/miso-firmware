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
//
// WHICH pitch is not the colour layer's to choose. The rows follow the preset's
// pitch generator — its layout and its anchor — so a key is always coloured by
// the accidental of the note it actually plays. There is deliberately no way to
// colour by one layout while sounding another.

import { LED_POS, cellKey } from "./layout";
import {
  LAYOUTS, accidentalLabel, anchorVec, generatePitchAt, type LayoutId, type PitchGenerator,
} from "./tuning";
import { scaleHex, lightenHex, parseHex } from "./ledColor";

/**
 * One slice of the chain of fifths, and the colour of every note inside it.
 *
 * Colouring is a partition of the chain of fifths: … B♭ F C G D A E B F♯ …
 * is cut into consecutive runs, and a key takes the colour of the run its note
 * falls in. The partitions repeat end to end, so a scheme of total width P
 * colours two notes alike whenever they are P fifths apart.
 */
export interface Partition {
  /** How many adjacent fifths it spans. */
  width: number;
  /** Colour at full brightness. */
  color: string;
}

export const PARTITION_WIDTH_MAX = 31;
export const PARTITIONS_MAX = 12;

/** The naturals in chain-of-fifths order; the index is the note's fifths position. */
export const FIFTHS_LETTERS = ["F", "C", "G", "D", "A", "E", "B"] as const;

/**
 * Parameters of a procedurally generated (row-coloured) scheme.
 *
 * Only what the rows LOOK like. Where they fall comes from the pitch generator.
 *
 * Two ways of saying how the chain of fifths is cut up, chosen by `scheme`.
 * Both sets of fields are kept whichever is active, so switching back and forth
 * loses nothing:
 *
 *  - "accidental": equal slices of seven fifths starting on F — the naturals,
 *    the sharps, the flats and so on — one `palette` entry each.
 *  - "partitions": slices of any widths, pinned by naming the natural that
 *    starts one of them. Seven then five is the usual two-row scheme.
 *
 * The first is a special case of the second (see `partitionsFromAccidentals`).
 * It stays a mode of its own because it is the one the firmware can generate.
 */
export interface GeneratorParams {
  scheme: "accidental" | "partitions";
  /** "partitions" scheme: the slices, in rising fifths order. */
  partitions: Partition[];
  /** "partitions" scheme: the natural that starts a partition, as an index into FIFTHS_LETTERS. */
  anchorLetter: number;
  /** "partitions" scheme: which partition that natural starts. */
  anchorPartition: number;
  /** Row colours at full brightness; `brightness` dims them on write. */
  palette: string[];
  /** The accidental that receives palette[0]. */
  startAccidental: number;
  /** Percent of full brightness written to the LEDs. */
  brightness: number;
  /**
   * Landmarks: keys lightened toward white so the hand can find its place.
   * Which one applies depends on the layout (`landmarkFor`), and each keeps its
   * own setting so switching layouts does not lose the other.
   *
   * Wicki-Hayden: lighten C, D and E — the left half of each row.
   */
  lightenCDE: boolean;
  /** Bosanquet and the Harmonic Table: lighten every key of one letter. */
  lightenLetter: boolean;
  /** Which letter, as an index into LANDMARK_LETTERS (C = 0 … B = 6). */
  lightenLetterIndex: number;
  /** How far toward white a landmark key goes: 0 unchanged, 100 white. */
  lightenAmount: number;
}

/** The letters in scale order, which is the order of a pitch's `pc7`. */
export const LANDMARK_LETTERS = ["C", "D", "E", "F", "G", "A", "B"] as const;

/** Which kind of landmark a layout uses. */
export function landmarkFor(layout: LayoutId): "cde" | "letter" {
  return layout === "wicki-hayden" ? "cde" : "letter";
}

/**
 * The letters a generator lightens under a layout, as a bit per `pc7` (C = bit
 * 0 … B = bit 6). Zero means no landmark. This is the form the firmware takes.
 */
export function lightenMask(p: GeneratorParams, layout: LayoutId): number {
  if (landmarkFor(layout) === "cde") return p.lightenCDE ? 0b0000111 : 0;
  if (!p.lightenLetter) return 0;
  const i = Math.round(p.lightenLetterIndex ?? 0);
  return i >= 0 && i < 7 ? 1 << i : 0;
}

/** The lighten amount as the whole percent that is drawn and sent. */
function lightenPercent(p: GeneratorParams): number {
  const v = Number.isFinite(p.lightenAmount) ? p.lightenAmount : LIGHTEN_DEFAULT;
  return Math.min(100, Math.max(0, Math.round(v)));
}

/** The default lighten amount, and the fixed one firmware before 0.10.0 used.
 *  Paler, not brighter, so it stays distinct from the firmware's velocity glow
 *  (which brightens a key's own colour while held). */
export const LIGHTEN_DEFAULT = 35;

/**
 * A preset's LED mapping.
 *
 * The same shape as a `PitchMap`: a generator that is always there, with
 * hand-painted keys layered on top as overrides. A layer with no overrides is
 * procedural; painting any key makes it custom; clearing the overrides returns
 * it to the generated colours, which is why painting never discards the
 * generator.
 */
export interface ColorLayer {
  /** Hand-painted colours by "x,y", over the generated ones. */
  colors: Record<string, string>;
  generator: GeneratorParams;
}

// Chosen so that at 10% brightness these reproduce the board's boot pattern
// byte for byte (250 -> 25, 100 -> 10, 20 -> 2 …).
export const DEFAULT_GENERATOR: GeneratorParams = {
  scheme: "accidental",
  partitions: [],
  anchorLetter: 0,
  anchorPartition: 0,
  palette: ["#fa1414", "#fa6414", "#c8c814", "#32fa32", "#3232fa"],
  startAccidental: -2,
  brightness: 10,
  lightenCDE: false,
  lightenLetter: false,
  lightenLetterIndex: 0,
  lightenAmount: LIGHTEN_DEFAULT,
};

/** A fresh copy of the default generator, safe to mutate. */
export function defaultGenerator(): GeneratorParams {
  return { ...DEFAULT_GENERATOR, palette: [...DEFAULT_GENERATOR.palette], partitions: [] };
}

const mod = (v: number, m: number): number => ((v % m) + m) % m;

/** A note's position on the chain of fifths, with F natural at 0, C at 1, F♯ at 7. */
export function fifthsOf(pitch: { chroma: number }): number {
  return pitch.chroma + 1;
}

/** Name of a chain-of-fifths position, without an octave: 0 → F, 7 → F♯, −1 → B♭. */
export function fifthsName(i: number): string {
  const acc = Math.floor(i / 7);
  return FIFTHS_LETTERS[mod(i, 7)] + (acc === 0 ? "" : accidentalLabel(acc));
}

/**
 * The accidental scheme, said as partitions: one slice of seven per palette
 * row, with F starting the naturals' slice. Colours every key identically.
 */
export function partitionsFromAccidentals(
  p: Pick<GeneratorParams, "palette" | "startAccidental">,
): Pick<GeneratorParams, "partitions" | "anchorLetter" | "anchorPartition"> {
  const n = Math.max(1, p.palette.length);
  return {
    partitions: p.palette.map((color) => ({ width: 7, color })),
    anchorLetter: 0,
    anchorPartition: mod(-p.startAccidental, n),
  };
}

/** True when the generator is using a usable partition scheme. */
function usesPartitions(p: GeneratorParams): boolean {
  return p.scheme === "partitions" && Array.isArray(p.partitions) && p.partitions.length > 0;
}

/**
 * The chain-of-fifths position where each partition starts, for the cycle that
 * contains the anchor. Add or subtract the total width for the other cycles.
 */
export function partitionStarts(p: GeneratorParams): number[] {
  const starts: number[] = [];
  let at = 0;
  for (const part of p.partitions) {
    starts.push(at);
    at += part.width;
  }
  const k = mod(p.anchorPartition ?? 0, Math.max(1, starts.length));
  const shift = (p.anchorLetter ?? 0) - (starts[k] ?? 0);
  return starts.map((s) => s + shift);
}

/** Which partition a chain-of-fifths position falls in. */
export function partitionIndexAt(p: GeneratorParams, fifths: number): number {
  const starts = partitionStarts(p);
  const total = p.partitions.reduce((sum, part) => sum + part.width, 0);
  const pos = mod(fifths - starts[0], total);
  let acc = 0;
  for (let i = 0; i < p.partitions.length; i++) {
    acc += p.partitions[i].width;
    if (pos < acc) return i;
  }
  return 0;
}

/**
 * The palette and first accidental that colour every key exactly as this
 * generator does, or null if no accidental scheme can.
 *
 * This is what decides whether the boards can generate the colours themselves:
 * the `K` frame carries an accidental scheme and nothing else. A partition
 * scheme qualifies only when it happens to be one — every slice seven wide and
 * a slice starting on F.
 */
export function asAccidentalScheme(
  p: GeneratorParams,
): Pick<GeneratorParams, "palette" | "startAccidental"> | null {
  if (!usesPartitions(p)) return { palette: p.palette, startAccidental: p.startAccidental };
  if (p.partitions.some((part) => part.width !== 7)) return null;
  if (mod(partitionStarts(p)[0], 7) !== 0) return null;
  return {
    palette: p.partitions.map((part) => part.color),
    // Partition 0 starts 7·k fifths from F, which is accidental k.
    startAccidental: partitionStarts(p)[0] / 7,
  };
}

/**
 * Colour for one coordinate under a procedural scheme: the colour of the slice
 * of the chain of fifths that the note `pitchGen` puts on that key falls in.
 *
 * The generated pitch, not a pitch override — the board colours itself from
 * parameters, and an override is not one.
 */
export function generateColorAt(
  p: GeneratorParams,
  pitchGen: PitchGenerator,
  x: number,
  y: number,
): string {
  const pitch = generatePitchAt(pitchGen, x, y);
  let hex: string;
  if (usesPartitions(p)) {
    hex = p.partitions[partitionIndexAt(p, fifthsOf(pitch))].color;
  } else {
    const n = Math.max(1, p.palette.length);
    // Rows outside the palette wrap back through it.
    hex = p.palette[mod(pitch.accidental - p.startAccidental, n)] ?? "#000000";
  }
  if ((lightenMask(p, pitchGen.layout) >> pitch.pc7) & 1) hex = lightenHex(hex, lightenPercent(p));
  return scaleHex(hex, p.brightness);
}

/**
 * Colour of a coordinate under a mapping. Generated schemes are evaluated
 * lazily rather than materialised, so attaching another board needs no
 * regeneration — its keys simply resolve.
 */
export function colorAt(m: ColorLayer, pitchGen: PitchGenerator, x: number, y: number): string {
  const explicit = m.colors[cellKey(x, y)];
  if (explicit !== undefined) return explicit;
  return generateColorAt(m.generator, pitchGen, x, y);
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
  pitchGen: PitchGenerator,
  ox: number,
  oy: number,
  into: Uint8Array,
  at: number,
): void {
  LED_POS.forEach(([lx, ly], led) => {
    const [r, g, b] = parseHex(colorAt(m, pitchGen, lx + ox, ly + oy));
    into[at + led * 3] = r;
    into[at + led * 3 + 1] = g;
    into[at + led * 3 + 2] = b;
  });
}

// --- The `K` wire frame ------------------------------------------------------

/** Palette entries the firmware's frame can carry. */
export const COLORGEN_PAL_MAX = 8;
/** Partitions the firmware's frame can carry: six colours and six widths fill the palette bytes. */
export const COLORGEN_PART_MAX = 6;
const COLORGEN_PART_FLAG = 0x80;

export const COLORGEN_FRAME_LEN = 42; // 'G' + 41 payload bytes
export const COLORGEN_LEGACY_FRAME_LEN = 41; // 'K' + 40, for firmware before 0.10.0

/** What the attached firmware's colour generator can do. */
export interface ColorgenCaps {
  /** 0.9.0: the frame can carry a partition scheme. */
  partitions: boolean;
  /** 0.10.0: the `G` frame, with a letter mask and a lighten amount. */
  lighten: boolean;
}

/** Everything current firmware can do. */
export const COLORGEN_CAPS_LATEST: ColorgenCaps = { partitions: true, lighten: true };

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
 * The six integers of geometry are the pitch generator's own — the same matrix
 * and anchor `P` carries — which is what keeps the board's rows on the notes it
 * plays. The frame is still self-contained, so a board needs no tuning to colour
 * itself. `brightness` does NOT fold into the palette, because the landmark tint is applied per key BEFORE scaling and rounds on the way
 * (see `generateColorAt`), so pre-scaling would shift some channels by a count.
 *
 * The same 40 bytes carry one of two forms, told apart by the top bit of the
 * length byte:
 *
 *   accidental rows   int8 start accidental · u8 palette length · … · 8 × RGB
 *   partitions        int8 origin · u8 0x80 | count · … · 6 × RGB · 6 × u8 width
 *
 * A scheme that IS accidental rows always goes out in the first form, however it
 * was authored, because every firmware since 0.8.0 understands it. The second
 * needs 0.9.0, so it is only built when `caps.partitions` says the board has it. An
 * older board rejects a partition frame outright (it reads the marked length as
 * a palette over 8) and keeps the colours it had.
 *
 * The landmark tint decides the command. Firmware from 0.10.0 takes `G`: the
 * same block with the flags byte as a mask of letters to lighten and one more
 * byte on the end for how far. Before that there is only `K`, whose one flag
 * means "C, D and E, by 35%" — so an older board can be sent parameters only
 * when the landmark is off or is exactly that.
 *
 * Returns null when the scheme cannot be expressed as parameters: a palette over
 * eight rows, more than six partitions, or partitions or a landmark that the
 * attached firmware predates. The caller falls back to per-board `L` frames.
 */
export function colorgenFrame(
  g: GeneratorParams,
  pitchGen: PitchGenerator,
  caps: ColorgenCaps = { partitions: false, lighten: false },
): Uint8Array | null {
  const partitionsOk = caps.partitions;
  const letters = lightenMask(g, pitchGen.layout);
  const amount = lightenPercent(g);
  const legacyTint = letters === 0 || (letters === 0b0000111 && amount === LIGHTEN_DEFAULT);
  if (!caps.lighten && !legacyTint) return null;

  let scheme = asAccidentalScheme(g);
  if (scheme && (scheme.startAccidental < -128 || scheme.startAccidental > 127)) scheme = null;
  if (scheme && scheme.palette.length > COLORGEN_PAL_MAX) scheme = null;
  // Not accidental rows the frame can carry; a partition scheme may still fit
  // (which includes accidental rows authored as six or fewer partitions with a
  // first accidental out of an int8's reach — not that anyone will).
  if (!scheme) {
    if (!partitionsOk || !usesPartitions(g) || g.partitions.length > COLORGEN_PART_MAX) return null;
    if (g.partitions.some((part) => part.width < 1 || part.width > PARTITION_WIDTH_MAX)) return null;
  }
  const p = scheme ? { ...g, ...scheme } : g;
  if (scheme && p.palette.length < 1) return null;

  const layout = LAYOUTS[pitchGen.layout];
  const anchor = anchorVec(pitchGen);

  const frame = new Uint8Array(caps.lighten ? COLORGEN_FRAME_LEN : COLORGEN_LEGACY_FRAME_LEN);
  frame[0] = caps.lighten ? 0x47 : 0x4b; // 'G' : 'K'
  const dv = new DataView(frame.buffer);
  // Signed: Wicki-Hayden's matrix is (1, −2, 0, −1).
  dv.setInt16(1, layout.matrix.m00, true);
  dv.setInt16(3, layout.matrix.m01, true);
  dv.setInt16(5, layout.matrix.m10, true);
  dv.setInt16(7, layout.matrix.m11, true);
  dv.setInt16(9, anchor.w, true);
  dv.setInt16(11, anchor.h, true);
  if (caps.lighten) {
    frame[15] = letters;
    frame[41] = amount;
  } else {
    frame[15] = letters ? 0x01 : 0x00;
  }
  frame[16] = Math.round(p.brightness);

  if (!scheme) {
    // Where partition 0 starts, as the residue nearest zero: the pattern repeats
    // every `total` fifths, and the total is at most 6 × 31, so it fits an int8.
    const total = g.partitions.reduce((sum, part) => sum + part.width, 0);
    let origin = mod(partitionStarts(g)[0], total);
    if (origin > total / 2) origin -= total;
    dv.setInt8(13, origin);
    frame[14] = COLORGEN_PART_FLAG | g.partitions.length;
    g.partitions.forEach((part, i) => {
      const [r, gr, b] = parseHex(part.color);
      frame[17 + i * 3] = r;
      frame[17 + i * 3 + 1] = gr;
      frame[17 + i * 3 + 2] = b;
      frame[17 + COLORGEN_PART_MAX * 3 + i] = part.width;
    });
    return frame;
  }

  dv.setInt8(13, p.startAccidental);
  frame[14] = p.palette.length;

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
