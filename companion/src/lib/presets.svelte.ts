// Presets: reactive state, localStorage persistence, and the wire frames that
// push one to the boards.
//
// A PRESET is how the instrument is set up: an LED mapping and a pitch mapping,
// with room for further preset-scoped settings. The two halves work the same way
// — keyed by absolute grid coordinate, a generator that is always present with
// per-key overrides layered on top, evaluated lazily so a board attached later
// simply resolves.
//
// The colour generator has no geometry of its own: its rows follow the pitch
// generator, so keys are coloured by the accidental of the note they play.
// Painting a key adds an override and makes the colours "custom";
// `resetColors` drops the overrides and returns to the generated scheme. The
// generator is never removed by painting, which is what makes that reset
// possible.
//
// See `PitchMap` in tuning.ts for why a pitch generator must be permanent.

import { NUM_KEYS } from "./layout";
import {
  DEFAULT_PITCH_GENERATOR,
  LAYOUTS,
  fifthCents,
  tuningError,
  REFERENCE_HZ_MAX,
  REFERENCE_HZ_MIN,
  referenceOffsetMilli,
  defaultPitchMap,
  pitchFrame,
  type PitchGenerator,
  type PitchMap,
} from "./tuning";
import {
  COLORGEN_CAPS_LATEST,
  FIFTHS_LETTERS,
  LIGHTEN_DEFAULT,
  PARTITIONS_MAX,
  PARTITION_WIDTH_MAX,
  colorgenFrame,
  coloursFromLedArray,
  edoWrap,
  wrapPeriod,
  defaultGenerator,
  rgbForBoard,
  type ColorLayer,
  type ColorgenCaps,
  type GeneratorParams,
} from "./colorMaps";
import { autoColorsFor, rowsForWrap } from "./autoColors";
import { sendBytes } from "./serial";
import { ui, toast, fwAtLeast } from "./ui.svelte";
import { mesh, gridCells, cellKey } from "./mesh.svelte";

/**
 * One way of setting the instrument up.
 *
 * `settings` is a deliberate hold for preset-scoped options that do not exist
 * yet; unknown keys survive a load/save round trip rather than being dropped.
 */
export interface Preset {
  name: string;
  colors: ColorLayer;
  pitch: PitchMap;
  settings?: Record<string, unknown>;
}

/** Firmware that understands the `P` tuning frame. */
export const PITCH_PUSH_FW = [0, 7, 0] as const;

/** Firmware with the `Q` frame: the tuning plus a concert pitch. */
export const PITCH_REFERENCE_FW = [0, 10, 0] as const;

/** Firmware that understands the `K` colour-generator frame. */
export const COLORGEN_PUSH_FW = [0, 8, 0] as const;

/** Firmware whose `K` frame can carry a partition scheme, not just accidental rows. */
export const COLORGEN_PARTITIONS_FW = [0, 9, 0] as const;

/** Firmware with the `G` frame: any letters as the landmark, lightened by any amount. */
export const COLORGEN_LIGHTEN_FW = [0, 10, 0] as const;

/** Firmware with the `J` frame: accidental rows wrapped to an EDO, generated on the board. */
export const COLORGEN_WRAP_FW = [0, 11, 0] as const;

/**
 * The out-of-the-box preset.
 *
 * Its colours are the GENERATOR. This used to be a frozen 31-entry layer
 * mirroring the firmware's boot pattern by LED index; the two agree byte for byte
 * on a board at the origin — `DEFAULT_GENERATOR` was chosen so they would, and
 * `check-firmware-colors.py` holds both sides to it — but a table keyed by the
 * origin board's coordinates paints the origin's rows onto every board in a tiled
 * grid rather than each board's own. The generator is also what lets the whole
 * scheme go out as one `K` frame and keep working after this app is closed.
 */
function defaultPreset(): Preset {
  return {
    name: "Bosanquet",
    colors: { colors: {}, generator: defaultGenerator() },
    pitch: defaultPitchMap(),
  };
}

export const presetState = $state({
  /** The presets as they are being edited. */
  presets: [defaultPreset()] as Preset[],
  /**
   * Each preset as it was last SAVED, as JSON, index for index with `presets`.
   *
   * Editing a preset changes only the working copy above; nothing reaches
   * storage until `savePreset`. Keeping the saved form alongside is what lets
   * the list be written out (when a preset is added, removed or selected)
   * without sweeping another preset's unsaved edits along with it, and what
   * `isDirty` compares against.
   */
  saved: [JSON.stringify(defaultPreset())] as string[],
  active: 0,
  /**
   * The pitch frame the board is believed to be running, as hex.
   *
   * The board's tuning is now whatever we last pushed, so an edited-but-unpushed
   * preset makes every MIDI monitor row disagree. Tracking this lets the UI say
   * so instead of looking broken.
   */
  pushedPitch: null as string | null,
  /** The colour-generator frame the board is believed to be running, as hex. */
  pushedColorgen: null as string | null,
  /**
   * What the boards' colours were last brought up to date with — everything a
   * colour push depends on, as one string (see `colorsSignature`). Null until
   * the first push of a connection.
   */
  pushedColors: null as string | null,
});

export function activePreset(): Preset {
  return presetState.presets[presetState.active] ?? presetState.presets[0];
}

/** Convenience for the many call sites that only want the colour layer. */
export function activeColors(): ColorLayer {
  return activePreset().colors;
}

export function activePitch(): PitchMap {
  return activePreset().pitch;
}

// --- storage ----------------------------------------------------------------

const STORAGE_KEY = "miso-presets-v1";
const STORAGE_KEY_COLORS_V2 = "miso-color-maps-v2";
const STORAGE_KEY_COLORS_V1 = "miso-color-maps-v1";

const asColours = (v: unknown): Record<string, string> | null =>
  Array.isArray(v)
    ? coloursFromLedArray(v as string[])
    : v && typeof v === "object"
      ? Object.fromEntries(Object.entries(v).map(([k, val]) => [k, String(val)]))
      : null;

/**
 * Read one stored entry, in any format this app has ever written.
 *
 * Three shapes arrive here: a v1 colour map (31-entry LED-indexed array), a v2
 * colour map (colours keyed by coordinate, optional generator), and a preset.
 * The first two get the default pitch mapping — 31-EDO Bosanquet with D4 on the
 * centre key — which is exactly what the firmware plays today, so migrating
 * changes nothing about how the instrument sounds.
 */
function normalise(raw: any): Preset | null {
  if (!raw || typeof raw !== "object") return null;

  const colours = asColours(raw.colors?.colors ?? raw.colors);
  if (!colours) return null;

  const colors: ColorLayer = {
    colors: colours,
    generator: normaliseColorGenerator(raw.colors?.generator ?? raw.generator),
  };

  const pitch = normalisePitch(raw.pitch);

  return {
    name: String(raw.name ?? "Preset"),
    colors,
    pitch,
    ...(raw.settings && typeof raw.settings === "object" ? { settings: raw.settings } : {}),
  };
}

/**
 * A stored colour generator, in the current shape.
 *
 * Older documents gave the colour generator its own `layout` and `offset`; both
 * are dropped, since the rows now follow the pitch mapping. A document with no
 * generator at all was hand-painted: it gets the default one underneath, with
 * its painted keys kept as overrides, so "reset" has something to return to.
 */
function normaliseColorGenerator(raw: any): GeneratorParams {
  const d = defaultGenerator();
  if (!raw || typeof raw !== "object") return d;
  const palette = Array.isArray(raw.palette)
    ? raw.palette.filter((c: unknown) => typeof c === "string" && /^#[0-9a-f]{6}$/i.test(c))
    : [];
  const isHex = (c: unknown): c is string => typeof c === "string" && /^#[0-9a-f]{6}$/i.test(c);
  const percent = (v: unknown): number =>
    Number.isFinite(v) ? Math.min(100, Math.max(0, Math.round(Number(v)))) : LIGHTEN_DEFAULT;
  const partitions = (Array.isArray(raw.partitions) ? raw.partitions : [])
    .filter((p: any) => p && isHex(p.color) && Number.isFinite(p.width))
    .slice(0, PARTITIONS_MAX)
    .map((p: any) => ({
      width: Math.min(PARTITION_WIDTH_MAX, Math.max(1, Math.round(p.width))),
      color: p.color as string,
    }));
  const letter = Math.round(Number(raw.anchorLetter));
  const anchorPartition = Math.round(Number(raw.anchorPartition));
  return {
    autoColors: raw.autoColors === true,
    wrapEdo: raw.wrapEdo === true,
    symmetricRows: raw.symmetricRows === true,
    symmetricPartitions: raw.symmetricPartitions === true,
    // A partition scheme with no partitions has nothing to draw with.
    scheme:
      raw.scheme === "partitions" && partitions.length
        ? "partitions"
        : raw.scheme === "piano"
          ? "piano"
          : "accidental",
    piano: {
      natural: isHex(raw.piano?.natural) ? raw.piano.natural : d.piano.natural,
      accidental: isHex(raw.piano?.accidental) ? raw.piano.accidental : d.piano.accidental,
      centres: raw.piano?.centres === true,
      centreNatural: isHex(raw.piano?.centreNatural) ? raw.piano.centreNatural : d.piano.centreNatural,
      centreAccidental: isHex(raw.piano?.centreAccidental)
        ? raw.piano.centreAccidental
        : d.piano.centreAccidental,
    },
    partitions,
    anchorLetter: letter >= 0 && letter < FIFTHS_LETTERS.length ? letter : 0,
    anchorPartition:
      anchorPartition >= 0 && anchorPartition < partitions.length ? anchorPartition : 0,
    palette: palette.length ? palette : d.palette,
    startAccidental: Number.isFinite(raw.startAccidental) ? Number(raw.startAccidental) : d.startAccidental,
    brightness: Number.isFinite(raw.brightness) ? Number(raw.brightness) : d.brightness,
    lightenCDE: raw.lightenCDE === true,
    lightenLetter: raw.lightenLetter === true,
    lightenLetterIndex:
      Number.isInteger(raw.lightenLetterIndex) && raw.lightenLetterIndex >= 0 && raw.lightenLetterIndex < 7
        ? raw.lightenLetterIndex
        : 0,
    // One amount per landmark. `lightenAmount` is the single shared one that
    // presets saved before they were separated carry; it seeds both.
    lightenCDEAmount: percent(raw.lightenCDEAmount ?? raw.lightenAmount),
    lightenLetterAmount: percent(raw.lightenLetterAmount ?? raw.lightenAmount),
  };
}

function normalisePitch(raw: any): PitchMap {
  const m = defaultPitchMap();
  if (!raw || typeof raw !== "object") return m;

  const g = raw.generator;
  if (g && typeof g === "object") {
    const layout = g.layout in LAYOUTS ? g.layout : DEFAULT_PITCH_GENERATOR.layout;
    const tuning =
      g.tuning?.kind === "fifth" && Number.isFinite(g.tuning.cents)
        ? { kind: "fifth" as const, cents: Number(g.tuning.cents) }
        : g.tuning?.kind === "edo" && Number.isFinite(g.tuning.edo)
          ? { kind: "edo" as const, edo: Number(g.tuning.edo) }
          : DEFAULT_PITCH_GENERATOR.tuning;
    const key = Array.isArray(g.anchorKey) ? g.anchorKey.map(Number) : null;
    m.generator = {
      layout,
      tuning,
      anchorNote: typeof g.anchorNote === "string" ? g.anchorNote : DEFAULT_PITCH_GENERATOR.anchorNote,
      anchorKey:
        key && key.length === 2 && key.every(Number.isFinite)
          ? ([key[0], key[1]] as [number, number])
          : ([...DEFAULT_PITCH_GENERATOR.anchorKey] as [number, number]),
      bendSemitones: Number.isFinite(g.bendSemitones)
        ? Number(g.bendSemitones)
        : DEFAULT_PITCH_GENERATOR.bendSemitones,
      rotation: Number.isInteger(g.rotation) ? (((g.rotation % 6) + 6) % 6) : 0,
      // Absent from presets saved before concert pitch existed, which played
      // with every C on a 12-TET C — exactly what the default says.
      referenceNote:
        typeof g.referenceNote === "string" ? g.referenceNote : DEFAULT_PITCH_GENERATOR.referenceNote,
      referenceHz:
        Number.isFinite(g.referenceHz) && g.referenceHz >= REFERENCE_HZ_MIN && g.referenceHz <= REFERENCE_HZ_MAX
          ? Number(g.referenceHz)
          : DEFAULT_PITCH_GENERATOR.referenceHz,
    };
  }

  if (raw.overrides && typeof raw.overrides === "object") {
    for (const [k, v] of Object.entries(raw.overrides as Record<string, any>)) {
      if (v && Number.isFinite(v.w) && Number.isFinite(v.h)) {
        m.overrides[k] = { w: Number(v.w), h: Number(v.h) };
      }
    }
  }
  return m;
}

/**
 * Load from storage, migrating the older colour-only formats forward.
 *
 * The superseded keys are left in place as a backup rather than deleted, which
 * is how the v1 -> v2 colour migration behaved too.
 */
export function load(): void {
  try {
    const raw =
      localStorage.getItem(STORAGE_KEY) ??
      localStorage.getItem(STORAGE_KEY_COLORS_V2) ??
      localStorage.getItem(STORAGE_KEY_COLORS_V1);
    if (!raw) return;
    const data = JSON.parse(raw);
    const list = Array.isArray(data.presets) ? data.presets : data.maps;
    if (!Array.isArray(list) || list.length === 0) return;
    const presets = list
      .map(normalise)
      .filter((p: Preset | null): p is Preset => p !== null);
    presetState.presets = presets.length ? presets : [defaultPreset()];
    presetState.saved = presetState.presets.map((p) => JSON.stringify(p));
    presetState.active = Math.min(Math.max(0, data.active | 0), presetState.presets.length - 1);
    persist(); // migrate storage forward on first load
  } catch {}
}

/** Write the SAVED presets and the selection to storage. Never the working copies. */
function persist(): void {
  try {
    localStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({
        presets: presetState.saved.map((json) => JSON.parse(json)),
        active: presetState.active,
      }),
    );
  } catch {}
}

/** True when a preset has edits that have not been saved. */
export function isDirty(index: number = presetState.active): boolean {
  const p = presetState.presets[index];
  return p !== undefined && JSON.stringify(p) !== presetState.saved[index];
}

/** True when any preset has unsaved edits. */
export function anyDirty(): boolean {
  return presetState.presets.some((_, i) => isDirty(i));
}

/** Save the active preset's edits. */
export function savePreset(): void {
  presetState.saved[presetState.active] = JSON.stringify(activePreset());
  persist();
}

/** Throw the active preset's unsaved edits away, returning to what was saved. */
export function revertPreset(): void {
  const json = presetState.saved[presetState.active];
  if (json === undefined) return;
  presetState.presets[presetState.active] = JSON.parse(json);
  schedulePush({ colors: true, pitch: true });
}

// --- editing the colour layer ----------------------------------------------

/** Paint one key by hand. The generator stays underneath for the rest. */
export function paint(x: number, y: number, hex: string): void {
  activeColors().colors[cellKey(x, y)] = hex;
  schedulePush({ colors: true });
}

/** Paint every key in the current grid. */
export function fillAll(hex: string): void {
  const colors: Record<string, string> = {};
  for (const c of gridCells()) colors[c.key] = hex;
  activeColors().colors = colors;
  schedulePush({ colors: true });
}

/** How many keys have been painted by hand; zero means purely procedural. */
export function paintedCount(): number {
  return Object.keys(activeColors().colors).length;
}

/** Discard every hand-painted key, returning to the generated colours. */
export function resetColors(): void {
  activeColors().colors = {};
  schedulePush({ colors: true });
}

/** Update the active preset's colour generator parameters. */
export function updateColorGenerator(patch: Partial<GeneratorParams>): void {
  const m = activeColors();
  m.generator = { ...m.generator, ...patch };
  growRowsForWrap();
  recolourFromTuning();
  schedulePush({ colors: true });
}

// --- editing the pitch map -------------------------------------------------

/**
 * Update the active preset's pitch generator.
 *
 * Overrides are left alone: a reassigned pitch is a decision the user made, and
 * retuning is not a reason to discard it.
 *
 * The colour rows follow the layout and the anchor, so changing either re-pushes
 * the colours too. The tuning does not move a single accidental, and dragging it
 * must not drag the LEDs along (see `schedulePush`), so it pushes pitch alone.
 */
export function updatePitchGenerator(patch: Partial<PitchGenerator>): void {
  const m = activePitch();
  m.generator = { ...m.generator, ...patch };
  const movesRows =
    "layout" in patch || "rotation" in patch || "anchorNote" in patch || "anchorKey" in patch;
  // A scheme that follows the tuning changes colour with it — the one case
  // where retuning does have to reach the LEDs.
  const grown = "tuning" in patch && growRowsForWrap();
  const recoloured = "tuning" in patch && recolourFromTuning();
  schedulePush({ pitch: true, colors: movesRows || recoloured || grown });
}

/**
 * Give every accidental the active EDO wraps to a row of its own, adding rows
 * from the default sequence where the palette falls short (`rowsForWrap`).
 * Returns whether rows were added.
 */
function growRowsForWrap(): boolean {
  const g = activeColors().generator;
  const period = wrapPeriod(g, activePitch().generator);
  if (period === null) return false;
  const grown = rowsForWrap(g, edoWrap(period).accidentals);
  if (!grown) return false;
  activeColors().generator = { ...g, ...grown };
  return true;
}

/**
 * Rewrite the active preset's colours from its tuning, if it asks for that.
 *
 * Called after anything that could change the answer: the tuning, or the rows
 * themselves (how many, which scheme, where the anchor is). Returns whether the
 * colours are generated, so a caller knows the LEDs need them.
 */
function recolourFromTuning(): boolean {
  const g = activeColors().generator;
  const tuning = activePitch().generator.tuning;
  if (!g.autoColors || tuningError(tuning) !== null) return false;
  activeColors().generator = { ...g, ...autoColorsFor(g, fifthCents(tuning)) };
  return true;
}

// Mutators for per-key pitch overrides are deliberately absent. The data model
// carries them -- `pitchOn` resolves them, storage and the JSON interchange
// round-trip them, and the Preset tab lists any it finds -- but `P` pushes
// parameters, not a table, so a reassigned key cannot reach the board yet.
// Writing setters now would mean guessing at an editing flow before the command
// that has to carry it exists.

// --- managing the list -----------------------------------------------------

/**
 * Add a preset to the list, saved as it stands, and select it.
 *
 * Adding one is an explicit act, so unlike an edit it is written straight away
 * — a preset that vanished on reload because nobody pressed Save would be a
 * surprise. The same goes for removing and selecting.
 */
function addPreset(p: Preset): void {
  presetState.presets.push(p);
  presetState.saved.push(JSON.stringify(p));
  presetState.active = presetState.presets.length - 1;
  persist();
  schedulePush({ colors: true, pitch: true });
}

export function newPreset(): void {
  addPreset({
    name: `Preset ${presetState.presets.length + 1}`,
    colors: { colors: {}, generator: defaultGenerator() },
    pitch: defaultPitchMap(),
  });
}

/** Copy the active preset as it is on screen, unsaved edits included. */
export function duplicatePreset(): void {
  const src = $state.snapshot(activePreset()) as Preset;
  addPreset({ ...structuredClone(src), name: `${src.name} copy` });
}

/** Rename the active preset. An edit like any other: it needs saving. */
export function renamePreset(name: string): void {
  activePreset().name = name;
}

export function deletePreset(): void {
  if (presetState.presets.length <= 1) {
    const fresh = defaultPreset();
    presetState.presets = [fresh];
    presetState.saved = [JSON.stringify(fresh)];
    presetState.active = 0;
  } else {
    presetState.presets.splice(presetState.active, 1);
    presetState.saved.splice(presetState.active, 1);
    presetState.active = Math.min(presetState.active, presetState.presets.length - 1);
  }
  persist();
  schedulePush({ colors: true, pitch: true });
}

export function selectPreset(index: number): void {
  if (index < 0 || index >= presetState.presets.length) return;
  presetState.active = index;
  persist();
  schedulePush({ colors: true, pitch: true });
}

// --- pushing to the boards -------------------------------------------------

/**
 * Push the active preset's colours.
 *
 * Two routes, and which one is taken matters for more than bandwidth.
 *
 * `K` sends the GENERATOR — forty bytes the firmware evaluates per key at
 * absolute coordinates, propagated down the mesh tree. It is preferred whenever
 * the mapping is procedural, because it is the only route that keeps working
 * once we are gone: a board hot-plugged or power-cycled later learns its colours
 * from its parent, at its own grid offset, with no host in the loop.
 *
 * `L` sends a rendered 93-byte table per board, which is the only way to express
 * hand-painted keys. It is sent for boards carrying explicit colours, AFTER the
 * `K` frame so it lands on top of the generated base rather than under it.
 *
 * Firmware predating the mesh has neither `L` nor `T`, so when no topology has
 * been seen we fall back to the original `C` frame for the attached board.
 */
export async function pushColors(): Promise<void> {
  if (!ui.connected) return;
  // Taken before anything is sent: if the preset changes while frames are
  // going out, the boards hold THIS state, and the difference is noticed.
  const signature = colorsSignature();
  try {
    await sendColors();
  } finally {
    presetState.pushedColors = signature;
  }
}

/**
 * Everything a colour push depends on, as one string: the colours themselves,
 * the geometry they follow, what the attached firmware can be sent, and — only
 * when the boards cannot generate the scheme themselves — which boards there
 * are, since each then needs a table of its own.
 */
function colorsSignature(): string {
  const g = activePitch().generator;
  return JSON.stringify({
    colors: activeColors(),
    geometry: [g.layout, g.rotation ?? 0, g.anchorNote, g.anchorKey],
    // Wrapped rows change with the EDO; nothing else about the tuning moves a colour.
    wrap: wrapPeriod(activeColors().generator, g),
    caps: [canPushColorgen(), colorgenCaps()],
    seen: mesh.seen,
    grid: colorsAreSelfSufficient() ? null : mesh.boards.map((b) => [b.ox, b.oy]),
  });
}

/** True when the boards' colours are behind the active preset. */
export function colorsAreStale(): boolean {
  return ui.connected && presetState.pushedColors !== colorsSignature();
}

async function sendColors(): Promise<void> {
  const m = activeColors();
  const pitchGen = activePitch().generator;

  if (!mesh.seen) {
    const frame = new Uint8Array(1 + NUM_KEYS * 3);
    frame[0] = 0x43; // 'C'
    rgbForBoard(m, pitchGen, 0, 0, frame, 1);
    await sendBytes(frame);
    return;
  }

  const gen = canPushColorgen()
    ? colorgenFrame(m.generator, pitchGen, colorgenCaps())
    : null;
  if (gen) {
    await sendBytes(gen);
    presetState.pushedColorgen = frameHex(gen);
  }

  // With the generator pushed and nothing painted over it, the boards have
  // everything they need — including the ones not attached yet.
  if (gen && Object.keys(m.colors).length === 0) return;

  for (const board of mesh.boards) {
    const frame = new Uint8Array(1 + 4 + NUM_KEYS * 3);
    frame[0] = 0x4c; // 'L'
    const dv = new DataView(frame.buffer);
    dv.setInt16(1, board.ox, true);
    dv.setInt16(3, board.oy, true);
    rgbForBoard(m, pitchGen, board.ox, board.oy, frame, 5);
    await sendBytes(frame);
  }
}

/**
 * True when every board's colours follow from parameters the boards themselves
 * hold, so a board arriving later needs nothing from us.
 *
 * What the grid-change watcher in App.svelte keys off: a hand-painted or mixed
 * layer still has to be re-pushed when the topology changes.
 */
export function colorsAreSelfSufficient(): boolean {
  const m = activeColors();
  return (
    canPushColorgen() &&
    Object.keys(m.colors).length === 0 &&
    colorgenFrame(m.generator, activePitch().generator, colorgenCaps()) !== null
  );
}

/** True when the attached firmware understands `K`. */
export function canPushColorgen(): boolean {
  return ui.connected && fwAtLeast(...COLORGEN_PUSH_FW);
}

/** What the attached firmware's colour generator can do. */
export function colorgenCaps(): ColorgenCaps {
  return {
    partitions: ui.connected && fwAtLeast(...COLORGEN_PARTITIONS_FW),
    lighten: ui.connected && fwAtLeast(...COLORGEN_LIGHTEN_FW),
    wrap: ui.connected && fwAtLeast(...COLORGEN_WRAP_FW),
  };
}

/**
 * Whether the boards can generate the active colour scheme themselves, and if
 * not, why — which decides whether it survives the app being closed.
 *
 * With no board attached the answer is for current firmware, since there is no
 * version to ask.
 */
export function colorGenerationLimit(): "none" | "firmware" | "too-many" {
  const g = activeColors().generator;
  const pitchGen = activePitch().generator;
  if (colorgenFrame(g, pitchGen, ui.connected ? colorgenCaps() : COLORGEN_CAPS_LATEST)) return "none";
  return colorgenFrame(g, pitchGen, COLORGEN_CAPS_LATEST) ? "firmware" : "too-many";
}

/** True when the attached firmware understands `P`. */
export function canPushPitch(): boolean {
  return ui.connected && fwAtLeast(...PITCH_PUSH_FW);
}

/**
 * Push the active preset's tuning parameters.
 *
 * One unaddressed frame, not one per board: MIDI is generated only by the
 * USB-connected board, which plays every key in the grid from absolute
 * coordinates, so the master is the only board that needs a tuning at all.
 *
 * Gated on firmware version, and that gate is load-bearing rather than polite.
 * A 0.6.0 board ignores the unknown `P` command byte but then reads the 16
 * PAYLOAD bytes as commands — and 0x43 ('C') or 0x4C ('L') are entirely
 * reachable values in a millicent count or a signed matrix entry, either of
 * which would drop that board into a 97-byte colour collect and swallow the
 * rest of the stream.
 */
export async function pushPitch(): Promise<void> {
  if (!canPushPitch()) return;
  const frame = pitchFrame(activePitch(), canPushReference());
  await sendBytes(frame);
  presetState.pushedPitch = frameHex(frame);
}

const frameHex = (f: Uint8Array): string =>
  [...f].map((b) => b.toString(16).padStart(2, "0")).join("");

/**
 * True when the board is running a different tuning from the active preset.
 *
 * Never for long: `keepBoardsInSync` pushes as soon as this is so. It is still
 * exported for the MIDI monitor, which would otherwise report every note as
 * wrong for the instant in between.
 */
export function pitchIsStale(): boolean {
  if (!canPushPitch()) return false;
  return presetState.pushedPitch !== frameHex(pitchFrame(activePitch(), canPushReference()));
}

/** True when the attached firmware can be given a concert pitch. */
export function canPushReference(): boolean {
  return ui.connected && fwAtLeast(...PITCH_REFERENCE_FW);
}

/**
 * True when the preset asks for a concert pitch the attached board cannot be
 * given, so it sounds with every C on a 12-TET C instead.
 */
export function referenceIsIgnored(): boolean {
  return canPushPitch() && !canPushReference() && referenceOffsetMilli(activePitch().generator) !== 0;
}

/**
 * Bring the boards up to date with the active preset, if they are not.
 *
 * The boards showing and playing what the app shows is an invariant, not
 * something to ask the user to restore. This is called from an effect
 * (App.svelte) that re-runs whenever anything either side of the comparison
 * changes — an edit, a preset switch, a board being attached, the firmware
 * version arriving after connecting — so no caller has to remember to push.
 * The pushes themselves stay debounced.
 */
export function keepBoardsInSync(): void {
  if (!ui.connected) return;
  schedulePush({ pitch: pitchIsStale(), colors: colorsAreStale() });
}

/** Everything, for connect time. */
export async function pushAll(): Promise<void> {
  await pushPitch();
  await pushColors();
}

// Separate timers on purpose. A colour push is one 98-byte frame PER BOARD, and
// streaming the LED chain is what makes it flicker (the data line has no
// voltage margin — see the README), so dragging a tuning control must not drag
// the LEDs along with it.
let colorTimer: ReturnType<typeof setTimeout> | null = null;
let pitchTimer: ReturnType<typeof setTimeout> | null = null;

export function schedulePush(what: { colors?: boolean; pitch?: boolean }): void {
  if (!ui.connected) return;
  if (what.colors) {
    if (colorTimer) clearTimeout(colorTimer);
    colorTimer = setTimeout(() => void pushColors(), 100);
  }
  if (what.pitch) {
    if (pitchTimer) clearTimeout(pitchTimer);
    pitchTimer = setTimeout(() => void pushPitch(), 100);
  }
}

/** Forget what the board is running — on disconnect, where it may change. */
export function resetPushed(): void {
  presetState.pushedPitch = null;
  presetState.pushedColorgen = null;
  presetState.pushedColors = null;
}

// --- interchange -----------------------------------------------------------

export async function copyPresetJson(): Promise<void> {
  const text = JSON.stringify({ schema: "miso-preset-1", ...activePreset() }, null, 2);
  try {
    await navigator.clipboard.writeText(text);
    toast("Preset JSON copied.");
  } catch {
    window.prompt("Copy the preset JSON:", text);
  }
}

/** Accepts a preset, or either older colour-only document. */
export function importPresetJson(text: string): boolean {
  try {
    const p = normalise(JSON.parse(text));
    if (!p) return false;
    addPreset(p);
    return true;
  } catch {
    return false;
  }
}
