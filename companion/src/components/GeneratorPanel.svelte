<script lang="ts">
  import { LAYOUTS, accidentalLabel, noteName, pitchAt, CENTRE_KEY, type LayoutId } from "../lib/tuning";
  import { swatchStyle } from "../lib/ledColor";
  import { DEFAULT_GENERATOR, type GeneratorParams } from "../lib/colorMaps";
  import {
    activeColors, createProceduralColorMap, updateColorGenerator,
  } from "../lib/presets.svelte";

  const updateGenerator = (patch: Partial<GeneratorParams>) => updateColorGenerator(patch);

  const gen = $derived(activeColors().generator);
  const layout = $derived(gen ? LAYOUTS[gen.layout] : LAYOUTS.bosanquet);
  // Which note the colour rows are reckoned from at the board's centre key.
  // This follows the COLOUR offset, not the preset's pitch mapping — the two are
  // independent, so it is labelled as a colour reference rather than as the note
  // the key sounds. The Pitch mapping tab is where sounding pitch lives.
  const colourRefNote = $derived(
    gen ? noteName(pitchAt(layout, CENTRE_KEY[0], CENTRE_KEY[1], gen.offset)) : "",
  );

  function setPaletteColor(i: number, hex: string) {
    if (!gen) return;
    const palette = [...gen.palette];
    palette[i] = hex;
    updateGenerator({ palette });
  }

  function addRow() {
    if (!gen) return;
    updateGenerator({ palette: [...gen.palette, "#888888"] });
  }

  function removeRow(i: number) {
    if (!gen || gen.palette.length <= 1) return;
    updateGenerator({ palette: gen.palette.filter((_: string, j: number) => j !== i) });
  }

  function nudge(axis: 0 | 1, delta: number) {
    if (!gen) return;
    const offset: [number, number] = [gen.offset[0], gen.offset[1]];
    offset[axis] += delta;
    updateGenerator({ offset });
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Procedural rows
  </h2>

  {#if !gen}
    <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
      Colour every key by the accidental of the note that lands on it — the key's Bosanquet row —
      computed with <b>meantonal</b>. Rows beyond your palette wrap back through it.
      This preset's colours were painted by hand, so generating replaces them.
    </p>
    <button class="btn primary" onclick={() => createProceduralColorMap()}>Generate colour rows</button>
  {:else}
    <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
      Each palette row is one accidental; rows past the end wrap back to the start.
      <span class="mono">{layout.axes}</span>
    </p>

    <div class="flex flex-wrap items-start gap-x-6 gap-y-3">
      <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Layout
        <select
          class="btn text-sm normal-case tracking-normal"
          value={gen.layout}
          onchange={(e) => updateGenerator({ layout: e.currentTarget.value as LayoutId })}
        >
          {#each Object.values(LAYOUTS) as l}<option value={l.id}>{l.name}</option>{/each}
        </select>
      </label>

      <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        First row is
        <select
          class="btn text-sm normal-case tracking-normal"
          value={gen.startAccidental}
          onchange={(e) => updateGenerator({ startAccidental: +e.currentTarget.value })}
        >
          {#each [-4, -3, -2, -1, 0, 1, 2, 3, 4] as a}
            <option value={a}>{accidentalLabel(a)}</option>
          {/each}
        </select>
      </label>

      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Offset
        <div class="flex items-center gap-1">
          {#each [0, 1] as axis}
            <span class="mono ml-1 text-xs normal-case" style="color: var(--text-dim)">{axis === 0 ? "x" : "y"}</span>
            <button class="btn px-2 py-1" aria-label="decrease" onclick={() => nudge(axis as 0 | 1, -1)}>−</button>
            <span class="mono w-6 text-center text-sm" style="color: var(--text)">{gen.offset[axis]}</span>
            <button class="btn px-2 py-1" aria-label="increase" onclick={() => nudge(axis as 0 | 1, 1)}>+</button>
          {/each}
        </div>
      </div>

      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Colour reference
        <span class="mono text-base" style="color: var(--accent)" title="Which note the colour rows are reckoned from — not necessarily what the key sounds">{colourRefNote}</span>
      </div>

      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Landmark
        <button
          class="btn text-sm normal-case tracking-normal"
          style={gen.lightenCDE ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={gen.lightenCDE ?? false}
          title="Tint the keys whose letter is C, D or E paler"
          onclick={() => updateGenerator({ lightenCDE: !gen.lightenCDE })}
        >Lighten C D E</button>
      </div>

      <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        LED brightness <span class="mono normal-case" style="color: var(--text)">{gen.brightness}%</span>
        <input
          type="range" min="1" max="60" value={gen.brightness}
          oninput={(e) => updateGenerator({ brightness: +e.currentTarget.value })}
          class="w-32"
        />
      </label>
    </div>

    <div class="mt-4 flex flex-wrap items-end gap-2">
      {#each gen.palette as hex, i}
        <div class="flex flex-col items-center gap-1">
          <span class="text-xs" style="color: var(--text-dim)">{accidentalLabel(gen.startAccidental + i)}</span>
          <div class="relative">
            <span class="block h-8 w-8 rounded-md border" style="{swatchStyle(hex)}; border-color: var(--line)"></span>
            <input
              type="color" value={hex}
              oninput={(e) => setPaletteColor(i, e.currentTarget.value)}
              class="absolute inset-0 h-8 w-8 cursor-pointer opacity-0"
              aria-label="Row {i + 1} colour"
            />
          </div>
          <button
            class="text-xs" style="color: var(--text-dim); background: none; border: none; cursor: pointer"
            disabled={gen.palette.length <= 1}
            onclick={() => removeRow(i)}
            aria-label="Remove row {i + 1}"
          >✕</button>
        </div>
      {/each}
      <button class="btn mb-6" onclick={addRow}>+ row</button>
      <div class="flex-1"></div>
      <button class="btn mb-6" onclick={() => updateGenerator(DEFAULT_GENERATOR)}>Reset to default</button>
    </div>
  {/if}
</section>

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
