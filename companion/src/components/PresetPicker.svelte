<script lang="ts">
  // Which preset is being set up, chosen from a row of pictures.
  //
  // A preset is one LED mapping plus one pitch mapping: switching here changes
  // what the instrument looks like AND what it plays. Each icon is that preset's
  // own keys — its colours over its layout — on a 2x2 tiling of boards.
  //
  // Edits to a preset are not saved as they are made. They show on the keys and
  // stream to the boards at once, but stay unsaved until "Save changes", so
  // trying something out cannot quietly overwrite a preset.

  import { ui, toast } from "../lib/ui.svelte";
  import { colorAt } from "../lib/colorMaps";
  import { ledColor } from "../lib/ledColor";
  import { ICON_SHIFT } from "../lib/tilingIcon";
  import {
    presetState, activePreset, selectPreset, renamePreset, newPreset, duplicatePreset,
    deletePreset, savePreset, revertPreset, isDirty, anyDirty,
    copyPresetJson, importPresetJson, pushAll, canPushPitch, pitchIsStale, PITCH_PUSH_FW,
    type Preset,
  } from "../lib/presets.svelte";
  import TilingIcon from "./TilingIcon.svelte";

  const dirty = $derived(isDirty());

  /** A preset's own colours, painted keys included, as icon fills. */
  function fillFor(p: Preset): (x: number, y: number) => string {
    const pitchGen = p.pitch.generator;
    const [dx, dy] = ICON_SHIFT[pitchGen.layout] ?? [0, 0];
    return (x, y) => {
      const c = ledColor(colorAt(p.colors, pitchGen, x + dx, y + dy));
      return c.off ? "var(--panel-2)" : c.flat;
    };
  }

  function confirmDelete() {
    const name = activePreset().name;
    if (window.confirm(`Delete the preset "${name}"? This can't be undone.`)) {
      deletePreset();
      toast(`Deleted "${name}".`);
    }
  }

  function save() {
    savePreset();
    toast(`Saved "${activePreset().name}".`);
  }

  function importPrompt() {
    const text = window.prompt("Paste a preset JSON (or an older mapping JSON):");
    if (text && !importPresetJson(text)) toast("Couldn't parse that JSON.");
    else if (text) toast("Preset imported.");
  }

  async function send() {
    // pushAll skips the tuning by itself on firmware that can't be retuned.
    const retunable = canPushPitch();
    await pushAll();
    toast(retunable ? "Colors and tuning sent to board." : "Colors sent to board.");
  }

  // Unsaved edits live only in this page, so say so before it goes away.
  function beforeUnload(e: BeforeUnloadEvent) {
    if (anyDirty()) e.preventDefault();
  }
</script>

<svelte:window onbeforeunload={beforeUnload} />

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Select a preset
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    A preset is a tuning, a layout and a set of colours. Changes show on the keys and reach the
    boards straight away, but are only kept once you save them.
  </p>

  <div class="presets" role="radiogroup" aria-label="Preset">
    {#each presetState.presets as p, i}
      {@const selected = presetState.active === i}
      <div class="card" class:selected>
        <button
          class="pick"
          role="radio"
          aria-checked={selected}
          aria-label={p.name + (isDirty(i) ? " (unsaved changes)" : "")}
          onclick={() => selectPreset(i)}
        >
          <span class="icon"><TilingIcon fill={fillFor(p)} /></span>
          <span class="name">{p.name}</span>
          <span class="unsaved" style:visibility={isDirty(i) ? "visible" : "hidden"}>unsaved</span>
        </button>
        {#if selected}
          <button class="bin" aria-label="Delete preset {p.name}" title="Delete this preset" onclick={confirmDelete}>
            <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" fill="none"
                 stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
              <path d="M2.5 4.5h11M6.5 4.5V3a.5.5 0 0 1 .5-.5h2a.5.5 0 0 1 .5.5v1.5M4 4.5l.6 8a1 1 0 0 0 1 .9h4.8a1 1 0 0 0 1-.9l.6-8M6.6 7v4M9.4 7v4" />
            </svg>
          </button>
        {/if}
      </div>
    {/each}
    <div class="card">
      <button class="pick create" onclick={newPreset}>
        <span class="icon plus" aria-hidden="true">+</span>
        <span class="name">Create new</span>
        <span class="unsaved" style:visibility="hidden">unsaved</span>
      </button>
    </div>
  </div>

  <div class="mt-3 flex flex-wrap items-center gap-2">
    <input
      class="btn"
      style="width: 170px"
      value={activePreset().name}
      onchange={(e) => renamePreset(e.currentTarget.value || "Untitled")}
      aria-label="Preset name"
    />
    <button class="btn" class:primary={dirty} disabled={!dirty} onclick={save}>Save changes</button>
    {#if dirty}
      <button class="btn" onclick={revertPreset}>Discard changes</button>
    {/if}
    <div class="flex-1"></div>
    <button class="btn" onclick={duplicatePreset}>Duplicate preset</button>
    <button class="btn" onclick={copyPresetJson}>Copy JSON</button>
    <button class="btn" onclick={importPrompt}>Import…</button>
    <button class="btn primary" disabled={!ui.connected} onclick={send}>Send to board</button>
  </div>

  {#if !ui.connected}
    <!-- Said once for the whole tab, above this section (MappingTab). -->
  {:else if !canPushPitch()}
    <p class="note" style="border-left-color: var(--crit)">
      {#if ui.fwVersion}
        This firmware (<span class="mono">{ui.fwVersion}</span>) plays a fixed
        <b style="color: var(--text)">Bosanquet · 31-EDO</b> tuning and has no way to be retuned, so
        colours reach the board but the tuning and layout here are display-only. Update to
        <b style="color: var(--text)">{PITCH_PUSH_FW.join(".")}</b> or newer to change what the
        instrument plays.
      {:else}
        Waiting for the board to report its firmware version — retuning needs
        <b style="color: var(--text)">{PITCH_PUSH_FW.join(".")}</b> or newer, so no tuning is sent
        until it does.
      {/if}
    </p>
  {:else if pitchIsStale()}
    <p class="note" style="border-left-color: var(--warn); color: var(--warn)">
      The board is running a different tuning from this preset — press <b>Send to board</b>.
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
  .btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-ink); font-weight: 600; }

  .presets {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(150px, 1fr));
    gap: 10px;
  }
  .card { position: relative; }
  .pick {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    width: 100%;
    height: 100%;
    font: inherit;
    color: var(--text-dim);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 10px 10px 6px;
    cursor: pointer;
  }
  .pick:hover { border-color: var(--accent-dim); }
  .card.selected .pick { border-color: var(--accent); color: var(--text); }
  .icon { display: block; width: 100%; height: 84px; }
  .name {
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 13px;
    font-weight: 600;
  }
  .unsaved {
    font-size: 10px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--warn);
    line-height: 1;
  }
  .create { border-style: dashed; }
  .plus {
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 44px;
    font-weight: 300;
    line-height: 1;
    color: var(--text-dim);
  }
  .bin {
    position: absolute;
    top: 6px;
    right: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 26px;
    color: var(--text-dim);
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 6px;
    cursor: pointer;
  }
  .bin:hover { color: var(--crit); border-color: var(--crit); }

  .note {
    margin: 12px 0 0;
    border: 1px solid var(--line);
    border-left-width: 3px;
    border-radius: 6px;
    padding: 8px 14px;
    font-size: 13px;
    color: var(--text-dim);
    background: var(--panel-2);
  }
</style>
