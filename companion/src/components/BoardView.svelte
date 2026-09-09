<script lang="ts">
  import { LED_PIXEL, SENSOR_FOR_LED, BOARD_ROTATION, NUM_KEYS } from "../lib/layout";
  import { ledColor } from "../lib/ledColor";

  interface Props {
    /** Assigned color per LED-chain index, as stored/sent (#rrggbb). */
    colors: string[];
    /** Per-LED "currently pressed" flags (outline highlight). */
    heldLed?: boolean[];
    labels?: "led" | "sensor" | "note" | "none";
    /** Per-LED note names, used when labels === "note". */
    noteNames?: string[];
    /** Draw literal stored values instead of the lit-LED simulation. */
    trueColor?: boolean;
    /** Paint callback; when set, hexes are clickable/drag-paintable. */
    onpaint?: (led: number) => void;
    onhover?: (led: number | null) => void;
  }
  let {
    colors,
    heldLed = [],
    labels = "none",
    noteNames = [],
    trueColor = false,
    onpaint,
    onhover,
  }: Props = $props();

  const S = 26; // hex radius in SVG units
  const ROT_DEG = (BOARD_ROTATION * 180) / Math.PI;
  // Unique per instance so multiple boards don't share gradient/filter ids.
  const gid = "bv" + Math.random().toString(36).slice(2, 8);

  const centers = LED_PIXEL.map(([x, y]) => [x * S, y * S] as [number, number]);
  const lit = $derived(colors.map((hex) => ledColor(hex)));

  const PAD = S * 1.6; // room for the bloom to spill past the outer keys
  const minX = Math.min(...centers.map((c) => c[0])) - PAD;
  const maxX = Math.max(...centers.map((c) => c[0])) + PAD;
  const minY = Math.min(...centers.map((c) => c[1])) - PAD;
  const maxY = Math.max(...centers.map((c) => c[1])) + PAD;

  /** Hexagon vertices, rotated with the grid so the tiling stays rigid. */
  function hexPoints(cx: number, cy: number, r: number): string {
    const pts: string[] = [];
    for (let i = 0; i < 6; i++) {
      const a = ((60 * i - 90 - ROT_DEG) * Math.PI) / 180;
      pts.push(`${(cx + r * Math.cos(a)).toFixed(1)},${(cy + r * Math.sin(a)).toFixed(1)}`);
    }
    return pts.join(" ");
  }

  function fillFor(led: number): string {
    if (lit[led].off) return "var(--panel-2)";
    return trueColor ? colors[led] : `url(#${gid}-g${led})`;
  }

  function onEnter(led: number, e: PointerEvent) {
    onhover?.(led);
    if (onpaint && (e.buttons & 1) !== 0) onpaint(led);
  }
</script>

<svg
  viewBox="{minX} {minY} {maxX - minX} {maxY - minY}"
  class="block w-full select-none"
  style="height: min(52vh, 400px); touch-action: none"
  role={onpaint ? "group" : "img"}
  aria-label="Miso board"
  onpointerleave={() => onhover?.(null)}
>
  <defs>
    <filter id="{gid}-bloom" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="5" />
    </filter>
    {#each lit as c, led}
      {#if !c.off}
        <radialGradient id="{gid}-g{led}" cx="50%" cy="38%" r="62%">
          <stop offset="0%" stop-color={c.core} />
          <stop offset="100%" stop-color={c.hue} />
        </radialGradient>
      {/if}
    {/each}
  </defs>

  <!-- Outer bloom: one filter over the whole group, so neighbouring glows merge -->
  {#if !trueColor}
    <g filter="url(#{gid}-bloom)">
      {#each centers as [cx, cy], led}
        {#if !lit[led].off}
          <polygon
            points={hexPoints(cx, cy, S * 1.22)}
            fill={lit[led].hue}
            opacity={lit[led].bloom}
          />
        {/if}
      {/each}
    </g>
  {/if}

  {#each centers as [cx, cy], led}
    <polygon
      points={hexPoints(cx, cy, S * 0.93)}
      fill={fillFor(led)}
      stroke={heldLed[led] ? "var(--accent)" : "var(--line)"}
      stroke-width={heldLed[led] ? 3 : 1}
      style={onpaint ? "cursor: pointer" : ""}
      role={onpaint ? "button" : "presentation"}
      aria-label="Key LED {led}"
      onpointerdown={() => onpaint?.(led)}
      onpointerenter={(e) => onEnter(led, e)}
    >
      <title>LED {led} · sensor {SENSOR_FOR_LED[led]} · {colors[led]}</title>
    </polygon>
    {#if labels !== "none"}
      <text
        x={cx}
        y={cy}
        text-anchor="middle"
        dominant-baseline="central"
        fill={lit[led].off || trueColor ? "var(--text-dim)" : "#000000bb"}
        style="font: {labels === 'note' ? 11 : 10}px 'IBM Plex Mono', monospace; pointer-events: none"
      >{labels === "note" ? (noteNames[led] ?? "") : labels === "led" ? led : SENSOR_FOR_LED[led]}</text>
    {/if}
  {/each}
</svg>
{#if colors.length !== NUM_KEYS}
  <p class="text-xs" style="color: var(--crit)">BoardView: expected {NUM_KEYS} colors.</p>
{/if}
