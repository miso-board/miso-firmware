<script lang="ts">
  // What every key in the grid plays, under the active preset.
  //
  // Reuses BoardView, so a tiled set of boards shows as one instrument with each
  // key at its absolute coordinate — the same view the Key colors tab paints on.
  // Hexes are drawn in the preset's own LED colours, because the two halves of a
  // preset describe the same instrument and seeing them together is the point.

  import BoardView, { type ViewCell } from "./BoardView.svelte";
  import PitchGeneratorPanel from "./PitchGeneratorPanel.svelte";
  import PresetBar from "./PresetBar.svelte";
  import {
    LAYOUTS, noteName, pitchOn, isOverridden, midiFor, hzAt, tuningLabel, tuningError,
    fifthCents,
  } from "../lib/tuning";
  import { colorAt } from "../lib/colorMaps";
  import { ui } from "../lib/ui.svelte";
  import { mesh, gridCells } from "../lib/mesh.svelte";
  import { activePreset, activeColors, canPushPitch, pitchIsStale, PITCH_PUSH_FW } from "../lib/presets.svelte";

  let labelMode = $state<"note" | "midi" | "cents" | "hz" | "none">("note");
  let trueColor = $state(false);
  let hovered = $state<ViewCell | null>(null);

  const pitch = $derived(activePreset().pitch);
  const gen = $derived(pitch.generator);
  const layout = $derived(LAYOUTS[gen.layout]);
  const invalid = $derived(tuningError(gen.tuning) !== null);

  interface Row {
    cell: ViewCell;
    name: string;
    note: number;
    bend: number;
    cents: number;
    hz: number;
    overridden: boolean;
  }

  const rows = $derived.by<Row[]>(() => {
    if (invalid) return [];
    const colors = activeColors();
    const t = gen.tuning;
    return gridCells().map((c) => {
      const p = pitchOn(pitch, c.x, c.y);
      const m = midiFor(p, t, gen.bendSemitones);
      const name = noteName(p);
      const overridden = isOverridden(pitch, c.x, c.y);
      const hz = hzAt(t, p);
      const label =
        labelMode === "note" ? name
        : labelMode === "midi" ? String(m.note)
        : labelMode === "cents" ? m.cents.toFixed(0)
        : labelMode === "hz" ? Math.round(hz).toString()
        : undefined;
      return {
        cell: {
          key: c.key, x: c.x, y: c.y,
          color: colorAt(colors, c.x, c.y),
          label,
          held: ui.held[c.key] === true,
          boardUid: c.board.uid,
          title: `${c.x},${c.y} · ${name} · MIDI ${m.note} bend ${m.bend} (${m.cents.toFixed(1)}¢)`
               + ` · ${hz.toFixed(2)} Hz · LED ${c.led} · sensor ${c.sensor}`
               + (overridden ? " · set by hand" : "")
               + (mesh.boards.length > 1 ? ` · board ${c.board.uid}` : ""),
        },
        name, note: m.note, bend: m.bend, cents: m.cents, hz, overridden,
      };
    });
  });

  const cells = $derived(rows.map((r) => r.cell));

  /** Distinct sounding pitches. Fewer than the key count whenever the tuning
   *  tempers out the interval between two spellings — in 12-TET the 31 keys
   *  sound only 13 pitches, because C♯ and D♭ coincide. */
  const distinctPitches = $derived(new Set(rows.map((r) => Math.round(r.note * 100 + r.cents))).size);

  const overrideCount = $derived(Object.keys(pitch.overrides).length);
  // A vanishing diatonic semitone is the extreme case of enharmonic collapse: a
  // whole Bosanquet row becomes one pitch, rather than pairs merging.
  const semitoneCollapsed = $derived(Math.abs(3600 - 5 * fifthCents(gen.tuning)) < 1e-9);
  const hoveredRow = $derived(hovered ? rows.find((r) => r.cell.key === hovered!.key) : undefined);
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Pitch mapping — {activePreset().name}
  </h2>
  <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
    {layout.name} in <span class="mono">{tuningLabel(gen.tuning)}</span>, with
    <span class="mono">{gen.anchorNote}</span> on key
    <span class="mono">{gen.anchorKey[0]},{gen.anchorKey[1]}</span>. Keys are shown in this preset's
    LED colours; hover one for its note, MIDI number, bend and frequency.
  </p>

  <PresetBar sends="pitch" />

  {#if !ui.connected}
    <p class="m-0 mb-2.5 rounded-md border border-l-[3px] px-3.5 py-2 text-[13px]"
       style="background: var(--panel-2); border-color: var(--line); border-left-color: var(--warn); color: var(--text-dim)">
      Not connected — the mapping saves in this browser, and reaches the board once you connect.
    </p>
  {:else if !canPushPitch()}
    <p class="m-0 mb-2.5 rounded-md border-l-[3px] px-3.5 py-2 text-[13px]"
       style="background: var(--panel-2); border-color: var(--crit); color: var(--text-dim)">
      {#if ui.fwVersion}
        This firmware (<span class="mono">{ui.fwVersion}</span>) plays a fixed
        <b style="color: var(--text)">Bosanquet · 31-EDO</b> tuning and has no way to be retuned, so
        this tab is display-only. Update to
        <b style="color: var(--text)">{PITCH_PUSH_FW.join(".")}</b> or newer to change what the
        instrument plays.
      {:else}
        Waiting for the board to report its firmware version — retuning needs
        <b style="color: var(--text)">{PITCH_PUSH_FW.join(".")}</b> or newer, so nothing is sent
        until it does.
      {/if}
    </p>
  {:else if pitchIsStale()}
    <p class="m-0 mb-2.5 rounded-md border-l-[3px] px-3.5 py-2 text-[13px]"
       style="background: var(--panel-2); border-color: var(--warn); color: var(--warn)">
      The board is running a different tuning from this preset — press <b>Send tuning to board</b>.
    </p>
  {/if}

  <div class="mb-3 flex flex-wrap items-center gap-2">
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
      <option value="none">no labels</option>
    </select>
    <div class="flex-1"></div>
    <span class="mono text-xs" style="color: var(--text-dim)">
      {rows.length} keys · {distinctPitches} distinct pitches
      {#if overrideCount > 0}· {overrideCount} set by hand{/if}
    </span>
  </div>

  {#if invalid}
    <p class="m-0 text-xs" style="color: var(--crit)">
      This tuning can't be rendered — fix it below.
    </p>
  {:else}
    <BoardView
      {cells}
      {trueColor}
      boardCaptions
      onhover={(c) => (hovered = c)}
    />

    <p class="mono m-0 mt-1 text-xs" style="color: var(--text-dim)">
      {#if hoveredRow}
        <b style="color: var(--text)">{hoveredRow.cell.x},{hoveredRow.cell.y}</b>
        · <b style="color: var(--accent)">{hoveredRow.name}</b>
        · MIDI <b style="color: var(--text)">{hoveredRow.note}</b>
        bend <b style="color: var(--text)">{hoveredRow.bend}</b>
        ({hoveredRow.cents.toFixed(1)}¢)
        · <b style="color: var(--text)">{hoveredRow.hz.toFixed(2)} Hz</b>
        {#if hoveredRow.overridden}· <b style="color: var(--warn)">set by hand</b>{/if}
      {:else}
        Hover a key for its note, MIDI number, bend and frequency.
      {/if}
    </p>

    {#if distinctPitches < rows.length}
      <p class="m-0 mt-2 text-xs" style="color: var(--warn)">
        {rows.length - distinctPitches} keys duplicate a pitch another key already sounds: this tuning
        makes those spellings enharmonic{#if semitoneCollapsed}, and its diatonic semitone has
        collapsed entirely, so a whole row is one pitch{:else} — in 12-tone equal temperament, for
        instance, C♯ and D♭ are the same note{/if}. The MIDI monitor can only match those notes by
        pitch rather than attribute them to a particular key.
      </p>
    {/if}
  {/if}
</section>

<PitchGeneratorPanel />

{#if Object.keys(pitch.overrides).length > 0}
  <section
    class="mb-3.5 rounded-lg border p-4"
    style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
  >
    <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
      Keys set by hand
    </h2>
    <p class="m-0 mb-2 text-xs" style="color: var(--text-dim)">
      These came from an imported preset. They override the generator in this app, but the board is
      retuned by parameters rather than a per-key table, so it still plays the generated pitch for
      them — per-key editing lands with the firmware command that can carry it.
    </p>
    <ul class="mono m-0 flex list-none flex-wrap gap-x-4 gap-y-0.5 p-0 text-xs">
      {#each rows.filter((r) => r.overridden) as r}
        <li style="color: var(--text-dim)">
          <b style="color: var(--text)">{r.cell.x},{r.cell.y}</b> → {r.name}
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
