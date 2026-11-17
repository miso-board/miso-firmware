<script lang="ts">
  // The keyboard itself, key by key: what every key looks like AND what it
  // plays, with a brush for painting individual keys over the generated colours.
  //
  // The two halves are orthogonal — colour is the hex's fill, pitch is its label
  // — so they share one board. Painting acts on the fill; the label and the
  // hover readout come from the pitch mapping. A tiled set of boards shows as
  // one instrument, each key at its absolute coordinate.
  //
  // It sits at the bottom of the tab: everything above it sets up the whole
  // instrument at once, and this is for the occasional exception.

  import BoardView, { type ViewCell } from "./BoardView.svelte";
  import {
    noteName, pitchOn, isOverridden, midiFor, hzAt, tuningError, referenceOf,
  } from "../lib/tuning";
  import { colorAt } from "../lib/colorMaps";
  import { swatchStyle, ledColor } from "../lib/ledColor";
  import { ui } from "../lib/ui.svelte";
  import { mesh, gridCells } from "../lib/mesh.svelte";
  import {
    activePreset, activeColors, paint, fillAll, paintedCount, resetColors,
  } from "../lib/presets.svelte";

  // Swatches pre-dimmed for SK6812s — full-brightness values are blinding.
  const SWATCHES = [
    "#000000", "#190202", "#190a02", "#141402", "#051905",
    "#02140f", "#050519", "#0f0219", "#190214", "#0f0f0f",
  ];
  let brush = $state("#051905");
  let customColor = $state("#00ff88");
  let customLevel = $state(12); // % brightness applied to the custom pick
  let labelMode = $state<"note" | "midi" | "cents" | "hz" | "sensor" | "led" | "none">("note");
  let trueColor = $state(false);
  let hovered = $state<ViewCell | null>(null);

  const pitch = $derived(activePreset().pitch);
  const gen = $derived(pitch.generator);
  // An unusable tuning must not take the board away — colours can still be
  // painted — so it only blanks the pitch-derived labels and readouts.
  const invalid = $derived(tuningError(gen.tuning) !== null);

  interface Sounding {
    name: string;
    note: number;
    bend: number;
    cents: number;
    hz: number;
    overridden: boolean;
  }
  interface Row {
    cell: ViewCell;
    /** What the key plays; null while the tuning is invalid. */
    sound: Sounding | null;
  }

  // One entry per key in the whole grid.
  const rows = $derived.by<Row[]>(() => {
    const colors = activeColors();
    const t = gen.tuning;
    const ref = referenceOf(gen);
    return gridCells().map((c) => {
      const color = colorAt(colors, gen, c.x, c.y);
      let sound: Sounding | null = null;
      if (!invalid) {
        const p = pitchOn(pitch, c.x, c.y);
        const m = midiFor(p, t, gen.bendSemitones, ref);
        sound = {
          name: noteName(p), note: m.note, bend: m.bend, cents: m.cents,
          hz: hzAt(t, p, ref), overridden: isOverridden(pitch, c.x, c.y),
        };
      }
      const label =
        labelMode === "sensor" ? String(c.sensor)
        : labelMode === "led" ? String(c.led)
        : labelMode === "none" || !sound ? undefined
        : labelMode === "note" ? sound.name
        : labelMode === "midi" ? String(sound.note)
        : labelMode === "cents" ? sound.cents.toFixed(0)
        : Math.round(sound.hz).toString();
      return {
        cell: {
          key: c.key, x: c.x, y: c.y, color, label,
          held: ui.held[c.key] === true,
          boardUid: c.board.uid,
          title: `${c.x},${c.y}`
               + (sound
                 ? ` · ${sound.name} · MIDI ${sound.note} bend ${sound.bend} (${sound.cents.toFixed(1)}¢)`
                   + ` · ${sound.hz.toFixed(2)} Hz`
                 : "")
               + ` · ${color} · LED ${c.led} · sensor ${c.sensor}`
               + (sound?.overridden ? " · pitch set by hand" : "")
               + (mesh.boards.length > 1 ? ` · board ${c.board.uid}` : ""),
        },
        sound,
      };
    });
  });

  const cells = $derived(rows.map((r) => r.cell));
  const sounding = $derived(rows.flatMap((r) => (r.sound ? [r.sound] : [])));

  /** Distinct sounding pitches. Fewer than the key count whenever the tuning
   *  tempers out the interval between two spellings — in 12-TET the 31 keys
   *  sound only 13 pitches, because C♯ and D♭ coincide. */
  const distinctPitches = $derived(new Set(sounding.map((s) => Math.round(s.note * 100 + s.cents))).size);

  // Any hand-painted key makes the colours "custom"; none means every key is
  // still coloured by the generator.
  const painted = $derived(paintedCount());

  const overrideCount = $derived(Object.keys(pitch.overrides).length);
  const hoveredRow = $derived(hovered ? rows.find((r) => r.cell.key === hovered!.key) : undefined);

  function useCustom() {
    const v = parseInt(customColor.slice(1), 16);
    const k = customLevel / 100;
    const scale = (c: number) => Math.round(c * k);
    brush =
      "#" +
      [scale((v >> 16) & 0xff), scale((v >> 8) & 0xff), scale(v & 0xff)]
        .map((c) => c.toString(16).padStart(2, "0"))
        .join("");
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-2.5 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Keys
  </h2>
  <div class="mb-3 flex flex-wrap items-center gap-2">
    <span class="text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">Brush</span>
    {#each SWATCHES as p}
      <button
        class="h-7 w-7 rounded-md border-2"
        style="{swatchStyle(p)}; border-color: {brush === p ? 'var(--accent)' : 'var(--line)'}"
        aria-label="Swatch {p}"
        onclick={() => (brush = p)}
      ></button>
    {/each}
    <label class="ml-2 flex items-center gap-1.5 text-xs" style="color: var(--text-dim)">
      custom
      <input type="color" bind:value={customColor} onchange={useCustom} class="h-7 w-9 cursor-pointer border-0 bg-transparent p-0" />
    </label>
    <label class="flex items-center gap-1.5 text-xs" style="color: var(--text-dim)">
      @ <input type="range" min="2" max="100" bind:value={customLevel} onchange={useCustom} class="w-20" />
      <span class="mono">{customLevel}%</span>
    </label>
    <span class="h-7 w-7 rounded-md border" style="{swatchStyle(brush)}; border-color: var(--accent-dim)" title="Active brush"></span>
    <div class="flex-1"></div>
    {#if painted > 0}
      <span class="text-xs" style="color: var(--accent)">Custom colours</span>
      <button class="btn" onclick={resetColors}>Reset to generated colours</button>
    {/if}
    <button class="btn" onclick={() => fillAll(brush)}>Fill all</button>
    <button class="btn" onclick={() => fillAll("#000000")}>Clear</button>
    <button
      class="btn"
      style={trueColor ? "border-color: var(--accent); color: var(--accent)" : ""}
      aria-pressed={trueColor}
      onclick={() => (trueColor = !trueColor)}
    >True colors</button>
    <select class="btn" bind:value={labelMode} aria-label="Key labels">
      <option value="note">note names</option>
      <option value="midi">MIDI number</option>
      <option value="cents">bend cents</option>
      <option value="hz">frequency</option>
      <option value="sensor">sensor #</option>
      <option value="led">LED #</option>
      <option value="none">no labels</option>
    </select>
  </div>

  <BoardView
    {cells}
    {trueColor}
    boardCaptions
    onpaint={(c) => paint(c.x, c.y, brush)}
    onhover={(c) => (hovered = c)}
  />

  <div class="mt-1 flex flex-wrap items-baseline gap-x-4 gap-y-1">
    <p class="mono m-0 text-xs" style="color: var(--text-dim)">
      {#if hoveredRow}
        {@const c = ledColor(hoveredRow.cell.color)}
        {@const s = hoveredRow.sound}
        <b style="color: var(--text)">{hoveredRow.cell.x},{hoveredRow.cell.y}</b>
        {#if mesh.boards.length > 1}· board <b style="color: var(--text)">{hoveredRow.cell.boardUid}</b>{/if}
        {#if s}
          · <b style="color: var(--accent)">{s.name}</b>
          · MIDI <b style="color: var(--text)">{s.note}</b>
          bend <b style="color: var(--text)">{s.bend}</b>
          ({s.cents.toFixed(1)}¢)
          · <b style="color: var(--text)">{s.hz.toFixed(2)} Hz</b>
          {#if s.overridden}· <b style="color: var(--warn)">pitch set by hand</b>{/if}
        {/if}
        · <b style="color: var(--text)">{c.hex}</b>
        · <b style="color: var(--text)">{Math.round(c.intensity * 100)}%</b> brightness
      {:else}
        Hover a key for its note, MIDI number, bend, frequency and stored colour.
      {/if}
    </p>
    <div class="flex-1"></div>
    {#if !invalid}
      <span class="mono text-xs" style="color: var(--text-dim)">
        {rows.length} keys · {distinctPitches} distinct pitches
        {#if overrideCount > 0}· {overrideCount} set by hand{/if}
      </span>
    {/if}
  </div>

  {#if invalid}
    <p class="m-0 mt-2 text-xs" style="color: var(--crit)">
      This tuning can't be rendered, so keys aren't labelled with pitches — fix it under Tuning below.
    </p>
  {/if}
</section>

{#if overrideCount > 0}
  <section
    class="mb-3.5 rounded-lg border p-4"
    style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
  >
    <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
      Pitches set by hand
    </h2>
    <p class="m-0 mb-2 text-xs" style="color: var(--text-dim)">
      These came from an imported preset. They override the generator in this app, but the board is
      retuned by parameters rather than a per-key table, so it still plays the generated pitch for
      them — per-key editing lands with the firmware command that can carry it.
    </p>
    <ul class="mono m-0 flex list-none flex-wrap gap-x-4 gap-y-0.5 p-0 text-xs">
      {#each rows.filter((r) => r.sound?.overridden) as r}
        <li style="color: var(--text-dim)">
          <b style="color: var(--text)">{r.cell.x},{r.cell.y}</b> → {r.sound?.name}
        </li>
      {/each}
    </ul>
  </section>
{/if}

<style>
  .btn {
    font: inherit;
    color: var(--text);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 6px 12px;
    cursor: pointer;
  }
  .btn:hover:not(:disabled) { border-color: var(--accent-dim); }
  .btn:disabled { opacity: 0.45; cursor: default; }
</style>
