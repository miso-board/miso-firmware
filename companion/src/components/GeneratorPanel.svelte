<script lang="ts">
  // What the colour rows look like. Where they fall is not set here: a key is
  // coloured by where the note the pitch mapping puts on it sits on the chain
  // of fifths, so the rows always line up with what the instrument plays.
  //
  // Two ways to cut the chain up: by accidental (equal slices of seven), or as
  // a hand-built list of partitions of any widths.

  import { LAYOUTS, accidentalLabel } from "../lib/tuning";
  import { swatchStyle } from "../lib/ledColor";
  import {
    FIFTHS_LETTERS, PARTITIONS_MAX, PARTITION_WIDTH_MAX,
    COLORGEN_PART_MAX, COLORGEN_PAL_MAX,
    defaultGenerator, fifthsName, partitionStarts, partitionsFromAccidentals,
    type GeneratorParams, type Partition,
  } from "../lib/colorMaps";
  import {
    activeColors, activePitch, updateColorGenerator, paintedCount, colorGenerationLimit,
    COLORGEN_PARTITIONS_FW,
  } from "../lib/presets.svelte";
  import { ui } from "../lib/ui.svelte";

  const updateGenerator = (patch: Partial<GeneratorParams>) => updateColorGenerator(patch);

  const gen = $derived(activeColors().generator);
  const layoutName = $derived(LAYOUTS[activePitch().generator.layout].name);
  const painted = $derived(paintedCount());
  const byPartition = $derived(gen.scheme === "partitions" && gen.partitions.length > 0);

  // --- by accidental ---

  function setPaletteColor(i: number, hex: string) {
    const palette = [...gen.palette];
    palette[i] = hex;
    updateGenerator({ palette });
  }

  function addRow() {
    updateGenerator({ palette: [...gen.palette, "#888888"] });
  }

  function removeRow(i: number) {
    if (gen.palette.length <= 1) return;
    updateGenerator({ palette: gen.palette.filter((_: string, j: number) => j !== i) });
  }

  // --- by partition ---

  const starts = $derived(byPartition ? partitionStarts(gen) : []);
  const totalWidth = $derived(gen.partitions.reduce((sum, p) => sum + p.width, 0));
  // Whether the boards can generate this themselves, or need it sent key by key.
  const limit = $derived(colorGenerationLimit());

  /** The notes a partition covers, e.g. "F – B" or "F♯ – A♯". */
  function span(i: number): string {
    const from = starts[i];
    const to = from + gen.partitions[i].width - 1;
    return from === to ? fifthsName(from) : `${fifthsName(from)} – ${fifthsName(to)}`;
  }

  function usePartitions() {
    // First time in: start from the accidental rows, said as partitions, so
    // switching scheme changes nothing on the keys until something is edited.
    const seed = gen.partitions.length > 0 ? {} : partitionsFromAccidentals(gen);
    updateGenerator({ scheme: "partitions", ...seed });
  }

  function setPartitions(partitions: Partition[], anchorPartition = gen.anchorPartition) {
    updateGenerator({
      partitions,
      anchorPartition: Math.min(Math.max(0, anchorPartition), partitions.length - 1),
    });
  }

  function setPartition(i: number, patch: Partial<Partition>) {
    setPartitions(gen.partitions.map((p, j) => (j === i ? { ...p, ...patch } : p)));
  }

  function setWidth(i: number, raw: string) {
    const v = Math.round(Number(raw));
    if (!Number.isFinite(v)) return;
    setPartition(i, { width: Math.min(PARTITION_WIDTH_MAX, Math.max(1, v)) });
  }

  function addPartition() {
    if (gen.partitions.length >= PARTITIONS_MAX) return;
    setPartitions([...gen.partitions, { width: 7, color: "#888888" }]);
  }

  function removePartition(i: number) {
    if (gen.partitions.length <= 1) return;
    // Keep the anchor on the partition it was on, where that one survives.
    const anchor = gen.anchorPartition > i ? gen.anchorPartition - 1 : gen.anchorPartition;
    setPartitions(gen.partitions.filter((_, j) => j !== i), anchor);
  }

  /** Seven naturals then five sharps, repeating every twelve fifths. */
  function sevenFive() {
    // Reuse the colours already in play: the naturals' and the sharps'.
    const colourOf = (accidental: number, fallback: string) =>
      gen.palette[accidental - gen.startAccidental] ?? fallback;
    updateGenerator({
      scheme: "partitions",
      partitions: [
        { width: 7, color: colourOf(0, "#c8c814") },
        { width: 5, color: colourOf(1, "#3232fa") },
      ],
      anchorLetter: 0,
      anchorPartition: 0,
    });
  }
</script>

<section
  class="rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Colour rows
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    Every key is coloured by where the note it plays sits on the chain of fifths, so the rows follow
    the <b style="color: var(--text)">{layoutName}</b> layout.
  </p>

  {#if painted > 0}
    <p class="m-0 mb-3 rounded-md border-l-[3px] px-3 py-2 text-xs"
       style="background: var(--panel-2); border-color: var(--accent); color: var(--text-dim)">
      <b style="color: var(--text)">Custom colours</b> — {painted} key{painted === 1 ? "" : "s"}
      painted by hand. These settings still colour the rest; <b>Reset to generated colours</b> above
      the keyboard discards the painting.
    </p>
  {/if}

  <div class="flex flex-wrap items-start gap-x-6 gap-y-3">
    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Colour by
      <div class="flex items-center gap-1">
        <button
          class="btn text-sm normal-case tracking-normal"
          style={!byPartition ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={!byPartition}
          title="Equal slices of seven fifths: the naturals, the sharps, the flats…"
          onclick={() => updateGenerator({ scheme: "accidental" })}
        >Accidental</button>
        <button
          class="btn text-sm normal-case tracking-normal"
          style={byPartition ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={byPartition}
          title="Slices of the chain of fifths of any widths"
          onclick={usePartitions}
        >Partitions</button>
      </div>
    </div>

    {#if !byPartition}
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
    {:else}
      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Starting note
        <div class="flex items-center gap-1.5 text-sm normal-case tracking-normal">
          <select
            class="btn"
            value={gen.anchorLetter}
            aria-label="The natural that starts a partition"
            onchange={(e) => updateGenerator({ anchorLetter: +e.currentTarget.value })}
          >
            {#each FIFTHS_LETTERS as letter, i}<option value={i}>{letter}</option>{/each}
          </select>
          <span class="text-xs">starts partition</span>
          <select
            class="btn"
            value={gen.anchorPartition}
            aria-label="Which partition it starts"
            onchange={(e) => updateGenerator({ anchorPartition: +e.currentTarget.value })}
          >
            {#each gen.partitions as _, i}<option value={i}>{i + 1}</option>{/each}
          </select>
        </div>
      </div>
    {/if}

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

  {#if !byPartition}
    <p class="m-0 mt-4 text-xs" style="color: var(--text-dim)">
      Each row is one accidental — seven adjacent fifths, F to B. Rows past the end wrap back to the
      start.
    </p>
    <div class="mt-2 flex flex-wrap items-end gap-2">
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
    </div>
  {:else}
    <p class="m-0 mt-4 text-xs" style="color: var(--text-dim)">
      Each partition is a run of adjacent fifths (… B♭ F C G D A E B F♯ …) and colours every note in
      it. Together they span <b class="mono" style="color: var(--text)">{totalWidth}</b>
      fifth{totalWidth === 1 ? "" : "s"}, then repeat.
    </p>
    <div class="mt-2 flex flex-wrap items-stretch gap-2">
      {#each gen.partitions as part, i}
        <div class="partition" class:anchor={i === gen.anchorPartition}>
          <div class="flex items-center gap-2">
            <div class="relative">
              <span class="block h-8 w-8 rounded-md border" style="{swatchStyle(part.color)}; border-color: var(--line)"></span>
              <input
                type="color" value={part.color}
                oninput={(e) => setPartition(i, { color: e.currentTarget.value })}
                class="absolute inset-0 h-8 w-8 cursor-pointer opacity-0"
                aria-label="Partition {i + 1} colour"
              />
            </div>
            <label class="flex items-center gap-1 text-xs" style="color: var(--text-dim)">
              <input
                class="btn mono text-sm"
                style="width: 56px; padding: 4px 6px"
                type="number" min="1" max={PARTITION_WIDTH_MAX} step="1"
                value={part.width}
                onchange={(e) => setWidth(i, e.currentTarget.value)}
                aria-label="Partition {i + 1} width in fifths"
              />
              fifths
            </label>
            <button
              class="text-xs" style="color: var(--text-dim); background: none; border: none; cursor: pointer"
              disabled={gen.partitions.length <= 1}
              onclick={() => removePartition(i)}
              aria-label="Remove partition {i + 1}"
            >✕</button>
          </div>
          <span class="mono text-xs" style="color: var(--text-dim)">
            <b style="color: var(--text)">{i + 1}</b> · {span(i)}
          </span>
        </div>
      {/each}
      <button class="btn self-center" disabled={gen.partitions.length >= PARTITIONS_MAX} onclick={addPartition}>
        + partition
      </button>
    </div>
  {/if}

  {#if limit !== "none"}
    <p class="m-0 mt-3 text-xs" style="color: var(--text-dim)">
      {#if limit === "firmware"}
        This firmware (<span class="mono">{ui.fwVersion ?? "unknown"}</span>) can only generate
        accidental rows on its own; update to
        <b style="color: var(--text)">{COLORGEN_PARTITIONS_FW.join(".")}</b> or newer for partitions.
      {:else if byPartition}
        The boards can generate up to {COLORGEN_PART_MAX} partitions on their own.
      {:else}
        The boards can generate up to {COLORGEN_PAL_MAX} rows on their own.
      {/if}
      This scheme is sent key by key while the app is connected instead, and a board powered up
      without the app shows the last scheme it could generate.
    </p>
  {/if}

  <div class="mt-3 flex flex-wrap items-center gap-2">
    <button class="btn" title="Seven naturals then five sharps, repeating" onclick={sevenFive}>7 + 5</button>
    <div class="flex-1"></div>
    <button class="btn" onclick={() => updateGenerator(defaultGenerator())}>Reset to default</button>
  </div>
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
  .partition {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px;
    border: 1px solid var(--line);
    border-radius: 8px;
    background: var(--panel-2);
  }
  .partition.anchor { border-color: var(--accent-dim); }
</style>
