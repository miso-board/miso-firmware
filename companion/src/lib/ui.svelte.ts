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
  key: number;
  kind: "DOWN" | "UP";
  vel: number | null;
  dtUs: number | null;
  at: string; // wall-clock HH:MM:SS.mmm
}

export const ui = $state({
  connected: false,
  serialSupported: true,
  serialBlocked: false, // SecurityError: embedded context without serial permission
  scanHz: null as number | null,
  fps: 0,
  badFrames: 0,
  toast: "",
  rows: [] as Row[],
  traces: [] as TraceMeta[],
  events: [] as KeyEvent[],
  held: Array(NUM_KEYS).fill(false) as boolean[],
});

export function pushEvent(ev: KeyEvent): void {
  ui.events.unshift(ev);
  if (ui.events.length > 12) ui.events.pop();
  if (ev.key < NUM_KEYS) ui.held[ev.key] = ev.kind === "DOWN";
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
