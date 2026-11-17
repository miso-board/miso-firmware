<script lang="ts">
  // The tuning: how wide the fifth is, and where concert pitch sits.
  //
  // One number, three ways to set it, and no modes between them. The EDO
  // buttons, the list of other EDOs, the ruler and the cents box all show the
  // same fifth and all write it, so whichever was touched last, the others
  // follow: drag the ruler onto 31-EDO's fifth and the 31 button lights up.
  //
  // The layout is chosen separately (LayoutPicker), below this — a tuning comes
  // first, because a layout only says which interval each direction is, and the
  // tuning is what makes those intervals a size.

  import {
    SELECTABLE_EDOS, FIFTH_MIN, FIFTH_MAX, DEFAULT_REFERENCE,
    REFERENCE_HZ_MIN, REFERENCE_HZ_MAX,
    fifthCents, tuningError, tuningWarning, effectiveEdo, referenceOf, referenceError,
    referenceOffsetCents,
    type Tuning,
  } from "../lib/tuning";
  import {
    activePreset, updatePitchGenerator, referenceIsIgnored, PITCH_REFERENCE_FW,
  } from "../lib/presets.svelte";
  import { ui } from "../lib/ui.svelte";
  import FifthRuler from "./FifthRuler.svelte";

  // The tunings worth a button of their own, from the narrowest fifth to the
  // widest — which is also left to right along the ruler underneath.
  const FEATURED_EDOS = [7, 26, 19, 31, 12, 53, 41, 17, 22] as const;
  const OTHER_EDOS = SELECTABLE_EDOS.filter((e) => !(FEATURED_EDOS as readonly number[]).includes(e));
  const edoFifth = (edo: number) => fifthCents({ kind: "edo", edo });
  const RULER_MARKS = FEATURED_EDOS.map((edo) => ({ cents: edoFifth(edo), label: String(edo) }));

  const gen = $derived(activePreset().pitch.generator);
  const fifth = $derived(fifthCents(gen.tuning));
  const error = $derived(tuningError(gen.tuning));
  const warning = $derived(tuningWarning(gen.tuning));

  /**
   * The EDO this fifth is, if it is one. Asked of the fifth rather than of how
   * it was set, so the ruler and the cents box light the buttons too. Half a
   * millicent is the finest the ruler and the box can say, and the wire carries
   * whole millicents, so anything closer is the same tuning.
   */
  const matchedEdo = $derived.by<number | null>(() => {
    if (gen.tuning.kind === "edo") return gen.tuning.edo;
    return SELECTABLE_EDOS.find((e) => Math.abs(edoFifth(e) - fifth) < 0.0005) ?? null;
  });
  const otherEdo = $derived(
    matchedEdo !== null && !(FEATURED_EDOS as readonly number[]).includes(matchedEdo) ? matchedEdo : null,
  );

  let centsDraft = $state("");
  // Re-seed the draft whenever the stored tuning changes from elsewhere (a
  // button, the ruler, a preset switch), but leave it alone while being typed.
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

  function setFifth(cents: number) {
    setTuning({ kind: "fifth", cents: Math.min(FIFTH_MAX, Math.max(FIFTH_MIN, cents)) });
  }

  function commitCents(raw: string) {
    const v = Number(raw);
    if (Number.isFinite(v)) setFifth(v);
    else centsDraft = fifth.toFixed(3);
  }

  // --- concert pitch ---

  const reference = $derived(referenceOf(gen));
  const refError = $derived(referenceError(reference));
  const refOffset = $derived(error ? 0 : referenceOffsetCents(gen.tuning, reference));

  let hzDraft = $state("");
  let lastHz = $state("");
  $effect(() => {
    const v = reference.hz.toFixed(2);
    if (v !== lastHz) {
      lastHz = v;
      hzDraft = v;
    }
  });

  function commitHz(raw: string) {
    const v = Number(raw);
    // Typing the two decimals already shown must not nudge the stored value.
    if (!Number.isFinite(v) || raw.trim() === reference.hz.toFixed(2)) {
      hzDraft = reference.hz.toFixed(2);
      return;
    }
    updatePitchGenerator({
      referenceHz: Math.min(REFERENCE_HZ_MAX, Math.max(REFERENCE_HZ_MIN, v)),
    });
  }

  function setReference(note: string, hz: number) {
    updatePitchGenerator({ referenceNote: note, referenceHz: hz });
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Tuning
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    A tuning is one number — the width of its fifth. Pick an equal division of the octave, or set
    the fifth directly on the ruler; they are the same control.
  </p>

  <div class="edos" role="radiogroup" aria-label="Equal division of the octave">
    {#each FEATURED_EDOS as edo}
      {@const selected = matchedEdo === edo}
      <button
        class="edo"
        class:selected
        role="radio"
        aria-checked={selected}
        aria-label="{edo}-EDO"
        onclick={() => setTuning({ kind: "edo", edo })}
      >
        <span class="n">{edo}</span>
        <span class="unit">EDO</span>
        <span class="mono cents">{edoFifth(edo).toFixed(1)}¢</span>
      </button>
    {/each}
    <label class="edo other" class:selected={otherEdo !== null}>
      <span class="unit">Other EDO</span>
      <select
        class="btn"
        value={otherEdo ?? ""}
        aria-label="Other equal divisions of the octave"
        onchange={(e) => {
          if (e.currentTarget.value !== "") setTuning({ kind: "edo", edo: +e.currentTarget.value });
        }}
      >
        <option value="">choose…</option>
        {#each OTHER_EDOS as e}<option value={e}>{e}-EDO</option>{/each}
        <!-- A preset saved in an EDO no longer offered keeps it, and shows it. -->
        {#if otherEdo !== null && !OTHER_EDOS.includes(otherEdo)}
          <option value={otherEdo}>{otherEdo}-EDO (plays as {effectiveEdo(otherEdo)}-EDO)</option>
        {/if}
      </select>
    </label>
  </div>

  <div class="mt-3">
    <FifthRuler value={fifth} marks={RULER_MARKS} onchange={setFifth} />
  </div>

  <div class="mt-3 flex flex-wrap items-end gap-x-6 gap-y-3">
    <label class="field">
      Fifth in cents
      <input
        class="btn mono text-sm normal-case tracking-normal"
        style="width: 112px"
        type="number"
        step="0.001"
        min={FIFTH_MIN.toFixed(3)}
        max={FIFTH_MAX}
        bind:value={centsDraft}
        onchange={() => commitCents(centsDraft)}
      />
    </label>

    <div class="field">
      Concert pitch
      <div class="flex items-center gap-1.5 text-sm normal-case tracking-normal">
        <input
          class="btn mono"
          style="width: 72px"
          value={reference.note}
          onchange={(e) => updatePitchGenerator({ referenceNote: e.currentTarget.value.trim() || DEFAULT_REFERENCE.note })}
          aria-label="Concert pitch note, in scientific pitch notation"
          title="Scientific pitch notation, e.g. C4, A4, F#3"
        />
        <span style="color: var(--text-dim)">=</span>
        <input
          class="btn mono"
          style="width: 104px"
          type="number"
          step="0.01"
          min={REFERENCE_HZ_MIN}
          max={REFERENCE_HZ_MAX}
          bind:value={hzDraft}
          onchange={() => commitHz(hzDraft)}
          aria-label="Concert pitch frequency in hertz"
        />
        <span style="color: var(--text-dim)">Hz</span>
      </div>
    </div>

    <div class="field">
      Set to
      <div class="flex items-center gap-1 text-xs normal-case tracking-normal">
        <button class="btn" onclick={() => setReference(DEFAULT_REFERENCE.note, DEFAULT_REFERENCE.hz)}>
          C4 = 261.63
        </button>
        <button class="btn" onclick={() => setReference("A4", 440)}>A4 = 440</button>
      </div>
    </div>
  </div>

  {#if error}
    <p class="note" style="border-color: var(--crit); color: var(--crit)">
      {error} The last usable tuning is still in effect.
    </p>
  {:else if warning}
    <p class="note" style="border-color: var(--warn); color: var(--warn)">{warning}</p>
  {/if}

  {#if refError}
    <p class="note" style="border-color: var(--crit); color: var(--crit)">
      {refError} Until it is fixed, C4 = 261.63 Hz is used.
    </p>
  {:else if referenceIsIgnored()}
    <p class="note" style="border-color: var(--warn); color: var(--text-dim)">
      This firmware (<span class="mono">{ui.fwVersion ?? "unknown"}</span>) can't be given a concert
      pitch, so the board sounds
      <b class="mono" style="color: var(--text)">{Math.abs(refOffset).toFixed(1)}¢</b>
      {refOffset > 0 ? "flat" : "sharp"} of what is shown here. Update to
      <b style="color: var(--text)">{PITCH_REFERENCE_FW.join(".")}</b> or newer.
    </p>
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

  .edos {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
  }
  .edo {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 1px;
    flex: 1 1 74px;
    min-width: 0;
    min-height: 76px;
    font: inherit;
    color: var(--text-dim);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 8px 6px;
    cursor: pointer;
  }
  .edo:hover { border-color: var(--accent-dim); }
  .edo.selected { border-color: var(--accent); color: var(--text); }
  .edo .n { font-size: 24px; font-weight: 700; line-height: 1.1; color: var(--text); }
  .edo.selected .n { color: var(--accent); }
  .edo .unit { font-size: 10px; letter-spacing: 0.14em; text-transform: uppercase; }
  .edo .cents { font-size: 11px; margin-top: 3px; }
  .edo.other { cursor: default; gap: 6px; flex: 1.6 1 132px; }
  .edo.other .btn { padding: 5px 8px; font-size: 13px; max-width: 100%; }

  .field {
    display: flex;
    flex-direction: column;
    gap: 4px;
    font-size: 10px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--text-dim);
  }
  .note {
    margin: 12px 0 0;
    border-left: 3px solid;
    border-radius: 6px;
    padding: 8px 12px;
    font-size: 12px;
    background: var(--panel-2);
  }
</style>
