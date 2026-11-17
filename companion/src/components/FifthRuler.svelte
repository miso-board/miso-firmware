<script lang="ts">
  // A ruler for picking the width of the fifth by eye: click anywhere to put
  // the marker there, or hold and drag to sweep the tuning continuously.
  //
  // It is a second view of the same number as the "fifth in cents" box, not a
  // second source of truth: it draws whatever `value` it is given and reports
  // changes through `onchange`, so the two can never disagree.

  import { onMount } from "svelte";
  import { FIFTH_MIN, FIFTH_MAX } from "../lib/tuning";

  interface Props {
    /** The fifth, in cents. */
    value: number;
    onchange: (cents: number) => void;
    /** Named fifths to point out along the bottom edge, e.g. the common EDOs. */
    marks?: { cents: number; label: string }[];
  }
  let { value, onchange, marks = [] }: Props = $props();

  const MARKS_H = 15; // room under the ruler for the marks' labels, when there are any
  const HEIGHT = $derived(62 + (marks.length ? MARKS_H : 0)); // CSS px
  const PAD = 10; // side margin, so the marker is not clipped at the endpoints
  const LABEL_Y = 10; // baseline row for the cent numbers
  const RULER_TOP = 26; // the marker's tip touches this edge
  const RULER_BOTTOM = 58;
  const MIN_TICK_GAP = 10; // CSS px between the finest gradations

  let canvas: HTMLCanvasElement;
  let width = $state(0);
  let dragging = $state(false);
  // Bumped when the colour scheme changes, to redraw with the new tokens.
  let themeTick = $state(0);

  const span = FIFTH_MAX - FIFTH_MIN;
  const xOf = (cents: number) => PAD + ((cents - FIFTH_MIN) / span) * (width - 2 * PAD);

  function centsAt(clientX: number): number {
    const rect = canvas.getBoundingClientRect();
    const t = (clientX - rect.left - PAD) / (rect.width - 2 * PAD);
    const cents = FIFTH_MIN + Math.min(1, Math.max(0, t)) * span;
    // To the millicent, which is what the input box shows and the board is sent.
    // The lower endpoint is not a round millicent, so clamp again after rounding.
    return Math.min(FIFTH_MAX, Math.max(FIFTH_MIN, Math.round(cents * 1000) / 1000));
  }

  function draw() {
    const ctx = canvas?.getContext("2d");
    if (!ctx || width <= 0) return;
    const dpr = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(width * dpr) || canvas.height !== Math.round(HEIGHT * dpr)) {
      canvas.width = Math.round(width * dpr);
      canvas.height = Math.round(HEIGHT * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, HEIGHT);

    const css = getComputedStyle(canvas);
    const token = (name: string) => css.getPropertyValue(name).trim();
    const line = token("--line");
    const dim = token("--text-dim");
    const text = token("--text");
    const marker = token("--crit");

    // The ruler body.
    ctx.fillStyle = token("--panel-2");
    ctx.strokeStyle = line;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.rect(PAD + 0.5, RULER_TOP + 0.5, width - 2 * PAD - 1, RULER_BOTTOM - RULER_TOP - 1);
    ctx.fill();
    ctx.stroke();

    // Gradations: the finest whole-cent step that still leaves room to read.
    const pxPerCent = (width - 2 * PAD) / span;
    const step = [1, 2, 5, 10].find((s) => s * pxPerCent >= MIN_TICK_GAP) ?? 10;
    const bodyH = RULER_BOTTOM - RULER_TOP;
    const tick = (cents: number, len: number, colour: string) => {
      const x = Math.round(xOf(cents)) + 0.5;
      ctx.strokeStyle = colour;
      ctx.beginPath();
      ctx.moveTo(x, RULER_TOP);
      ctx.lineTo(x, RULER_TOP + len);
      ctx.stroke();
    };
    for (let c = Math.ceil(FIFTH_MIN / step) * step; c <= FIFTH_MAX; c += step) {
      if (c % 10 === 0) tick(c, bodyH * 0.62, text);
      else if (c % 5 === 0) tick(c, bodyH * 0.42, dim);
      else tick(c, bodyH * 0.24, dim);
    }
    // The lower endpoint is not a round number, so it gets a tick of its own.
    tick(FIFTH_MIN, bodyH * 0.62, text);

    // Cent numbers over every round 10¢, plus the lower endpoint.
    ctx.font = '10px "IBM Plex Mono", ui-monospace, monospace';
    ctx.textBaseline = "alphabetic";
    ctx.fillStyle = dim;
    const GAP = 5;
    const tens: { label: string; left: number; w: number }[] = [];
    for (let c = Math.ceil(FIFTH_MIN / 10) * 10; c <= FIFTH_MAX; c += 10) {
      const label = `${c}¢`;
      const w = ctx.measureText(label).width;
      // Centred on its tick, but kept inside the canvas at the right-hand end.
      tens.push({ label, w, left: Math.min(xOf(c) - w / 2, width - w - 1) });
    }
    ctx.textAlign = "left";
    let taken = -Infinity; // right edge of the last label drawn
    // The endpoint sits only 4.3¢ from 690, so its label gives way first: in
    // full if there is room before 690's, else to one decimal, else not at all.
    const endLabel = [FIFTH_MIN.toFixed(3), FIFTH_MIN.toFixed(1)]
      .map((t) => `${t}¢`)
      .find((t) => PAD + ctx.measureText(t).width + GAP <= (tens[0]?.left ?? Infinity));
    if (endLabel) {
      ctx.fillText(endLabel, PAD, LABEL_Y);
      taken = PAD + ctx.measureText(endLabel).width + GAP;
    }
    for (const t of tens) {
      if (t.left < taken) continue; // no room beside the previous label
      ctx.fillText(t.label, t.left, LABEL_Y);
      taken = t.left + t.w + GAP;
    }

    // Named fifths: a tick up from the bottom edge and a label underneath, in
    // the accent colour so they read as a different kind of thing from cents.
    if (marks.length) {
      const accent = token("--accent");
      ctx.strokeStyle = accent;
      ctx.fillStyle = accent;
      ctx.lineWidth = 1.5;
      ctx.textBaseline = "alphabetic";
      let takenTo = -Infinity;
      for (const m of [...marks].sort((a, b) => a.cents - b.cents)) {
        if (m.cents < FIFTH_MIN || m.cents > FIFTH_MAX) continue;
        const x = xOf(m.cents);
        ctx.beginPath();
        ctx.moveTo(x, RULER_BOTTOM);
        ctx.lineTo(x, RULER_BOTTOM - bodyH * 0.3);
        ctx.stroke();
        const w = ctx.measureText(m.label).width;
        const left = Math.min(Math.max(x - w / 2, 0), width - w);
        if (left < takenTo) continue; // no room beside the previous label
        ctx.fillText(m.label, left, RULER_BOTTOM + 12);
        takenTo = left + w + 3;
      }
      ctx.lineWidth = 1;
    }

    // The marker: a triangle whose tip rests on the ruler's top edge, and a
    // line from that tip across to the far side.
    const mx = xOf(Math.min(FIFTH_MAX, Math.max(FIFTH_MIN, value)));
    ctx.fillStyle = marker;
    ctx.strokeStyle = marker;
    ctx.beginPath();
    ctx.moveTo(mx, RULER_TOP);
    ctx.lineTo(mx - 5.5, RULER_TOP - 9);
    ctx.lineTo(mx + 5.5, RULER_TOP - 9);
    ctx.closePath();
    ctx.fill();
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(mx, RULER_TOP);
    ctx.lineTo(mx, RULER_BOTTOM);
    ctx.stroke();
  }

  $effect(() => {
    // Named so the effect tracks them; draw() reads the rest from the DOM.
    void value;
    void marks;
    void width;
    void themeTick;
    draw();
  });

  onMount(() => {
    const ro = new ResizeObserver(() => (width = canvas.clientWidth));
    ro.observe(canvas);
    width = canvas.clientWidth;
    const scheme = window.matchMedia("(prefers-color-scheme: light)");
    const onScheme = () => themeTick++;
    scheme.addEventListener("change", onScheme);
    // The label font may arrive after the first paint.
    void document.fonts?.ready.then(() => themeTick++);
    return () => {
      ro.disconnect();
      scheme.removeEventListener("change", onScheme);
    };
  });

  function onDown(e: PointerEvent) {
    if (e.button !== 0) return;
    canvas.setPointerCapture(e.pointerId);
    dragging = true;
    onchange(centsAt(e.clientX));
  }
  function onMove(e: PointerEvent) {
    if (dragging) onchange(centsAt(e.clientX));
  }
  function onUp(e: PointerEvent) {
    dragging = false;
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
  }

  function onKey(e: KeyboardEvent) {
    const dir = e.key === "ArrowRight" || e.key === "ArrowUp" ? 1
      : e.key === "ArrowLeft" || e.key === "ArrowDown" ? -1 : 0;
    if (dir === 0) return;
    e.preventDefault();
    const next = value + dir * (e.shiftKey ? 1 : 0.1);
    onchange(Math.min(FIFTH_MAX, Math.max(FIFTH_MIN, Math.round(next * 1000) / 1000)));
  }
</script>

<canvas
  bind:this={canvas}
  class="block w-full select-none"
  style="height: {HEIGHT}px; touch-action: none; cursor: {dragging ? 'ew-resize' : 'crosshair'}"
  role="slider"
  tabindex="0"
  aria-label="Width of the fifth in cents"
  aria-valuemin={FIFTH_MIN}
  aria-valuemax={FIFTH_MAX}
  aria-valuenow={value}
  aria-valuetext="{value.toFixed(3)} cents"
  onpointerdown={onDown}
  onpointermove={onMove}
  onpointerup={onUp}
  onpointercancel={onUp}
  onkeydown={onKey}
></canvas>
