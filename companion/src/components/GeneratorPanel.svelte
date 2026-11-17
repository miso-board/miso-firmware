<script lang="ts">
  // What the colour rows look like. Where they fall is not set here: a key is
  // coloured by where the note the pitch mapping puts on it sits on the chain
  // of fifths, so the rows always line up with what the instrument plays.
  //
  // Two ways to cut the chain up: by accidental (equal slices of seven), or as
  // a hand-built list of partitions of any widths.

  import { accidentalLabel, edoOf } from "../lib/tuning";
  import { swatchStyle } from "../lib/ledColor";
  import { defaultRowColor } from "../lib/autoColors";
  import {
    FIFTHS_LETTERS, PARTITIONS_MAX, PARTITION_WIDTH_MAX,
    COLORGEN_PART_MAX, COLORGEN_PAL_MAX, LANDMARK_LETTERS, landmarkFor, edoWrap, wrapPeriod,
    defaultGenerator, fifthsName, partitionStarts, partitionsFromAccidentals,
    expandSymmetric, symmetricFromRows, symmetricFromPartitions,
    type GeneratorParams, type Partition, type PianoColors,
  } from "../lib/colorMaps";
  import {
    activeColors, activePitch, updateColorGenerator, paintedCount, colorGenerationLimit,
    COLORGEN_WRAP_FW,
  } from "../lib/presets.svelte";
  import { ui } from "../lib/ui.svelte";

  const updateGenerator = (patch: Partial<GeneratorParams>) => updateColorGenerator(patch);

  const gen = $derived(activeColors().generator);
  // The landmark is the layout's: C D E under Wicki-Hayden, one letter otherwise.
  const landmark = $derived(landmarkFor(activePitch().generator.layout));
  const landmarkOn = $derived(landmark === "cde" ? gen.lightenCDE : gen.lightenLetter);
  // Each landmark has its own amount, so setting one layout's leaves the other's.
  const lightenAmount = $derived(landmark === "cde" ? gen.lightenCDEAmount : gen.lightenLetterAmount);
  function setLightenAmount(v: number) {
    updateGenerator(landmark === "cde" ? { lightenCDEAmount: v } : { lightenLetterAmount: v });
  }
  const scheme = $derived<"accidental" | "piano" | "partitions">(
    gen.scheme === "partitions" && gen.partitions.length > 0
      ? "partitions"
      : gen.scheme === "piano" ? "piano" : "accidental",
  );
  const painted = $derived(paintedCount());
  const byPartition = $derived(gen.scheme === "partitions" && gen.partitions.length > 0);

  // --- by accidental ---

  // Choosing a colour by hand is a decision to stop taking them from the
  // tuning, so every swatch edit switches that off.
  function setPaletteColor(i: number, hex: string) {
    const palette = [...gen.palette];
    palette[i] = hex;
    updateGenerator({ palette, autoColors: false });
  }

  // New rows continue the default colour sequence on whichever side they are
  // added, rather than starting grey.
  function addRow() {
    const next = gen.symmetricRows ? gen.palette.length : gen.startAccidental + gen.palette.length;
    updateGenerator({ palette: [...gen.palette, defaultRowColor(next)] });
  }

  function addFlatRow() {
    const next = gen.startAccidental - 1;
    updateGenerator({ palette: [defaultRowColor(next), ...gen.palette], startAccidental: next });
  }

  function removeRow(i: number) {
    if (gen.palette.length <= 1) return;
    updateGenerator({ palette: gen.palette.filter((_: string, j: number) => j !== i) });
  }

  // Symmetric rows keep only the naturals and the sharps; turning it off writes
  // the mirrored flats back out, so neither direction changes a key's colour.
  function toggleSymmetricRows() {
    if (gen.symmetricRows) {
      const full = expandSymmetric({ ...gen, symmetricPartitions: false });
      updateGenerator({ symmetricRows: false, palette: full.palette, startAccidental: full.startAccidental });
    } else {
      updateGenerator({ symmetricRows: true, ...symmetricFromRows(gen) });
    }
  }

  function toggleSymmetricPartitions() {
    if (gen.symmetricPartitions) {
      const full = expandSymmetric({ ...gen, symmetricRows: false });
      updateGenerator({
        symmetricPartitions: false, partitions: full.partitions, anchorPartition: full.anchorPartition,
      });
    } else {
      updateGenerator({ symmetricPartitions: true, ...symmetricFromPartitions(gen) });
    }
  }

  // --- by partition ---

  // Symmetric partitions are edited centre-first but laid out mirrored, so what
  // they cover is read from the full list: partition i is its upper copy there.
  const fullPartitions = $derived(expandSymmetric({ ...gen, symmetricRows: false }));
  const mirrorShift = $derived(gen.symmetricPartitions ? gen.partitions.length - 1 : 0);
  const starts = $derived(byPartition ? partitionStarts(fullPartitions) : []);
  const totalWidth = $derived(fullPartitions.partitions.reduce((sum, p) => sum + p.width, 0));
  // Whether the boards can generate this themselves, or need it sent key by key.
  const limit = $derived(colorGenerationLimit());

  // Wrapping accidental rows to the EDO: offered only when the tuning is one.
  const edo = $derived(edoOf(activePitch().generator.tuning));
  const wrapped = $derived(wrapPeriod(gen, activePitch().generator));
  const wrapWidths = $derived(wrapped !== null ? edoWrap(wrapped).widths : []);

  /** The notes a partition covers, e.g. "F – B" or "F♯ – A♯". */
  function span(i: number): string {
    const from = starts[mirrorShift + i];
    const to = from + gen.partitions[i].width - 1;
    return from === to ? fifthsName(from) : `${fifthsName(from)} – ${fifthsName(to)}`;
  }

  function usePartitions() {
    // First time in: start from the accidental rows, said as partitions, so
    // switching scheme changes nothing on the keys until something is edited.
    const seed = gen.partitions.length > 0 ? {} : partitionsFromAccidentals(expandSymmetric(gen));
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

  // In one update with switching the generated colours off — done as two, the
  // first would be recoloured from the tuning before the second landed.
  function setPartitionColor(i: number, color: string) {
    updateGenerator({
      partitions: gen.partitions.map((p, j) => (j === i ? { ...p, color } : p)),
      autoColors: false,
    });
  }

  function setWidth(i: number, raw: string) {
    const v = Math.round(Number(raw));
    if (!Number.isFinite(v)) return;
    setPartition(i, { width: Math.min(PARTITION_WIDTH_MAX, Math.max(1, v)) });
  }

  function addPartition() {
    if (gen.partitions.length >= PARTITIONS_MAX) return;
    // The next colour in the default sequence, counting outward from the centre.
    const i = gen.partitions.length;
    const color = defaultRowColor(gen.symmetricPartitions ? i : i - gen.anchorPartition);
    setPartitions([...gen.partitions, { width: 7, color }]);
  }

  function removePartition(i: number) {
    if (gen.partitions.length <= 1) return;
    // Keep the anchor on the partition it was on, where that one survives.
    const anchor = gen.anchorPartition > i ? gen.anchorPartition - 1 : gen.anchorPartition;
    setPartitions(gen.partitions.filter((_, j) => j !== i), anchor);
  }

  // --- piano ---

  function setPiano(patch: Partial<PianoColors>) {
    const picksColour = Object.keys(patch).some((k) => k !== "centres");
    updateGenerator({
      piano: { ...gen.piano, ...patch },
      ...(picksColour ? { autoColors: false } : {}),
    });
  }
</script>

<section
  class="rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-3 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Colour scheme
  </h2>

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
          style={scheme === "accidental" ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={scheme === "accidental"}
          title="One colour for the naturals, one for the sharps, one for the flats…"
          onclick={() => updateGenerator({ scheme: "accidental" })}
        >Accidental</button>
        <button
          class="btn text-sm normal-case tracking-normal"
          style={scheme === "piano" ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={scheme === "piano"}
          title="Two colours, like a piano's white and black keys"
          onclick={() => updateGenerator({ scheme: "piano" })}
        >Piano</button>
        <button
          class="btn text-sm normal-case tracking-normal"
          style={byPartition ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={byPartition}
          title="Slices of the chain of fifths of any widths"
          onclick={usePartitions}
        >Partitions</button>
      </div>
    </div>

    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Colours
      <button
        class="btn text-sm normal-case tracking-normal"
        style={gen.autoColors ? "border-color: var(--accent); color: var(--accent)" : ""}
        aria-pressed={gen.autoColors}
        title="Choose the colours from the width of the fifth: red at the flat end, gold around 31-EDO, green where the fifth is pure, blue and magenta beyond"
        onclick={() => updateGenerator({ autoColors: !gen.autoColors })}
      >Automatic</button>
    </div>

    {#if scheme === "accidental"}
      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Flats
        <button
          class="btn text-sm normal-case tracking-normal"
          style={gen.symmetricRows ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={gen.symmetricRows}
          title="Set the naturals and the sharps; each flat takes the colour of the sharp it mirrors"
          onclick={toggleSymmetricRows}
        >Symmetrical</button>
      </div>
      {#if !gen.symmetricRows}
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
      {/if}
      {#if edo !== null}
        <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
          Remote accidentals
          <button
            class="btn text-sm normal-case tracking-normal"
            style={gen.wrapEdo ? "border-color: var(--accent); color: var(--accent)" : ""}
            aria-pressed={gen.wrapEdo}
            title="Colour each of the {edo} notes once. The outermost flats and sharps are trimmed so the pattern repeats every {edo} fifths, and a note spelled further out takes the colour of the note it is the same as."
            onclick={() => updateGenerator({ wrapEdo: !gen.wrapEdo })}
          >Wrap to {edo}-EDO</button>
        </div>
      {/if}
    {:else if scheme === "partitions"}
      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Below the centre
        <button
          class="btn text-sm normal-case tracking-normal"
          style={gen.symmetricPartitions ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={gen.symmetricPartitions}
          title="Set the centre partition and those above it; the same are laid out below it in reverse"
          onclick={toggleSymmetricPartitions}
        >Symmetrical</button>
      </div>
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
            {#each gen.partitions as _, i}
              <option value={i}>{gen.symmetricPartitions && i === 0 ? "centre" : i + 1}</option>
            {/each}
          </select>
        </div>
      </div>
    {/if}

    {#if landmark !== "none"}
    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Landmark
      <div class="flex items-center gap-1.5 text-sm normal-case tracking-normal">
        {#if landmark === "cde"}
          <button
            class="btn"
            style={gen.lightenCDE ? "border-color: var(--accent); color: var(--accent)" : ""}
            aria-pressed={gen.lightenCDE}
            onclick={() => updateGenerator({ lightenCDE: !gen.lightenCDE })}
          >Lighten C D E</button>
        {:else}
          <button
            class="btn"
            style={gen.lightenLetter ? "border-color: var(--accent); color: var(--accent)" : ""}
            aria-pressed={gen.lightenLetter}
            onclick={() => updateGenerator({ lightenLetter: !gen.lightenLetter })}
          >Lighten every</button>
          <select
            class="btn"
            value={gen.lightenLetterIndex}
            aria-label="Letter to lighten"
            onchange={(e) => updateGenerator({ lightenLetterIndex: +e.currentTarget.value, lightenLetter: true })}
          >
            {#each LANDMARK_LETTERS as letter, i}<option value={i}>{letter}</option>{/each}
          </select>
        {/if}
      </div>
    </div>

    <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Lighten by <span class="mono normal-case" style="color: var(--text)">{lightenAmount}%</span>
      <input
        type="range" min="0" max="100" value={lightenAmount}
        disabled={!landmarkOn}
        oninput={(e) => setLightenAmount(+e.currentTarget.value)}
        class="w-32"
        aria-label="How far landmark keys move toward white"
      />
    </label>
    {/if}

    <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      LED brightness <span class="mono normal-case" style="color: var(--text)">{gen.brightness}%</span>
      <input
        type="range" min="1" max="60" value={gen.brightness}
        oninput={(e) => updateGenerator({ brightness: +e.currentTarget.value })}
        class="w-32"
      />
    </label>
  </div>

  {#if scheme === "piano"}
    {#snippet swatch(label: string, hex: string, set: (hex: string) => void)}
      <div class="flex flex-col items-center gap-1">
        <span class="text-xs" style="color: var(--text-dim)">{label}</span>
        <div class="relative">
          <span class="block h-8 w-8 rounded-md border" style="{swatchStyle(hex)}; border-color: var(--line)"></span>
          <input
            type="color" value={hex}
            oninput={(e) => set(e.currentTarget.value)}
            class="absolute inset-0 h-8 w-8 cursor-pointer opacity-0"
            aria-label="{label} colour"
          />
        </div>
      </div>
    {/snippet}
    <p class="m-0 mt-4 text-xs" style="color: var(--text-dim)">
      One colour for the naturals and one for the sharps and flats, like a piano's white and black
      keys.
    </p>
    <div class="mt-2 flex flex-wrap items-end gap-x-3 gap-y-3">
      {@render swatch("Naturals", gen.piano.natural, (hex) => setPiano({ natural: hex }))}
      {@render swatch("Sharps / flats", gen.piano.accidental, (hex) => setPiano({ accidental: hex }))}
      <button
        class="btn ml-3 text-sm"
        style={gen.piano.centres ? "border-color: var(--accent); color: var(--accent)" : ""}
        aria-pressed={gen.piano.centres}
        title="Gives the middle key of each group of three its own colour: D among the naturals, G♯ / A♭ among the sharps and flats"
        onclick={() => setPiano({ centres: !gen.piano.centres })}
      >Pick out the middle of each group of three</button>
      {#if gen.piano.centres}
        {@render swatch("D", gen.piano.centreNatural, (hex) => setPiano({ centreNatural: hex }))}
        {@render swatch("G♯ / A♭", gen.piano.centreAccidental, (hex) => setPiano({ centreAccidental: hex }))}
      {/if}
    </div>
  {:else if scheme === "accidental"}
    <p class="m-0 mt-4 text-xs" style="color: var(--text-dim)">
      One colour per accidental. The pattern repeats for accidentals beyond the ones set here.
      {#if gen.symmetricRows}
        The flats mirror the sharps: ♭ takes ♯'s colour, ♭♭ takes ♯♯'s, and so on.
      {/if}
      {#if wrapped !== null}
        Wrapped to {wrapped}-EDO, the fifths are coloured in runs of
        <span class="mono" style="color: var(--text)">{wrapWidths.join(" · ")}</span>.
      {/if}
    </p>
    <div class="mt-2 flex flex-wrap items-end gap-2">
      {#if !gen.symmetricRows}
        <button class="btn mb-6" onclick={addFlatRow} title="Add a row on the flat side">+ ♭ row</button>
      {/if}
      {#each gen.palette as hex, i}
        <div class="flex flex-col items-center gap-1">
          <span class="text-xs" style="color: var(--text-dim)">
            {accidentalLabel(gen.symmetricRows ? i : gen.startAccidental + i)}
          </span>
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
      <button class="btn mb-6" onclick={addRow} title="Add a row on the sharp side">+ ♯ row</button>
    </div>
  {:else}
    <p class="m-0 mt-4 text-xs" style="color: var(--text-dim)">
      Each partition is a run of adjacent fifths (… B♭ F C G D A E B F♯ …) and colours every note in
      it. Together they span <b class="mono" style="color: var(--text)">{totalWidth}</b>
      fifth{totalWidth === 1 ? "" : "s"}, then repeat.
      {#if gen.symmetricPartitions}
        The first is the centre; the others are laid out above it as listed and below it in reverse.
      {/if}
    </p>
    <div class="mt-2 flex flex-wrap items-stretch gap-2">
      {#each gen.partitions as part, i}
        <div class="partition" class:anchor={i === gen.anchorPartition}>
          <div class="flex items-center gap-2">
            <div class="relative">
              <span class="block h-8 w-8 rounded-md border" style="{swatchStyle(part.color)}; border-color: var(--line)"></span>
              <input
                type="color" value={part.color}
                oninput={(e) => setPartitionColor(i, e.currentTarget.value)}
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
            <b style="color: var(--text)">{gen.symmetricPartitions && i === 0 ? "centre" : i + 1}</b>
            · {span(i)}{gen.symmetricPartitions && i > 0 ? " (and below)" : ""}
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
        This firmware (<span class="mono">{ui.fwVersion ?? "unknown"}</span>) can't generate this
        scheme on its own; update to
        <b style="color: var(--text)">{COLORGEN_WRAP_FW.join(".")}</b> or newer.
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
