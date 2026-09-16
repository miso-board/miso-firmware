<script lang="ts">
  // Controls for the active preset's procedural pitch mapping: a layout over a
  // tuning, pinned by one note on one key.
  //
  // Layout and tuning are independent by construction — a layout is a basis
  // change in meantonal's (w, h) pitch space, so Bosanquet and Wicki-Hayden mean
  // the same thing in every tuning, and only the fifth changes underneath.

  import {
    LAYOUTS, VALID_EDOS, FIFTH_MIN, FIFTH_MAX, CENTRE_KEY,
    fifthCents, tuningError, tuningWarning, noteName, generatePitchAt, hzAt,
    type LayoutId, type Tuning,
  } from "../lib/tuning";
  import { activePreset, activeColors, updatePitchGenerator, updateColorGenerator } from "../lib/presets.svelte";

  const gen = $derived(activePreset().pitch.generator);
  const layout = $derived(LAYOUTS[gen.layout]);
  const fifth = $derived(fifthCents(gen.tuning));
  const error = $derived(tuningError(gen.tuning));
  const warning = $derived(tuningWarning(gen.tuning));

  // What the anchor key actually sounds. Equal to `anchorNote` when the note
  // parses; when it does not, the generator falls back and this shows what the
  // board is really doing rather than what was typed.
  const anchorActual = $derived.by(() => {
    if (error) return null;
    const p = generatePitchAt(gen, gen.anchorKey[0], gen.anchorKey[1]);
    return { name: noteName(p), hz: hzAt(gen.tuning, p) };
  });

  // Fifths worth reaching for that no equal division provides.
  const NAMED_FIFTHS: { name: string; cents: number; why: string }[] = [
    { name: "1/4-comma", cents: 300 * Math.log2(5), why: "pure 5:4 major third" },
    { name: "1/3-comma", cents: 600 * Math.log2(6) - 600, why: "pure 6:5 minor third" },
    { name: "Pythagorean", cents: 1200 * Math.log2(1.5), why: "pure 3:2 fifth" },
    { name: "Equal", cents: 700, why: "ordinary 12-tone equal temperament" },
  ];

  let centsDraft = $state("");
  // Re-seed the draft whenever the stored tuning changes from elsewhere (a preset
  // switch, a named fifth, a mode change), but leave it alone while being typed.
  let lastSeeded = $state("");
  $effect(() => {
    const v = fifth.toFixed(3);
    if (v !== lastSeeded) {
      lastSeeded = v;
      centsDraft = v;
    }
  });

  function setTuning(t: Tuning) {
    updatePitchGenerator({ tuning: t });
  }

  function commitCents(raw: string) {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    setTuning({ kind: "fifth", cents: Math.min(FIFTH_MAX, Math.max(FIFTH_MIN, v)) });
  }

  function nudgeAnchorKey(axis: 0 | 1, delta: number) {
    const key: [number, number] = [gen.anchorKey[0], gen.anchorKey[1]];
    key[axis] += delta;
    updatePitchGenerator({ anchorKey: key });
  }

  /** Colour rows and sounding pitch can disagree; this reconciles them. */
  const colourLayout = $derived(activeColors().generator?.layout);
  const layoutsDiffer = $derived(colourLayout !== undefined && colourLayout !== gen.layout);
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Procedural pitch
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    Every key's pitch from a layout over a tuning, computed with <b>meantonal</b>.
    A tuning is one number — the width of its fifth — so an EDO and a historical
    temperament are the same control.
    <span class="mono">{layout.axes}</span>
  </p>

  <div class="flex flex-wrap items-start gap-x-6 gap-y-3">
    <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Layout
      <select
        class="btn text-sm normal-case tracking-normal"
        value={gen.layout}
        onchange={(e) => updatePitchGenerator({ layout: e.currentTarget.value as LayoutId })}
      >
        {#each Object.values(LAYOUTS) as l}<option value={l.id}>{l.name}</option>{/each}
      </select>
    </label>

    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Tuning by
      <div class="flex items-center gap-1">
        <button
          class="btn text-sm normal-case tracking-normal"
          style={gen.tuning.kind === "edo" ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={gen.tuning.kind === "edo"}
          onclick={() => setTuning({ kind: "edo", edo: 31 })}
        >EDO</button>
        <button
          class="btn text-sm normal-case tracking-normal"
          style={gen.tuning.kind === "fifth" ? "border-color: var(--accent); color: var(--accent)" : ""}
          aria-pressed={gen.tuning.kind === "fifth"}
          onclick={() => setTuning({ kind: "fifth", cents: fifth })}
        >Fifth</button>
      </div>
    </div>

    {#if gen.tuning.kind === "edo"}
      <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Divisions of the octave
        <select
          class="btn text-sm normal-case tracking-normal"
          value={gen.tuning.edo}
          onchange={(e) => setTuning({ kind: "edo", edo: +e.currentTarget.value })}
        >
          {#each VALID_EDOS as e}<option value={e}>{e}-EDO</option>{/each}
        </select>
      </label>
    {:else}
      <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Fifth in cents
        <input
          class="btn mono text-sm normal-case tracking-normal"
          style="width: 104px"
          type="number"
          step="0.001"
          min={FIFTH_MIN.toFixed(3)}
          max={FIFTH_MAX}
          bind:value={centsDraft}
          onchange={() => commitCents(centsDraft)}
        />
      </label>
      <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
        Named fifths
        <div class="flex flex-wrap items-center gap-1">
          {#each NAMED_FIFTHS as f}
            <button
              class="btn text-xs normal-case tracking-normal"
              title="{f.cents.toFixed(3)}¢ — {f.why}"
              onclick={() => setTuning({ kind: "fifth", cents: f.cents })}
            >{f.name}</button>
          {/each}
        </div>
      </div>
    {/if}

    <label class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Anchor note
      <input
        class="btn mono text-sm normal-case tracking-normal"
        style="width: 72px"
        value={gen.anchorNote}
        onchange={(e) => updatePitchGenerator({ anchorNote: e.currentTarget.value.trim() || "D4" })}
        aria-label="Anchor note in scientific pitch notation"
        title="Scientific pitch notation, e.g. D4, F#3, Bb5"
      />
    </label>

    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      on key
      <div class="flex items-center gap-1">
        {#each [0, 1] as axis}
          <span class="mono ml-1 text-xs normal-case" style="color: var(--text-dim)">{axis === 0 ? "x" : "y"}</span>
          <button class="btn px-2 py-1" aria-label="decrease" onclick={() => nudgeAnchorKey(axis as 0 | 1, -1)}>−</button>
          <span class="mono w-6 text-center text-sm" style="color: var(--text)">{gen.anchorKey[axis]}</span>
          <button class="btn px-2 py-1" aria-label="increase" onclick={() => nudgeAnchorKey(axis as 0 | 1, 1)}>+</button>
        {/each}
      </div>
    </div>

    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Sounds
      <span class="mono text-base" style="color: var(--accent)">
        {anchorActual ? `${anchorActual.name} · ${anchorActual.hz.toFixed(2)} Hz` : "—"}
      </span>
    </div>

    <div class="flex flex-col gap-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
      Fifth
      <span class="mono text-base" style="color: var(--text)">{fifth.toFixed(3)}¢</span>
    </div>
  </div>

  {#if error}
    <p class="m-0 mt-3 rounded-md border-l-[3px] px-3 py-2 text-xs"
       style="background: var(--panel-2); border-color: var(--crit); color: var(--crit)">
      {error} The last usable tuning is still in effect.
    </p>
  {:else if warning}
    <p class="m-0 mt-3 rounded-md border-l-[3px] px-3 py-2 text-xs"
       style="background: var(--panel-2); border-color: var(--warn); color: var(--warn)">
      {warning}
    </p>
  {/if}

  {#if layoutsDiffer}
    <p class="m-0 mt-3 flex flex-wrap items-center gap-2 text-xs" style="color: var(--text-dim)">
      This preset colours its rows by <b style="color: var(--text)">{LAYOUTS[colourLayout!].name}</b>
      but sounds <b style="color: var(--text)">{layout.name}</b>, so the colour rows won't line up with
      the accidentals you hear.
      <button class="btn text-xs normal-case tracking-normal" onclick={() => updateColorGenerator({ layout: gen.layout })}>
        Match colours to {layout.name}
      </button>
    </p>
  {/if}

  <div class="mt-4 flex flex-wrap items-center gap-2">
    <button class="btn" onclick={() => updatePitchGenerator({ anchorKey: [CENTRE_KEY[0], CENTRE_KEY[1]], anchorNote: "D4" })}>
      Re-centre on D4
    </button>
    <button class="btn" onclick={() => setTuning({ kind: "edo", edo: 31 })}>Reset to 31-EDO</button>
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
</style>
