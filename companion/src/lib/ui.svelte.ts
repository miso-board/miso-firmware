// Reactive UI state. The engine is non-reactive for throughput; this store
// holds what Svelte templates render, refreshed on slow intervals.

import * as engine from "./engine";
import { NUM_KEYS } from "./engine";

export interface Row {
  cur: number;
  rest: number;
  min: number;
  max: number;
  sigma: number;
  active: boolean;
}

export interface TraceMeta {
  key: number;
  color: string;
  peakDev: number;
  transitMs: number | null;
}

export interface KeyEvent {
  /** Sensor index on the originating board. */
  key: number;
  kind: "DOWN" | "UP";
  vel: number | null;
  dtUs: number | null;
  /** Absolute grid coordinate. Firmware before 0.6.0 omits it. */
  x: number;
  y: number;
  at: string; // wall-clock HH:MM:SS.mmm
}

export const ui = $state({
  connected: false,
  serialSupported: true,
  serialBlocked: false, // SecurityError: embedded context without serial permission
  scanHz: null as number | null,
  /** Firmware version from the INFO line's `fw=miso X.Y.Z`, null until seen. */
  fwVersion: null as string | null,
  fps: 0,
  badFrames: 0,
  toast: "",
  rows: [] as Row[],
  traces: [] as TraceMeta[],
  events: [] as KeyEvent[],
  /** Currently-held keys anywhere in the grid, keyed "x,y". */
  held: {} as Record<string, boolean>,
});

/**
 * Whether the attached firmware is at least this version.
 *
 * Capability gate for commands older firmware does not have. Unknown version
 * reads as too old, because sending a binary command to a board that does not
 * know it is worse than not sending it: the payload bytes get read as commands.
 */
export function fwAtLeast(major: number, minor: number, patch = 0): boolean {
  const m = /^(\d+)\.(\d+)\.(\d+)/.exec(ui.fwVersion ?? "");
  if (!m) return false;
  const have = [+m[1], +m[2], +m[3]];
  const want = [major, minor, patch];
  for (let i = 0; i < 3; i++) {
    if (have[i] !== want[i]) return have[i] > want[i];
  }
  return true;
}

export function pushEvent(ev: KeyEvent): void {
  ui.events.unshift(ev);
  if (ui.events.length > 12) ui.events.pop();
  const k = `${ev.x},${ev.y}`;
  if (ev.kind === "DOWN") ui.held[k] = true;
  else delete ui.held[k];
}

/** Drop every held flag — on disconnect, where no key-up is coming. */
export function clearHeld(): void {
  ui.held = {};
}

let toastTimer: ReturnType<typeof setTimeout> | null = null;
export function toast(msg: string): void {
  ui.toast = msg;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (ui.toast = ""), 3200);
}

export function refreshRows(): void {
  engine.updateSigmas();
  ui.rows = engine.keys.map((k) => ({
    cur: k.cur,
    rest: k.rest,
    min: k.min,
    max: k.max,
    sigma: k.sigma,
    active: !isNaN(k.rest) && Math.abs(k.cur - k.rest) > engine.triggerThreshold(k),
  }));
}

export function refreshTraces(): void {
  ui.traces = engine.traces
    .map((t) => ({ key: t.key, color: t.color, peakDev: t.peakDev, transitMs: t.transitMs }))
    .reverse();
}

refreshRows(); // populate the table before the first interval tick
if (ui.rows.length !== NUM_KEYS) ui.rows = []; // (defensive; never expected)
