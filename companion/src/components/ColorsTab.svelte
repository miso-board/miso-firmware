<script lang="ts">
  import BoardView from "./BoardView.svelte";
  import GeneratorPanel from "./GeneratorPanel.svelte";
  import { LED_FOR_SENSOR, SENSOR_FOR_LED, LED_POS, NUM_KEYS } from "../lib/layout";
  import { LAYOUTS, pitchAt, noteName } from "../lib/tuning";
  import { swatchStyle, ledColor } from "../lib/ledColor";
  import { ui, toast } from "../lib/ui.svelte";
  import {
    colorState, activeMap, paint, fillAll, newMap, duplicateMap, renameMap,
    deleteMap, selectMap, pushToBoard, copyMapJson, importMapJson,
  } from "../lib/colorMaps.svelte";

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
  let hovered = $state<number | null>(null);

  const heldLed = $derived.by(() => {
    const out = Array(NUM_KEYS).fill(false);
    ui.held.forEach((h, s) => (out[LED_FOR_SENSOR[s]] = h));
    return out;
  });

  // Note names follow the active mapping's layout; hand-painted mappings fall
  // back to the Bosanquet default so the labels always mean something.
  const noteNames = $derived.by(() => {
    const gen = activeMap().generator;
    const layout = LAYOUTS[gen?.layout ?? "bosanquet"];
    const offset = gen?.offset ?? ([0, 0] as [number, number]);
    return LED_POS.map(([x, y]) => noteName(pitchAt(layout, x, y, offset)));
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

  function importPrompt() {
    const text = window.prompt("Paste a mapping JSON (miso-colors-1):");
    if (text && !importMapJson(text)) toast("Couldn't parse that mapping JSON.");
    else if (text) toast("Mapping imported.");
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Key colors — {activeMap().name}
  </h2>
  <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
    Click or drag across the board to paint with the selected swatch. Edits save in this browser and
    stream to the board live while connected; pressed keys show an outline. Octaves run horizontally;
    colors are drawn as lit LEDs (hue at full legibility, real brightness as glow).
  </p>

  <div class="mb-3 flex flex-wrap items-center gap-2">
    <select class="btn" value={colorState.active} onchange={(e) => selectMap(+e.currentTarget.value)}>
      {#each colorState.maps as m, i}<option value={i}>{m.name}</option>{/each}
    </select>
    <input
      class="btn"
      style="width: 150px"
      value={activeMap().name}
      onchange={(e) => renameMap(e.currentTarget.value || "Untitled")}
      aria-label="Mapping name"
    />
    <button class="btn" onclick={newMap}>New</button>
    <button class="btn" onclick={duplicateMap}>Duplicate</button>
    <button class="btn" onclick={deleteMap}>Delete</button>
    <div class="flex-1"></div>
    <button class="btn" onclick={copyMapJson}>Copy JSON</button>
    <button class="btn" onclick={importPrompt}>Import…</button>
    <button
      class="btn primary"
      disabled={!ui.connected}
      onclick={() => { void pushToBoard(); toast("Colors sent to board."); }}
    >Send to board</button>
  </div>

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
    colors={activeMap().colors}
    {heldLed}
    labels={labelMode}
    {noteNames}
    {trueColor}
    onpaint={(led) => paint(led, brush)}
    onhover={(led) => (hovered = led)}
  />

  <p class="mono m-0 mt-1 text-xs" style="color: var(--text-dim)">
    {#if hovered !== null}
      {@const c = ledColor(activeMap().colors[hovered])}
      LED <b style="color: var(--text)">{String(hovered).padStart(2, "0")}</b>
      · sensor <b style="color: var(--text)">{String(SENSOR_FOR_LED[hovered]).padStart(2, "0")}</b>
      · <b style="color: var(--text)">{c.hex}</b>
      · rgb({c.rgb.join(", ")})
      · <b style="color: var(--text)">{Math.round(c.intensity * 100)}%</b> brightness
      · <b style="color: var(--accent)">{noteNames[hovered]}</b>
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
  .btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-ink); font-weight: 600; }
</style>
