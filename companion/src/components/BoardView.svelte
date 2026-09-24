<script lang="ts" module>
  /** One key to draw, positioned by absolute grid coordinate. */
  export interface ViewCell {
    /** "x,y" — unique across the grid. */
    key: string;
    x: number;
    y: number;
    /** Assigned colour, as stored/sent (#rrggbb). */
    color: string;
    held?: boolean;
    label?: string;
    title?: string;
    /** Which board this key belongs to, for the optional board captions. */
    boardUid?: string;
  }
</script>

<script lang="ts">
  import { axialToPixel, BOARD_ROTATION } from "../lib/layout";
  import { ledColor } from "../lib/ledColor";

  interface Props {
    /** Every key to render, anywhere in the grid. */
    cells: ViewCell[];
    /** Draw literal stored values instead of the lit-LED simulation. */
    trueColor?: boolean;
    /** Paint callback; when set, hexes are clickable/drag-paintable. */
    onpaint?: (cell: ViewCell) => void;
    onhover?: (cell: ViewCell | null) => void;
    /** Caption each board at its centroid (only useful for a multi-board grid). */
    boardCaptions?: boolean;
    heightCss?: string;
  }
  let {
    cells,
    trueColor = false,
    onpaint,
    onhover,
    boardCaptions = false,
    heightCss = "min(52vh, 400px)",
  }: Props = $props();

  const S = 26; // hex radius in SVG units
  const ROT_DEG = (BOARD_ROTATION * 180) / Math.PI;
  // Unique per instance so multiple views don't share gradient/filter ids.
  const gid = "bv" + Math.random().toString(36).slice(2, 8);

  // Geometry is derived from the cells, so the view fits whatever grid it is
  // given — one board or nine — with no fixed key count anywhere.
  const centers = $derived(
    cells.map((c) => {
      const [px, py] = axialToPixel(c.x, c.y);
      return [px * S, py * S] as [number, number];
    }),
  );
  const lit = $derived(cells.map((c) => ledColor(c.color)));

  const PAD = S * 1.6; // room for the bloom to spill past the outer keys
  const box = $derived.by(() => {
    if (centers.length === 0) return { minX: 0, minY: 0, w: 1, h: 1 };
    const xs = centers.map((c) => c[0]);
    const ys = centers.map((c) => c[1]);
    const minX = Math.min(...xs) - PAD;
    const maxX = Math.max(...xs) + PAD;
    const minY = Math.min(...ys) - PAD;
    const maxY = Math.max(...ys) + PAD;
    return { minX, minY, w: maxX - minX, h: maxY - minY };
  });

  /** One caption per board, at the centroid of its keys. */
  const captions = $derived.by(() => {
    if (!boardCaptions) return [];
    const acc = new Map<string, { n: number; x: number; y: number }>();
    cells.forEach((c, i) => {
      const uid = c.boardUid ?? "";
      if (!uid) return;
      const a = acc.get(uid) ?? { n: 0, x: 0, y: 0 };
      a.n++;
      a.x += centers[i][0];
      a.y += centers[i][1];
      acc.set(uid, a);
    });
    if (acc.size < 2) return []; // a single board needs no label
    return [...acc].map(([uid, a]) => ({ uid, x: a.x / a.n, y: a.y / a.n }));
  });

  /** Hexagon vertices, rotated with the grid so the tiling stays rigid. */
  function hexPoints(cx: number, cy: number, r: number): string {
    const pts: string[] = [];
    for (let i = 0; i < 6; i++) {
      const a = ((60 * i - 90 - ROT_DEG) * Math.PI) / 180;
      pts.push(`${(cx + r * Math.cos(a)).toFixed(1)},${(cy + r * Math.sin(a)).toFixed(1)}`);
    }
    return pts.join(" ");
  }

  function fillFor(i: number): string {
    if (lit[i].off) return "var(--panel-2)";
    return trueColor ? cells[i].color : `url(#${gid}-g${i})`;
  }

  function onEnter(i: number, e: PointerEvent) {
    onhover?.(cells[i]);
    if (onpaint && (e.buttons & 1) !== 0) onpaint(cells[i]);
  }
</script>

<svg
  viewBox="{box.minX} {box.minY} {box.w} {box.h}"
  class="block w-full select-none"
  style="height: {heightCss}; touch-action: none"
  role={onpaint ? "group" : "img"}
  aria-label="Miso grid"
  onpointerleave={() => onhover?.(null)}
>
  <defs>
    <filter id="{gid}-bloom" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="5" />
    </filter>
    {#each lit as c, i}
      {#if !c.off}
        <radialGradient id="{gid}-g{i}" cx="50%" cy="38%" r="62%">
          <stop offset="0%" stop-color={c.core} />
          <stop offset="100%" stop-color={c.hue} />
        </radialGradient>
      {/if}
    {/each}
  </defs>

  <!-- Outer bloom: one filter over the whole group, so neighbouring glows merge -->
  {#if !trueColor}
    <g filter="url(#{gid}-bloom)">
      {#each centers as [cx, cy], i}
        {#if !lit[i].off}
          <polygon points={hexPoints(cx, cy, S * 1.22)} fill={lit[i].hue} opacity={lit[i].bloom} />
        {/if}
      {/each}
    </g>
  {/if}

  {#each captions as cap}
    <text
      x={cap.x}
      y={cap.y}
      text-anchor="middle"
      dominant-baseline="central"
      fill="var(--text-dim)"
      opacity="0.35"
      style="font: 600 34px 'Archivo', system-ui, sans-serif; pointer-events: none">{cap.uid}</text
    >
  {/each}

  {#each centers as [cx, cy], i}
    <polygon
      points={hexPoints(cx, cy, S * 0.93)}
      fill={fillFor(i)}
      stroke={cells[i].held ? "var(--accent)" : "var(--line)"}
      stroke-width={cells[i].held ? 3 : 1}
      style={onpaint ? "cursor: pointer" : ""}
      role={onpaint ? "button" : "presentation"}
      aria-label="Key at {cells[i].x},{cells[i].y}"
      onpointerdown={() => onpaint?.(cells[i])}
      onpointerenter={(e) => onEnter(i, e)}
    >
      <title>{cells[i].title ?? `${cells[i].x},${cells[i].y} · ${cells[i].color}`}</title>
    </polygon>
    {#if cells[i].label}
      <text
        x={cx}
        y={cy}
        text-anchor="middle"
        dominant-baseline="central"
        fill={lit[i].off || trueColor ? "var(--text-dim)" : "#000000bb"}
        style="font: 10px 'IBM Plex Mono', monospace; pointer-events: none">{cells[i].label}</text
      >
    {/if}
  {/each}
</svg>
