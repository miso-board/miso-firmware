// Named colour mappings: reactive state, localStorage persistence, and the wire
// frames that push a mapping to the boards.
//
// A mapping is keyed by ABSOLUTE grid coordinate ("x,y"), not by LED index, so
// one scheme covers the whole grid however many boards are attached and
// survives boards being added, removed or rearranged. That also matches how the
// procedural generator already worked: it colours a key by the accidental of
// the pitch at its coordinate, which is a function of absolute position.

import { NUM_KEYS, LED_POS } from "./layout";
import { LAYOUTS, pitchAt, type LayoutId } from "./tuning";
import { scaleHex, mixWhite } from "./ledColor";
import { sendBytes } from "./serial";
import { ui, toast } from "./ui.svelte";
import { mesh, gridCells, cellKey } from "./mesh.svelte";

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

export interface ColorMap {
  name: string;
  /** Explicit colours by "x,y". A generated map fills the gaps from `generator`. */
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
export function colorAt(m: ColorMap, x: number, y: number): string {
  const explicit = m.colors[cellKey(x, y)];
  if (explicit !== undefined) return explicit;
  if (m.generator) return generateColorAt(m.generator, x, y);
  return "#000000";
}

const STORAGE_KEY = "miso-color-maps-v2";
const STORAGE_KEY_V1 = "miso-color-maps-v1";

// Mirrors the firmware boot pattern (fill_bosanquet groups by LED index),
// so a fresh install starts from what the board already shows.
const BOSANQUET_GROUPS: Record<string, number[]> = {
  "#190202": [0, 5, 6, 18, 19], // double-flat
  "#190a02": [1, 3, 4, 7, 16, 17, 20], // flat
  "#141402": [2, 8, 9, 15, 21, 22, 28], // natural
  "#051905": [10, 13, 14, 23, 26, 27, 29], // sharp
  "#050519": [11, 12, 24, 25, 30], // double-sharp
};

/** An LED-indexed array from the old format is a board at the origin. */
function coloursFromLedArray(arr: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  arr.forEach((hex, led) => {
    const pos = LED_POS[led];
    if (pos) out[cellKey(pos[0], pos[1])] = String(hex);
  });
  return out;
}

function bosanquetDefault(): ColorMap {
  const arr = Array(NUM_KEYS).fill("#000000");
  for (const [hex, leds] of Object.entries(BOSANQUET_GROUPS)) {
    for (const l of leds) arr[l] = hex;
  }
  return { name: "Bosanquet", colors: coloursFromLedArray(arr) };
}

export const colorState = $state({
  maps: [bosanquetDefault()] as ColorMap[],
  active: 0,
});

export function activeMap(): ColorMap {
  return colorState.maps[colorState.active] ?? colorState.maps[0];
}

function normalise(m: any): ColorMap | null {
  if (!m) return null;
  // v2: colours already keyed by coordinate. v1: a 31-entry LED-indexed array.
  const colors = Array.isArray(m.colors)
    ? coloursFromLedArray(m.colors as string[])
    : m.colors && typeof m.colors === "object"
      ? Object.fromEntries(Object.entries(m.colors).map(([k, v]) => [k, String(v)]))
      : null;
  if (!colors) return null;
  return {
    name: String(m.name ?? "Mapping"),
    colors,
    ...(m.generator ? { generator: m.generator as GeneratorParams } : {}),
  };
}

export function load(): void {
  try {
    const raw = localStorage.getItem(STORAGE_KEY) ?? localStorage.getItem(STORAGE_KEY_V1);
    if (!raw) return;
    const data = JSON.parse(raw);
    if (!Array.isArray(data.maps) || data.maps.length === 0) return;
    const maps = data.maps.map(normalise).filter((m: ColorMap | null): m is ColorMap => m !== null);
    colorState.maps = maps.length ? maps : [bosanquetDefault()];
    colorState.active = Math.min(Math.max(0, data.active | 0), colorState.maps.length - 1);
    save(); // migrate v1 storage forward on first load
  } catch {}
}

function save(): void {
  try {
    localStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({ maps: colorState.maps, active: colorState.active }),
    );
  } catch {}
}

/** Freeze a generated scheme into explicit colours over the current grid. */
function materialise(m: ColorMap): void {
  if (!m.generator) return;
  for (const c of gridCells()) {
    if (m.colors[c.key] === undefined) m.colors[c.key] = generateColorAt(m.generator, c.x, c.y);
  }
  delete m.generator;
}

export function paint(x: number, y: number, hex: string): void {
  const m = activeMap();
  // Painting a generated mapping by hand detaches it from its parameters.
  materialise(m);
  m.colors[cellKey(x, y)] = hex;
  save();
  schedulePush();
}

/** Start a procedural mapping (leaves hand-painted mappings untouched). */
export function createProceduralMap(params: GeneratorParams = DEFAULT_GENERATOR): void {
  const generator: GeneratorParams = {
    ...params,
    palette: [...params.palette],
    offset: [...params.offset] as [number, number],
  };
  colorState.maps.push({
    name: `${LAYOUTS[generator.layout].name} rows`,
    colors: {},
    generator,
  });
  colorState.active = colorState.maps.length - 1;
  save();
  schedulePush();
}

/** Update the active mapping's generator parameters. */
export function updateGenerator(patch: Partial<GeneratorParams>): void {
  const m = activeMap();
  if (!m.generator) return;
  m.generator = { ...m.generator, ...patch };
  m.colors = {}; // drop stale overrides so the new parameters show everywhere
  save();
  schedulePush();
}

export function fillAll(hex: string): void {
  const m = activeMap();
  delete m.generator;
  const colors: Record<string, string> = {};
  for (const c of gridCells()) colors[c.key] = hex;
  m.colors = colors;
  save();
  schedulePush();
}

export function newMap(): void {
  colorState.maps.push({ name: `Mapping ${colorState.maps.length + 1}`, colors: {} });
  colorState.active = colorState.maps.length - 1;
  save();
  schedulePush();
}

export function duplicateMap(): void {
  const src = activeMap();
  colorState.maps.push({
    name: `${src.name} copy`,
    colors: { ...src.colors },
    ...(src.generator ? { generator: structuredClone(src.generator) } : {}),
  });
  colorState.active = colorState.maps.length - 1;
  save();
}

export function renameMap(name: string): void {
  activeMap().name = name;
  save();
}

export function deleteMap(): void {
  if (colorState.maps.length <= 1) {
    colorState.maps = [bosanquetDefault()];
    colorState.active = 0;
  } else {
    colorState.maps.splice(colorState.active, 1);
    colorState.active = Math.min(colorState.active, colorState.maps.length - 1);
  }
  save();
  schedulePush();
}

export function selectMap(index: number): void {
  colorState.active = index;
  save();
  schedulePush();
}

function hexToRgb(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  if (!m) return [0, 0, 0];
  const v = parseInt(m[1], 16);
  return [(v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff];
}

/** 93 RGB bytes for one board, in LED-chain order, from absolute coordinates. */
function rgbForBoard(m: ColorMap, ox: number, oy: number, into: Uint8Array, at: number): void {
  LED_POS.forEach(([lx, ly], led) => {
    const [r, g, b] = hexToRgb(colorAt(m, lx + ox, ly + oy));
    into[at + led * 3] = r;
    into[at + led * 3 + 1] = g;
    into[at + led * 3 + 2] = b;
  });
}

/**
 * Push the active mapping to every board in the grid.
 *
 * `L` addresses a board by its grid origin, so one frame goes out per board.
 * Firmware predating the mesh has neither `L` nor `T`, so when no topology has
 * been seen we fall back to the original `C` frame for the attached board.
 */
export async function pushToBoard(): Promise<void> {
  if (!ui.connected) return;
  const m = activeMap();

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
    new DataView(frame.buffer).setInt16(1, board.ox, true);
    new DataView(frame.buffer).setInt16(3, board.oy, true);
    rgbForBoard(m, board.ox, board.oy, frame, 5);
    await sendBytes(frame);
  }
}

let pushTimer: ReturnType<typeof setTimeout> | null = null;
export function schedulePush(): void {
  if (!ui.connected) return;
  if (pushTimer) clearTimeout(pushTimer);
  pushTimer = setTimeout(() => void pushToBoard(), 100);
}

export async function copyMapJson(): Promise<void> {
  const text = JSON.stringify({ schema: "miso-colors-2", ...activeMap() }, null, 2);
  try {
    await navigator.clipboard.writeText(text);
    toast("Mapping JSON copied.");
  } catch {
    window.prompt("Copy the mapping JSON:", text);
  }
}

export function importMapJson(text: string): boolean {
  try {
    const m = normalise(JSON.parse(text));
    if (!m) return false;
    colorState.maps.push(m);
    colorState.active = colorState.maps.length - 1;
    save();
    schedulePush();
    return true;
  } catch {
    return false;
  }
}
