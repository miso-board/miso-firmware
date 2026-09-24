<script lang="ts">
  import BoardView, { type ViewCell } from "./BoardView.svelte";
  import GeneratorPanel from "./GeneratorPanel.svelte";
  import PresetBar from "./PresetBar.svelte";
  import { noteName, pitchOn } from "../lib/tuning";
  import { colorAt } from "../lib/colorMaps";
  import { swatchStyle, ledColor } from "../lib/ledColor";
  import { ui } from "../lib/ui.svelte";
  import { mesh, gridCells } from "../lib/mesh.svelte";
  import { activePreset, activeColors, paint, fillAll } from "../lib/presets.svelte";

  // Presets pre-dimmed for SK6812s — full-brightness values are blinding.
  const PRESETS = [
    "#000000", "#190202", "#190a02", "#141402", "#051905",
    "#02140f", "#050519", "#0f0219", "#190214", "#0f0f0f",
  ];
  let brush = $state("#051905");
  let customColor = $state("#00ff88");
  let customLevel = $state(12); // % brightness applied to the custom pick
  let labelMode = $state<"none" | "led" | "sensor" | "note">("none");
  let trueColor = $state(false);
  let hovered = $state<ViewCell | null>(null);

  // Note names come from the preset's PITCH mapping, not from the colour
  // generator's layout and offset. Those are independent — a preset can colour by
  // Bosanquet rows while sounding Wicki-Hayden — so labelling keys from the
  // colour side would name notes they do not play.
  const nameFor = $derived.by(() => {
    const pitch = activePreset().pitch;
    return (x: number, y: number) => noteName(pitchOn(pitch, x, y));
  });

  // One entry per key in the whole grid, positioned by absolute coordinate, so
  // a tiled set of boards renders as the single continuous instrument it is.
  const cells = $derived.by<ViewCell[]>(() => {
    const m = activeColors();
    return gridCells().map((c) => {
      const color = colorAt(m, c.x, c.y);
      const note = nameFor(c.x, c.y);
      const label =
        labelMode === "note" ? note
        : labelMode === "sensor" ? String(c.sensor)
        : labelMode === "led" ? String(c.led)
        : undefined;
      return {
        key: c.key, x: c.x, y: c.y, color, label,
        held: ui.held[c.key] === true,
        boardUid: c.board.uid,
        title: `${c.x},${c.y} · ${note} · LED ${c.led} · sensor ${c.sensor} · ${color}`
             + (mesh.boards.length > 1 ? ` · board ${c.board.uid}` : ""),
      };
    });
  });

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
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Key colors — {activePreset().name}
  </h2>
  <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
    Click or drag to paint with the selected swatch. Edits save in this browser and stream to the
    boards live while connected; pressed keys show an outline. Mappings are keyed by grid coordinate,
    so a scheme covers every attached board and survives them being rearranged. Colors are drawn as
    lit LEDs (hue at full legibility, real brightness as glow).
  </p>

  <PresetBar sends="colors" />

  <div class="mb-3 flex flex-wrap items-center gap-2">
    <span class="text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">Brush</span>
    {#each PRESETS as p}
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
    <button class="btn" onclick={() => fillAll(brush)}>Fill all</button>
    <button class="btn" onclick={() => fillAll("#000000")}>Clear</button>
    <button
      class="btn"
      style={trueColor ? "border-color: var(--accent); color: var(--accent)" : ""}
      aria-pressed={trueColor}
      onclick={() => (trueColor = !trueColor)}
    >True colors</button>
    <select class="btn" bind:value={labelMode} aria-label="Key labels">
      <option value="none">no labels</option>
      <option value="note">note names</option>
      <option value="sensor">sensor #</option>
      <option value="led">LED #</option>
    </select>
  </div>

  <BoardView
    {cells}
    {trueColor}
    boardCaptions
    onpaint={(c) => paint(c.x, c.y, brush)}
    onhover={(c) => (hovered = c)}
  />

  <p class="mono m-0 mt-1 text-xs" style="color: var(--text-dim)">
    {#if hovered !== null}
      {@const c = ledColor(hovered.color)}
      <b style="color: var(--text)">{hovered.x},{hovered.y}</b>
      {#if mesh.boards.length > 1}· board <b style="color: var(--text)">{hovered.boardUid}</b>{/if}
      · <b style="color: var(--text)">{c.hex}</b>
      · rgb({c.rgb.join(", ")})
      · <b style="color: var(--text)">{Math.round(c.intensity * 100)}%</b> brightness
      · <b style="color: var(--accent)">{nameFor(hovered.x, hovered.y)}</b>
    {:else}
      Hover a key for its exact stored value.
    {/if}
  </p>

  {#if !ui.connected}
    <p class="m-0 mt-2 text-xs" style="color: var(--warn)">
      Not connected — painting and saving work, but colors reach the board once you connect.
    </p>
  {/if}
</section>

<GeneratorPanel />

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
