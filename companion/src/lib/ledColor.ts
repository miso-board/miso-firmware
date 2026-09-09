// Screen rendering of LED colors.
//
// SK6812s are bright enough that usable values are tiny — a vivid green is
// #051905 — which a monitor renders as near-black. These helpers split a
// color into hue (shown at full legibility) and intensity (shown as a bloom),
// so the board reads the way the physical device looks rather than the way
// the raw numbers do. Stored and transmitted values are never touched.

export interface LedColor {
  hex: string;
  rgb: [number, number, number];
  /** 0..1, the max channel. Real mappings sit around 0.02–0.25. */
  intensity: number;
  /** Gamma-companded intensity, expanding the low end where real values live. */
  perceived: number;
  /** Hue at full legibility: channels scaled so the brightest reaches 255. */
  hue: string;
  /** Hue mixed toward white — the "hot core" of a lit LED. */
  core: string;
  /** Opacity for the outer bloom layer. */
  bloom: number;
  off: boolean;
}

export function parseHex(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  if (!m) return [0, 0, 0];
  const v = parseInt(m[1], 16);
  return [(v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff];
}

export function toHex(rgb: [number, number, number]): string {
  return (
    "#" +
    rgb.map((c) => Math.max(0, Math.min(255, Math.round(c))).toString(16).padStart(2, "0")).join("")
  );
}

export function ledColor(hex: string): LedColor {
  const rgb = parseHex(hex);
  const max = Math.max(...rgb);
  const intensity = max / 255;
  const perceived = intensity > 0 ? Math.pow(intensity, 1 / 2.2) : 0;
  const hueRgb: [number, number, number] = max > 0 ? [
    (rgb[0] * 255) / max,
    (rgb[1] * 255) / max,
    (rgb[2] * 255) / max,
  ] : [0, 0, 0];
  // Brighter LEDs look whiter at the middle; dimmer ones keep more of their hue.
  const k = 0.25 + 0.55 * perceived;
  const coreRgb: [number, number, number] = [
    hueRgb[0] + (255 - hueRgb[0]) * k,
    hueRgb[1] + (255 - hueRgb[1]) * k,
    hueRgb[2] + (255 - hueRgb[2]) * k,
  ];
  return {
    hex,
    rgb,
    intensity,
    perceived,
    hue: toHex(hueRgb),
    core: toHex(coreRgb),
    bloom: 0.12 + 0.5 * perceived,
    off: max === 0,
  };
}

/** Mix a color toward white by `amount` (0..1), keeping its hue family. */
export function mixWhite(hex: string, amount: number): string {
  const [r, g, b] = parseHex(hex);
  return toHex([r + (255 - r) * amount, g + (255 - g) * amount, b + (255 - b) * amount]);
}

/** Scale a color's channels to `percent` of their value (for LED brightness). */
export function scaleHex(hex: string, percent: number): string {
  const k = percent / 100;
  const [r, g, b] = parseHex(hex);
  return toHex([r * k, g * k, b * k]);
}

/** CSS for a small square/round swatch that reads like a lit LED. */
export function swatchStyle(hex: string): string {
  const c = ledColor(hex);
  if (c.off) return "background: var(--panel-2)";
  return (
    `background: radial-gradient(circle at 50% 38%, ${c.core}, ${c.hue});` +
    ` box-shadow: 0 0 ${(4 + 8 * c.perceived).toFixed(1)}px ${c.hue}${Math.round(c.bloom * 255)
      .toString(16)
      .padStart(2, "0")}`
  );
}
