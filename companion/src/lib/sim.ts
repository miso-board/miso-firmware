// Simulated sensor data so the instrument is alive before a board is connected.
// Feeds the same onFrame() pipeline as the serial parser.

import { NUM_KEYS, onFrame } from "./engine";

interface SimPress {
  key: number;
  t0: number;
  durUs: number;
  depth: number;
  dir: 1 | -1;
}

let timer: ReturnType<typeof setInterval> | null = null;
let press: SimPress | null = null;
let tUs = 0;
let lastWall = 0;

const rest = Array.from(
  { length: NUM_KEYS },
  (_, i) => 2400 + Math.round(60 * Math.sin(i * 1.7)) + (i % 5) * 8,
);

export function startSim(): void {
  if (timer) return;
  lastWall = performance.now();
  timer = setInterval(() => {
    // Generate frames for the wall-clock time actually elapsed, so the sim
    // keeps its 500 Hz rate even when the browser throttles background-tab
    // timers (capped at 1 s of catch-up).
    const now = performance.now();
    const frames = Math.min(Math.round((now - lastWall) / 2), 500);
    lastWall = now;
    for (let n = 0; n < frames; n++) {
      tUs += 2000; // 500 Hz scan
      const vals = new Uint16Array(NUM_KEYS);
      for (let i = 0; i < NUM_KEYS; i++) {
        let v = rest[i] + (Math.random() - 0.5) * 6;
        if (press && press.key === i) {
          const ph = (tUs - press.t0) / press.durUs;
          if (ph >= 0 && ph < 1) {
            v += press.dir * press.depth * Math.sin(Math.PI * Math.min(ph * 1.4, 1)) ** 2;
          } else if (ph >= 1) {
            press = null;
          }
        }
        vals[i] = Math.max(0, Math.min(4095, Math.round(v)));
      }
      onFrame(tUs >>> 0, vals);
    }
    if (!press && Math.random() < 0.02) {
      press = {
        key: Math.floor(Math.random() * NUM_KEYS),
        t0: tUs + 100_000,
        durUs: (150 + Math.random() * 500) * 1000,
        depth: 500 + Math.random() * 900,
        dir: Math.random() < 0.5 ? 1 : -1,
      };
    }
  }, 20);
}

export function stopSim(): void {
  if (timer) clearInterval(timer);
  timer = null;
  press = null;
}
