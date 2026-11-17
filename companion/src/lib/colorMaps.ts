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
  accidentalLabel, anchorVec, edoOf, generatePitchAt, layoutMatrix, type LayoutId, type PitchGenerator,
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
 *  - "piano": seven then five, repeating every twelve fifths — the naturals
 *    one colour and the sharps and flats another, as on a piano. Optionally the
 *    middle key of each run of three gets a colour of its own.
 *  - "partitions": slices of any widths, pinned by naming the natural that
 *    starts one of them.
 *
 * The first two are special cases of the third (see `partitionsFromAccidentals`
 * and `pianoPartitions`). They stay modes of their own because they are what
 * people mean, and need far fewer decisions.
 */
export interface GeneratorParams {
  /**
   * Take the colours from the tuning (see autoColors.ts) rather than from the
   * user. The generated colours are still WRITTEN into `palette`, `piano` and
   * `partitions` — by the preset store, whenever the tuning or the rows change —
   * so everything that draws or sends a scheme reads plain stored colours and
   * needs to know nothing about this. Editing a colour by hand turns it off.
   */
  autoColors: boolean;
  scheme: "accidental" | "piano" | "partitions";
  /**
   * "accidental" scheme, in an EDO: wrap the chain of fifths at the EDO's size,
   * so each of its notes is coloured once rather than the rows running on into
   * ever more remote accidentals that only repeat notes already coloured. See
   * `edoWrap`. Ignored in a tuning that is not an EDO.
   */
  wrapEdo: boolean;
  /**
   * "accidental" scheme: the rows are mirrored about the naturals. `palette` is
   * then centre-first — ♮, ♯, ♯♯, … — and the flats take the colours of the
   * sharps they mirror; `startAccidental` is not used. See `expandSymmetric`.
   */
  symmetricRows: boolean;
  /**
   * "partitions" scheme: the partitions are mirrored about the first. The list
   * is then centre-first, the partitions above it in rising fifths; the same
   * ones are laid out below it in reverse.
   */
  symmetricPartitions: boolean;
  /** "piano" scheme. */
  piano: PianoColors;
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
  /** How far toward white those go: 0 unchanged, 100 white. */
  lightenCDEAmount: number;
  /** Bosanquet: lighten every key of one letter. */
  lightenLetter: boolean;
  /** Which letter, as an index into LANDMARK_LETTERS (C = 0 … B = 6). */
  lightenLetterIndex: number;
  /** How far toward white that letter goes. Separate from Wicki-Hayden's amount. */
  lightenLetterAmount: number;
}

/** The colours of the "piano" scheme, at full brightness. */
export interface PianoColors {
  /** The seven naturals. */
  natural: string;
  /** The five sharps, and the five flats. */
  accidental: string;
  /** Give the middle key of each run of three its own colour. */
  centres: boolean;
  /** D — the middle of C D E, and of the naturals as a whole. */
  centreNatural: string;
  /** G♯ / A♭ — the middle of the three sharps or flats that sit together. */
  centreAccidental: string;
}

/** The letters in scale order, which is the order of a pitch's `pc7`. */
export const LANDMARK_LETTERS = ["C", "D", "E", "F", "G", "A", "B"] as const;

/** Which kind of landmark a layout uses, if any. */
export function landmarkFor(layout: LayoutId): "cde" | "letter" | "none" {
  return layout === "wicki-hayden" ? "cde" : layout === "bosanquet" ? "letter" : "none";
}

/**
 * The letters a generator lightens under a layout, as a bit per `pc7` (C = bit
 * 0 … B = bit 6). Zero means no landmark. This is the form the firmware takes.
 */
export function lightenMask(p: GeneratorParams, layout: LayoutId): number {
  const kind = landmarkFor(layout);
  if (kind === "none") return 0;
  if (kind === "cde") return p.lightenCDE ? 0b0000111 : 0;
  if (!p.lightenLetter) return 0;
  const i = Math.round(p.lightenLetterIndex ?? 0);
  return i >= 0 && i < 7 ? 1 << i : 0;
}

/**
 * The lighten amount under a layout, as the whole percent that is drawn and
 * sent. Each landmark has its own, so tuning one layout's does not move another's.
 */
export function lightenPercent(p: GeneratorParams, layout: LayoutId): number {
  const raw = landmarkFor(layout) === "cde" ? p.lightenCDEAmount : p.lightenLetterAmount;
  const v = Number.isFinite(raw) ? raw : LIGHTEN_DEFAULT;
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
  // Off by default: these are the colours a board shows with no app attached.
  autoColors: false,
  scheme: "accidental",
  wrapEdo: false,
  symmetricRows: false,
  symmetricPartitions: false,
  partitions: [],
  anchorLetter: 0,
  anchorPartition: 0,
  palette: ["#fa1414", "#fa6414", "#c8c814", "#32fa32", "#3232fa"],
  startAccidental: -2,
  brightness: 10,
  lightenCDE: false,
  lightenCDEAmount: LIGHTEN_DEFAULT,
  lightenLetter: false,
  lightenLetterIndex: 0,
  lightenLetterAmount: LIGHTEN_DEFAULT,
  // The default palette's naturals and sharps, with its flats and double sharps
  // for the two centre keys.
  piano: {
    natural: "#c8c814",
    accidental: "#32fa32",
    centres: false,
    centreNatural: "#fa6414",
    centreAccidental: "#3232fa",
  },
};

/** A fresh copy of the default generator, safe to mutate. */
export function defaultGenerator(): GeneratorParams {
  return {
    ...DEFAULT_GENERATOR,
    palette: [...DEFAULT_GENERATOR.palette],
    partitions: [],
    piano: { ...DEFAULT_GENERATOR.piano },
  };
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

/**
 * The piano scheme, said as partitions starting on F.
 *
 * Seven naturals then five sharps, repeating every twelve fifths — so the five
 * flats take the sharps' colour, and E♯ wraps round to the naturals'.
 *
 * With the centre keys on, each run of three alike gets its middle picked out:
 * D among the naturals (F C G · D · A E B along the fifths, and the middle of
 * C D E on the keys) and G♯ among the sharps (F♯ C♯ · G♯ · D♯ A♯). That is six
 * partitions, which is exactly what the firmware's frame carries.
 */
export function pianoPartitions(c: PianoColors): Partition[] {
  if (!c.centres) {
    return [
      { width: 7, color: c.natural },
      { width: 5, color: c.accidental },
    ];
  }
  return [
    { width: 3, color: c.natural }, // F C G
    { width: 1, color: c.centreNatural }, // D
    { width: 3, color: c.natural }, // A E B
    { width: 2, color: c.accidental }, // F♯ C♯
    { width: 1, color: c.centreAccidental }, // G♯
    { width: 2, color: c.accidental }, // D♯ A♯
  ];
}

/**
 * Accidental rows wrapped to an EDO of `period` notes: how wide each run along
 * the chain of fifths is, which accidental each stands for, and which run the
 * naturals are.
 *
 * The naturals sit in the middle with whole accidentals of seven either side —
 * as many as fit, always the same number each side — and the remainder is
 * split between a partial row at each end, the sharp side taking the odd one:
 *
 *   12-EDO   2 7 3            E♭ B♭ · F…B · F♯ C♯ G♯
 *   19-EDO   6 7 6
 *   31-EDO   5 7 7 7 5
 *
 * Both ends are at most seven: the remainder is under fourteen, so the flat
 * end is at most six and the sharp end at most seven (a whole row, only when
 * the remainder is 13 — 90-EDO). An EDO smaller than seven is a single run
 * centred on D.
 *
 * The flat end is never the larger, because the sharp end takes the odd one.
 */
export function edoWrap(period: number): {
  widths: number[];
  accidentals: number[];
  naturals: number;
  /** Where the window starts on the chain of fifths (F = 0): what the board is sent. */
  origin: number;
} {
  if (period <= 7) {
    return { widths: [period], accidentals: [0], naturals: 0, origin: 3 - Math.floor((period - 1) / 2) };
  }
  let k = 0; // whole accidentals each side of the naturals
  while (period - 7 * (2 * k + 3) >= 0) k++;
  const rest = period - 7 * (2 * k + 1);
  const flat = Math.floor(rest / 2);
  const sharp = rest - flat;
  const widths: number[] = [];
  const accidentals: number[] = [];
  if (flat > 0) {
    widths.push(flat);
    accidentals.push(-(k + 1));
  }
  for (let a = -k; a <= k; a++) {
    widths.push(7);
    accidentals.push(a);
  }
  if (sharp > 0) {
    widths.push(sharp);
    accidentals.push(k + 1);
  }
  return { widths, accidentals, naturals: (flat > 0 ? 1 : 0) + k, origin: -(flat + 7 * k) };
}

/** The EDO a generator wraps its colours to under a tuning, or null if it does not. */
export function wrapPeriod(g: GeneratorParams, pitchGen?: Pick<PitchGenerator, "tuning">): number | null {
  if (g.scheme !== "accidental" || !g.wrapEdo || !pitchGen) return null;
  return edoOf(pitchGen.tuning);
}

/** A centre-first list mirrored about its first entry: [c₂, c₁, c₀, c₁, c₂]. */
function mirror<T>(centreFirst: readonly T[]): T[] {
  const above = centreFirst.slice(1);
  return [...[...above].reverse(), ...centreFirst];
}

/**
 * A generator with any symmetric rows or partitions written out in full, as
 * the ordinary lists they amount to. Everything that draws or sends a scheme
 * goes through this first, so nothing else needs to know symmetry exists.
 *
 * The full list then repeats as any other does: rows cycle through the palette,
 * partitions repeat end to end.
 */
export function expandSymmetric(p: GeneratorParams): GeneratorParams {
  let out = p;
  if (p.symmetricRows && p.palette.length > 0) {
    out = {
      ...out,
      symmetricRows: false,
      palette: mirror(p.palette),
      startAccidental: -(p.palette.length - 1),
    };
  }
  if (p.symmetricPartitions && p.partitions.length > 0) {
    const anchor = Math.min(Math.max(0, p.anchorPartition ?? 0), p.partitions.length - 1);
    out = {
      ...out,
      symmetricPartitions: false,
      partitions: mirror(p.partitions),
      // The anchor names a partition in the centre-first list; in the full one
      // that is its copy on the upper side.
      anchorPartition: p.partitions.length - 1 + anchor,
    };
  }
  return out;
}

/**
 * Accidental rows made symmetric: the naturals' row and every row above it,
 * as they are now. The flat side is then taken from these.
 */
export function symmetricFromRows(
  p: Pick<GeneratorParams, "palette" | "startAccidental">,
): Pick<GeneratorParams, "palette" | "startAccidental"> {
  const n = Math.max(1, p.palette.length);
  const highest = Math.max(0, p.startAccidental + p.palette.length - 1);
  return {
    palette: Array.from({ length: highest + 1 }, (_, a) => p.palette[mod(a - p.startAccidental, n)]),
    startAccidental: 0,
  };
}

/**
 * Partitions made symmetric: the anchored one becomes the centre, with the
 * ones after it above. Its natural still starts it, so nothing moves.
 */
export function symmetricFromPartitions(
  p: Pick<GeneratorParams, "partitions" | "anchorPartition">,
): Pick<GeneratorParams, "partitions" | "anchorPartition"> {
  const k = Math.min(Math.max(0, p.anchorPartition ?? 0), Math.max(0, p.partitions.length - 1));
  return { partitions: p.partitions.slice(k), anchorPartition: 0 };
}

/**
 * A generator with the piano scheme, or wrapped accidental rows, spelled out as
 * the partitions they are — and anything symmetric written out in full.
 */
function resolveScheme(generator: GeneratorParams, pitchGen?: Pick<PitchGenerator, "tuning">): GeneratorParams {
  const p = expandSymmetric(generator);
  if (p.scheme === "piano") {
    return {
      ...p,
      scheme: "partitions",
      partitions: pianoPartitions(p.piano ?? DEFAULT_GENERATOR.piano),
      anchorLetter: 0,
      anchorPartition: 0,
    };
  }
  const period = wrapPeriod(p, pitchGen);
  if (period === null) return p;
  // Each run takes its accidental's row of the palette, wrapping through it as
  // unwrapped rows do.
  const { widths, accidentals, naturals } = edoWrap(period);
  const n = Math.max(1, p.palette.length);
  return {
    ...p,
    scheme: "partitions",
    partitions: widths.map((width, i) => ({
      width,
      color: p.palette[mod(accidentals[i] - p.startAccidental, n)] ?? "#000000",
    })),
    // The naturals' run starts on F, or for an EDO under seven, where the one
    // run is centred on D, on the natural it starts with.
    anchorLetter: period < 7 ? edoWrap(period).origin : 0,
    anchorPartition: naturals,
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
  generator: GeneratorParams,
  pitchGen?: Pick<PitchGenerator, "tuning">,
): Pick<GeneratorParams, "palette" | "startAccidental"> | null {
  const p = resolveScheme(generator, pitchGen);
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
  generator: GeneratorParams,
  pitchGen: PitchGenerator,
  x: number,
  y: number,
): string {
  const p = resolveScheme(generator, pitchGen);
  const pitch = generatePitchAt(pitchGen, x, y);
  let hex: string;
  if (usesPartitions(p)) {
    hex = p.partitions[partitionIndexAt(p, fifthsOf(pitch))].color;
  } else {
    const n = Math.max(1, p.palette.length);
    // Rows outside the palette wrap back through it.
    hex = p.palette[mod(pitch.accidental - p.startAccidental, n)] ?? "#000000";
  }
  if ((lightenMask(p, pitchGen.layout) >> pitch.pc7) & 1) {
    hex = lightenHex(hex, lightenPercent(p, pitchGen.layout));
  }
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

export const COLORGEN_FRAME_LEN = 44; // 'J' + 43 payload bytes
export const COLORGEN_G_FRAME_LEN = 42; // 'G' + 41, for firmware 0.10.0
export const COLORGEN_LEGACY_FRAME_LEN = 41; // 'K' + 40, for firmware before 0.10.0

/** What the attached firmware's colour generator can do. */
export interface ColorgenCaps {
  /** 0.9.0: the frame can carry a partition scheme. */
  partitions: boolean;
  /** 0.10.0: the `G` frame, with a letter mask and a lighten amount. */
  lighten: boolean;
  /** 0.11.0: the `J` frame, which can wrap accidental rows to an EDO. Implies `lighten`. */
  wrap?: boolean;
}

/** Everything current firmware can do. */
export const COLORGEN_CAPS_LATEST: ColorgenCaps = { partitions: true, lighten: true, wrap: true };

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
 * Accidental rows wrapped to an EDO go as `J` — the `G` block with two more
 * bytes, the EDO and where its window starts — when the firmware has it (from
 * 0.11.0), so any EDO is generated on the board. Before that they go as the
 * partitions they are, which fits up to 33-EDO.
 *
 * Returns null when the scheme cannot be expressed as parameters: a palette over
 * eight rows, more than six partitions, or partitions or a landmark that the
 * attached firmware predates. The caller falls back to per-board `L` frames.
 */
export function colorgenFrame(
  generator: GeneratorParams,
  pitchGen: PitchGenerator,
  caps: ColorgenCaps = { partitions: false, lighten: false },
): Uint8Array | null {
  // The board wraps rows itself if it can; otherwise spell them out as runs.
  const period = caps.wrap ? wrapPeriod(generator, pitchGen) : null;
  const wrap = period !== null ? { period, origin: edoWrap(period).origin } : null;
  const g = wrap ? expandSymmetric(generator) : resolveScheme(generator, pitchGen);
  const partitionsOk = caps.partitions;
  const letters = lightenMask(g, pitchGen.layout);
  const amount = lightenPercent(g, pitchGen.layout);
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

  // The pitch generator's own matrix, rotation included.
  const layout = { matrix: layoutMatrix(pitchGen) };
  const anchor = anchorVec(pitchGen);

  const frame = new Uint8Array(
    caps.wrap ? COLORGEN_FRAME_LEN : caps.lighten ? COLORGEN_G_FRAME_LEN : COLORGEN_LEGACY_FRAME_LEN,
  );
  frame[0] = caps.wrap ? 0x4a : caps.lighten ? 0x47 : 0x4b; // 'J' : 'G' : 'K'
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
  if (wrap) {
    frame[42] = wrap.period;
    dv.setInt8(43, wrap.origin);
  }

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
