// Data engine: frame ingestion, per-key statistics, press-capture triggering.
// Deliberately non-reactive — canvases read it directly every animation frame,
// and the reactive UI snapshots it on a slow interval (see ui.svelte.ts).

export const NUM_KEYS = 31;
export const FRAME_LEN = 3 + 4 + NUM_KEYS * 2 + 1;

const HIST = 128; // samples for the noise-floor estimate
const RING = 4096; // ~4 s of frames at 1 kHz
const PRE_MS = 30; // capture window around a trigger
const POST_MS = 200;
const REFRACTORY_MS = 300;

export interface KeyStats {
  cur: number;
  rest: number; // NaN until captured
  min: number;
  max: number;
  sigma: number;
  lastTrig: number;
}

export interface Trace {
  key: number;
  color: string;
  pts: [number, number][]; // [ms relative to trigger, raw counts]
  peakDev: number;
  transitMs: number | null; // 20% -> 70% of excursion
}

export const keys: KeyStats[] = Array.from({ length: NUM_KEYS }, () => ({
  cur: 0,
  rest: NaN,
  min: Infinity,
  max: -Infinity,
  sigma: 0,
  lastTrig: -1e9,
}));

const hist = Array.from({ length: NUM_KEYS }, () => new Float64Array(HIST));
let histIdx = 0;
let histFill = 0;

const ringT = new Float64Array(RING);
const ringV = Array.from({ length: RING }, () => new Uint16Array(NUM_KEYS));
let ringHead = 0;
let ringCount = 0;

let lastTus: number | null = null;
export let timelineMs = 0;
export let frameCounter = 0;
export let badFrames = 0;

export const traces: Trace[] = [];
let pending: { key: number; trigMs: number; endMs: number }[] = [];
let restSamples: Uint16Array[] | null = null;

export function triggerThreshold(k: KeyStats): number {
  return Math.max(10 * k.sigma, 50);
}

export function onFrame(tUs: number, vals: Uint16Array): void {
  if (lastTus !== null) timelineMs += ((tUs - lastTus) >>> 0) / 1000; // wrap-safe µs delta
  lastTus = tUs;
  frameCounter++;

  ringT[ringHead] = timelineMs;
  ringV[ringHead].set(vals);
  ringHead = (ringHead + 1) % RING;
  if (ringCount < RING) ringCount++;

  for (let i = 0; i < NUM_KEYS; i++) {
    const k = keys[i];
    const v = vals[i];
    k.cur = v;
    if (v < k.min) k.min = v;
    if (v > k.max) k.max = v;
    hist[i][histIdx] = v;

    if (!isNaN(k.rest)) {
      const dev = Math.abs(v - k.rest);
      if (dev > triggerThreshold(k) && timelineMs - k.lastTrig > REFRACTORY_MS) {
        k.lastTrig = timelineMs;
        pending.push({ key: i, trigMs: timelineMs, endMs: timelineMs + POST_MS });
      }
    }
  }
  histIdx = (histIdx + 1) % HIST;
  if (histFill < HIST) histFill++;

  for (let p = pending.length - 1; p >= 0; p--) {
    if (timelineMs >= pending[p].endMs) {
      finalizeTrace(pending[p]);
      pending.splice(p, 1);
    }
  }

  if (restSamples) restSamples.push(vals.slice());
}

export function countBadFrame(): void {
  badFrames++;
}

export function updateSigmas(): void {
  if (histFill < 8) return;
  for (let i = 0; i < NUM_KEYS; i++) {
    let s = 0;
    let s2 = 0;
    for (let j = 0; j < histFill; j++) {
      const x = hist[i][j];
      s += x;
      s2 += x * x;
    }
    const mean = s / histFill;
    keys[i].sigma = Math.sqrt(Math.max(0, s2 / histFill - mean * mean));
  }
}

const TRACE_TOKENS = ["--accent", "--bar", "--good", "--warn", "--crit"];
let traceListeners: (() => void)[] = [];
export function onTracesChanged(fn: () => void): void {
  traceListeners.push(fn);
}

function finalizeTrace(p: { key: number; trigMs: number; endMs: number }): void {
  const k = keys[p.key];
  const pts: [number, number][] = [];
  for (let j = 0; j < ringCount; j++) {
    const idx = (ringHead - ringCount + j + RING) % RING;
    if (ringT[idx] >= p.trigMs - PRE_MS && ringT[idx] <= p.endMs) {
      pts.push([ringT[idx] - p.trigMs, ringV[idx][p.key]]);
    }
  }
  if (pts.length < 8) return;

  let peakDev = 0;
  let peakVal = k.rest;
  for (const [, v] of pts) {
    const d = Math.abs(v - k.rest);
    if (d > peakDev) {
      peakDev = d;
      peakVal = v;
    }
  }
  const sign = Math.sign(peakVal - k.rest) || 1;
  let t20: number | null = null;
  let t70: number | null = null;
  for (const [t, v] of pts) {
    const prog = (sign * (v - k.rest)) / peakDev;
    if (t20 === null && prog >= 0.2) t20 = t;
    if (t20 !== null && prog >= 0.7) {
      t70 = t;
      break;
    }
  }

  const style = getComputedStyle(document.documentElement);
  traces.push({
    key: p.key,
    color: style.getPropertyValue(TRACE_TOKENS[traces.length % TRACE_TOKENS.length]).trim(),
    pts,
    peakDev,
    transitMs: t20 !== null && t70 !== null ? t70 - t20 : null,
  });
  while (traces.length > 8) traces.shift();
  for (const fn of traceListeners) fn();
}

export function clearTraces(): void {
  traces.length = 0;
  for (const fn of traceListeners) fn();
}

export function resetWatermarks(): void {
  for (const k of keys) {
    k.min = Infinity;
    k.max = -Infinity;
  }
}

/** Collects scans for `ms`, then sets each key's rest to the median. */
export function captureRest(ms: number, done: (nScans: number) => void): void {
  restSamples = [];
  setTimeout(() => {
    const samples = restSamples!;
    restSamples = null;
    if (samples.length >= 10) {
      for (let i = 0; i < NUM_KEYS; i++) {
        const col = samples.map((s) => s[i]).sort((a, b) => a - b);
        keys[i].rest = col[col.length >> 1];
      }
    }
    done(samples.length);
  }, ms);
}

export function resetTimeline(): void {
  lastTus = null;
  timelineMs = 0;
  ringHead = 0;
  ringCount = 0;
  frameCounter = 0;
  badFrames = 0;
  pending = [];
}

export function calibrationJson(live: boolean): string {
  return JSON.stringify(
    {
      schema: "miso-cal-1",
      captured: new Date().toISOString(),
      source: live ? "live" : "SIMULATED",
      keys: keys.map((k, i) => ({
        i,
        rest: isNaN(k.rest) ? null : k.rest,
        min: k.min <= k.max ? k.min : null,
        max: k.max >= k.min ? k.max : null,
      })),
    },
    null,
    2,
  );
}
