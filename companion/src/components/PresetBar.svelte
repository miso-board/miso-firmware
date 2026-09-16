<script lang="ts">
  // The preset selector, shared by the Key colors and Pitch mapping tabs.
  //
  // A preset is one LED mapping plus one pitch mapping, so it is deliberately
  // the same control on both tabs rather than two independent lists: switching
  // here changes what the instrument looks like AND what it plays.

  import { ui, toast } from "../lib/ui.svelte";
  import {
    presetState, activePreset, selectPreset, renamePreset, newPreset, duplicatePreset,
    deletePreset, copyPresetJson, importPresetJson, pushColors, pushPitch, canPushPitch,
  } from "../lib/presets.svelte";

  interface Props {
    /** Which half this tab sends, for the Send button's wording. */
    sends: "colors" | "pitch";
  }
  let { sends }: Props = $props();

  function importPrompt() {
    const text = window.prompt("Paste a preset JSON (or an older mapping JSON):");
    if (text && !importPresetJson(text)) toast("Couldn't parse that JSON.");
    else if (text) toast("Preset imported.");
  }

  async function send() {
    if (sends === "pitch") {
      await pushPitch();
      toast("Tuning sent to board.");
    } else {
      await pushColors();
      toast("Colors sent to board.");
    }
  }

  const sendDisabled = $derived(sends === "pitch" ? !canPushPitch() : !ui.connected);
</script>

<div class="mb-3 flex flex-wrap items-center gap-2">
  <span class="text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">Preset</span>
  <select
    class="btn"
    value={presetState.active}
    aria-label="Active preset"
    onchange={(e) => selectPreset(+e.currentTarget.value)}
  >
    {#each presetState.presets as p, i}<option value={i}>{p.name}</option>{/each}
  </select>
  <input
    class="btn"
    style="width: 150px"
    value={activePreset().name}
    onchange={(e) => renamePreset(e.currentTarget.value || "Untitled")}
    aria-label="Preset name"
  />
  <button class="btn" onclick={newPreset}>New</button>
  <button class="btn" onclick={duplicatePreset}>Duplicate</button>
  <button class="btn" onclick={deletePreset}>Delete</button>
  <div class="flex-1"></div>
  <button class="btn" onclick={copyPresetJson}>Copy JSON</button>
  <button class="btn" onclick={importPrompt}>Import…</button>
  <button class="btn primary" disabled={sendDisabled} onclick={send}>
    Send {sends === "pitch" ? "tuning" : "colors"} to board
  </button>
</div>

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
