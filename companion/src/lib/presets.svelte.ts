// Presets: reactive state, localStorage persistence, and the wire frames that
// push one to the boards.
//
// A PRESET is how the instrument is set up: an LED mapping and a pitch mapping,
// with room for further preset-scoped settings. The two halves work the same way
// — keyed by absolute grid coordinate, procedurally generated or set per key,
// evaluated lazily so a board attached later simply resolves — but they differ
// in one structural respect worth knowing before editing this file:
//
//   A colour layer's generator is OPTIONAL. Hand-painting one freezes it
//   (`materialise`), because an unpainted coordinate can safely be black.
//
//   A pitch map's generator is MANDATORY and permanent. A coordinate with no
//   pitch is a silent key, and the `P` frame carries parameters rather than a
//   table, so there would be nothing left to send. Overrides are layered on top
//   and are never a cache — editing the generator must not clear them.
//
// See `PitchMap` in tuning.ts for the long version.

import { NUM_KEYS } from "./layout";
import {
  DEFAULT_PITCH_GENERATOR,
  LAYOUTS,
  defaultPitchMap,
  pitchFrame,
  type PitchGenerator,
  type PitchMap,
} from "./tuning";
import {
  DEFAULT_GENERATOR,
  bosanquetDefaultColors,
  coloursFromLedArray,
  generateColorAt,
  rgbForBoard,
  type ColorLayer,
  type GeneratorParams,
} from "./colorMaps";
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

function defaultPreset(): Preset {
  return { name: "Bosanquet", colors: bosanquetDefaultColors(), pitch: defaultPitchMap() };
}

export const presetState = $state({
  presets: [defaultPreset()] as Preset[],
  active: 0,
  /**
   * The pitch frame the board is believed to be running, as hex.
   *
   * The board's tuning is now whatever we last pushed, so an edited-but-unpushed
   * preset makes every MIDI monitor row disagree. Tracking this lets the UI say
   * so instead of looking broken.
   */
  pushedPitch: null as string | null,
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

  const colorGenerator = (raw.colors?.generator ?? raw.generator) as GeneratorParams | undefined;
  const colors: ColorLayer = {
    colors: colours,
    ...(colorGenerator ? { generator: colorGenerator } : {}),
  };

  const pitch = normalisePitch(raw.pitch);

  return {
    name: String(raw.name ?? "Preset"),
    colors,
    pitch,
    ...(raw.settings && typeof raw.settings === "object" ? { settings: raw.settings } : {}),
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
    presetState.active = Math.min(Math.max(0, data.active | 0), presetState.presets.length - 1);
    save(); // migrate storage forward on first load
  } catch {}
}

function save(): void {
  try {
    localStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({ presets: presetState.presets, active: presetState.active }),
    );
  } catch {}
}

// --- editing the colour layer ----------------------------------------------

/** Freeze a generated colour scheme into explicit colours over the current grid. */
function materialiseColors(m: ColorLayer, cells: { key: string; x: number; y: number }[]): void {
  if (!m.generator) return;
  const gen = m.generator;
  for (const c of cells) {
    if (m.colors[c.key] === undefined) {
      m.colors[c.key] = generateColorAt(gen, c.x, c.y);
    }
  }
  delete m.generator;
}

export function paint(x: number, y: number, hex: string): void {
  const m = activeColors();
  // Painting a generated mapping by hand detaches it from its parameters.
  materialiseColors(m, gridCells());
  m.colors[cellKey(x, y)] = hex;
  save();
  schedulePush({ colors: true });
}

export function fillAll(hex: string): void {
  const m = activeColors();
  delete m.generator;
  const colors: Record<string, string> = {};
  for (const c of gridCells()) colors[c.key] = hex;
  m.colors = colors;
  save();
  schedulePush({ colors: true });
}

/** Give the active preset a procedural colour scheme. */
export function createProceduralColorMap(params: GeneratorParams = DEFAULT_GENERATOR): void {
  const p = activePreset();
  p.colors = {
    colors: {},
    generator: {
      ...params,
      palette: [...params.palette],
      offset: [...params.offset] as [number, number],
    },
  };
  save();
  schedulePush({ colors: true });
}

/** Update the active preset's colour generator parameters. */
export function updateColorGenerator(patch: Partial<GeneratorParams>): void {
  const m = activeColors();
  if (!m.generator) return;
  m.generator = { ...m.generator, ...patch };
  m.colors = {}; // drop stale overrides so the new parameters show everywhere
  save();
  schedulePush({ colors: true });
}

// --- editing the pitch map -------------------------------------------------

/**
 * Update the active preset's pitch generator.
 *
 * Note what this does NOT do, in contrast to `updateColorGenerator` above: it
 * does not clear `overrides`. A painted colour under a generated scheme is a
 * stale cache; a reassigned pitch is a decision the user made, and retuning is
 * not a reason to discard it.
 */
export function updatePitchGenerator(patch: Partial<PitchGenerator>): void {
  const m = activePitch();
  m.generator = { ...m.generator, ...patch };
  save();
  schedulePush({ pitch: true });
}

// Mutators for per-key pitch overrides are deliberately absent. The data model
// carries them -- `pitchOn` resolves them, storage and the JSON interchange
// round-trip them, and the Pitch mapping tab lists any it finds -- but `P` pushes
// parameters, not a table, so a reassigned key cannot reach the board yet.
// Writing setters now would mean guessing at an editing flow before the command
// that has to carry it exists.

// --- managing the list -----------------------------------------------------

export function newPreset(): void {
  presetState.presets.push({
    name: `Preset ${presetState.presets.length + 1}`,
    colors: { colors: {} },
    pitch: defaultPitchMap(),
  });
  presetState.active = presetState.presets.length - 1;
  save();
  schedulePush({ colors: true, pitch: true });
}

export function duplicatePreset(): void {
  const src = activePreset();
  presetState.presets.push({
    name: `${src.name} copy`,
    colors: structuredClone(src.colors),
    pitch: structuredClone(src.pitch),
    ...(src.settings ? { settings: structuredClone(src.settings) } : {}),
  });
  presetState.active = presetState.presets.length - 1;
  save();
}

export function renamePreset(name: string): void {
  activePreset().name = name;
  save();
}

export function deletePreset(): void {
  if (presetState.presets.length <= 1) {
    presetState.presets = [defaultPreset()];
    presetState.active = 0;
  } else {
    presetState.presets.splice(presetState.active, 1);
    presetState.active = Math.min(presetState.active, presetState.presets.length - 1);
  }
  save();
  schedulePush({ colors: true, pitch: true });
}

export function selectPreset(index: number): void {
  presetState.active = index;
  save();
  schedulePush({ colors: true, pitch: true });
}

// --- pushing to the boards -------------------------------------------------

/**
 * Push the active preset's colours to every board in the grid.
 *
 * `L` addresses a board by its grid origin, so one frame goes out per board.
 * Firmware predating the mesh has neither `L` nor `T`, so when no topology has
 * been seen we fall back to the original `C` frame for the attached board.
 */
export async function pushColors(): Promise<void> {
  if (!ui.connected) return;
  const m = activeColors();

  if (!mesh.seen) {
    const frame = new Uint8Array(1 + NUM_KEYS * 3);
    frame[0] = 0x43; // 'C'
    rgbForBoard(m, 0, 0, frame, 1);
    await sendBytes(frame);
    return;
  }

  for (const board of mesh.boards) {
    const frame = new Uint8Array(1 + 4 + NUM_KEYS * 3);
    frame[0] = 0x4c; // 'L'
    const dv = new DataView(frame.buffer);
    dv.setInt16(1, board.ox, true);
    dv.setInt16(3, board.oy, true);
    rgbForBoard(m, board.ox, board.oy, frame, 5);
    await sendBytes(frame);
  }
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
  const frame = pitchFrame(activePitch());
  await sendBytes(frame);
  presetState.pushedPitch = frameHex(frame);
}

const frameHex = (f: Uint8Array): string =>
  [...f].map((b) => b.toString(16).padStart(2, "0")).join("");

/** True when the board is running a different tuning from the active preset. */
export function pitchIsStale(): boolean {
  if (!canPushPitch()) return false;
  return presetState.pushedPitch !== frameHex(pitchFrame(activePitch()));
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
    presetState.presets.push(p);
    presetState.active = presetState.presets.length - 1;
    save();
    schedulePush({ colors: true, pitch: true });
    return true;
  } catch {
    return false;
  }
}
