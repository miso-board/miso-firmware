// Named per-key color mappings: reactive state, localStorage persistence,
// and the 'C' wire frame that pushes the active map to the board.
// Colors are hex strings ("#rrggbb") in LED-chain order.

import { NUM_KEYS, LED_POS } from "./layout";
import { LAYOUTS, pitchAt, type LayoutId } from "./tuning";
import { scaleHex, mixWhite } from "./ledColor";
import { sendBytes } from "./serial";
import { ui, toast } from "./ui.svelte";

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
  colors: string[]; // NUM_KEYS entries, LED-chain order
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

/** Colour every key by the accidental (Bosanquet row) of the note on it. */
export function generateColors(p: GeneratorParams): string[] {
  const layout = LAYOUTS[p.layout];
  const n = Math.max(1, p.palette.length);
  return LED_POS.map(([x, y]) => {
    const pitch = pitchAt(layout, x, y, p.offset);
    // Rows outside the palette wrap back through it.
    const idx = (((pitch.accidental - p.startAccidental) % n) + n) % n;
    let hex = p.palette[idx] ?? "#000000";
    if (p.lightenCDE && pitch.pc7 < 3) hex = mixWhite(hex, CDE_LIGHTEN);
    return scaleHex(hex, p.brightness);
  });
}

const STORAGE_KEY = "miso-color-maps-v1";

// Mirrors the firmware boot pattern (fill_bosanquet groups by LED index),
// so a fresh install starts from what the board already shows.
const BOSANQUET_GROUPS: Record<string, number[]> = {
  "#190202": [0, 5, 6, 18, 19], // double-flat
  "#190a02": [1, 3, 4, 7, 16, 17, 20], // flat
  "#141402": [2, 8, 9, 15, 21, 22, 28], // natural
  "#051905": [10, 13, 14, 23, 26, 27, 29], // sharp
  "#050519": [11, 12, 24, 25, 30], // double-sharp
};

function bosanquetDefault(): ColorMap {
  const colors = Array(NUM_KEYS).fill("#000000");
  for (const [hex, leds] of Object.entries(BOSANQUET_GROUPS)) {
    for (const l of leds) colors[l] = hex;
  }
  return { name: "Bosanquet", colors };
}

export const colorState = $state({
  maps: [bosanquetDefault()] as ColorMap[],
  active: 0,
});

export function activeMap(): ColorMap {
  return colorState.maps[colorState.active] ?? colorState.maps[0];
}

export function load(): void {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return;
    const data = JSON.parse(raw);
    if (Array.isArray(data.maps) && data.maps.length > 0) {
      colorState.maps = data.maps
        .filter((m: ColorMap) => Array.isArray(m.colors) && m.colors.length === NUM_KEYS)
        .map((m: ColorMap) => ({
          name: String(m.name),
          colors: m.colors.map(String),
          ...(m.generator ? { generator: m.generator } : {}),
        }));
      if (colorState.maps.length === 0) colorState.maps = [bosanquetDefault()];
      colorState.active = Math.min(Math.max(0, data.active | 0), colorState.maps.length - 1);
    }
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

export function paint(led: number, hex: string): void {
  const m = activeMap();
  // Painting a generated mapping by hand detaches it from its parameters.
  delete m.generator;
  m.colors[led] = hex;
  save();
  schedulePush();
}

/** Start a procedural mapping (leaves hand-painted mappings untouched). */
export function createProceduralMap(params: GeneratorParams = DEFAULT_GENERATOR): void {
  const generator: GeneratorParams = { ...params, palette: [...params.palette], offset: [...params.offset] as [number, number] };
  colorState.maps.push({
    name: `${LAYOUTS[generator.layout].name} rows`,
    colors: generateColors(generator),
    generator,
  });
  colorState.active = colorState.maps.length - 1;
  save();
  schedulePush();
}

/** Update the active mapping's generator parameters and regenerate its colours. */
export function updateGenerator(patch: Partial<GeneratorParams>): void {
  const m = activeMap();
  if (!m.generator) return;
  m.generator = { ...m.generator, ...patch };
  m.colors = generateColors(m.generator);
  save();
  schedulePush();
}

export function fillAll(hex: string): void {
  activeMap().colors = Array(NUM_KEYS).fill(hex);
  save();
  schedulePush();
}

export function newMap(): void {
  colorState.maps.push({ name: `Mapping ${colorState.maps.length + 1}`, colors: Array(NUM_KEYS).fill("#000000") });
  colorState.active = colorState.maps.length - 1;
  save();
  schedulePush();
}

export function duplicateMap(): void {
  const src = activeMap();
  colorState.maps.push({
    name: `${src.name} copy`,
    colors: [...src.colors],
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

/** Push the active map to the board: 'C' + 31x RGB in LED-chain order. */
export async function pushToBoard(): Promise<void> {
  if (!ui.connected) return;
  const frame = new Uint8Array(1 + NUM_KEYS * 3);
  frame[0] = 0x43; // 'C'
  activeMap().colors.forEach((hex, l) => {
    const [r, g, b] = hexToRgb(hex);
    frame[1 + l * 3] = r;
    frame[2 + l * 3] = g;
    frame[3 + l * 3] = b;
  });
  await sendBytes(frame);
}

let pushTimer: ReturnType<typeof setTimeout> | null = null;
export function schedulePush(): void {
  if (!ui.connected) return;
  if (pushTimer) clearTimeout(pushTimer);
  pushTimer = setTimeout(() => void pushToBoard(), 100);
}

export async function copyMapJson(): Promise<void> {
  const text = JSON.stringify({ schema: "miso-colors-1", ...activeMap() }, null, 2);
  try {
    await navigator.clipboard.writeText(text);
    toast("Mapping JSON copied.");
  } catch {
    window.prompt("Copy the mapping JSON:", text);
  }
}

export function importMapJson(text: string): boolean {
  try {
    const data = JSON.parse(text);
    if (!Array.isArray(data.colors) || data.colors.length !== NUM_KEYS) return false;
    colorState.maps.push({
      name: String(data.name || "Imported"),
      colors: data.colors.map(String),
      ...(data.generator ? { generator: data.generator } : {}),
    });
    colorState.active = colorState.maps.length - 1;
    save();
    schedulePush();
    return true;
  } catch {
    return false;
  }
}
