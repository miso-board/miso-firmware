<script lang="ts">
  // How the board sends notes: MPE, or plain notes for a synth tuned by Scala
  // files — with those files to download.
  //
  // A setting of the app rather than of a preset: it describes the synth on the
  // other end of the cable.

  import { gridCells } from "../lib/mesh.svelte";
  import { ui } from "../lib/ui.svelte";
  import {
    activePitch, output, setOutputMode, canPushOutput, OUTPUT_FW,
  } from "../lib/presets.svelte";
  import {
    scalaMap, banksFor, channelOfBank, scalaName, sclFile, kbmFile, scalaValue, bankOf,
  } from "../lib/scala";

  const gen = $derived(activePitch().generator);
  const map = $derived(scalaMap(gen));
  const cells = $derived(gridCells());
  const banks = $derived(map ? banksFor(map, cells) : []);
  // Keys beyond the sixteen channels' reach, which the board cannot send.
  const unreachable = $derived(
    map ? cells.filter((c) => channelOfBank(bankOf(scalaValue(map, c.x, c.y))) === null).length : 0,
  );

  // What was last downloaded, so the panel can say when the synth's files are
  // out of date. The files depend only on the tuning and concert pitch.
  let downloaded = $state<string | null>(null);
  const filesNow = $derived(
    map ? sclFile(map) + banks.map((b) => kbmFile(map, gen, b)).join("") : null,
  );
  const stale = $derived(downloaded !== null && filesNow !== null && downloaded !== filesNow);

  function save(name: string, text: string) {
    const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  function downloadAll() {
    if (!map) return;
    save(`${scalaName(map)}.scl`, sclFile(map));
    for (const b of banks) save(`${scalaName(map)} ch${channelOfBank(b)}.kbm`, kbmFile(map, gen, b));
    downloaded = filesNow;
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    MIDI output
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    MPE tunes every note with its own pitch bend. Scala sends plain notes, one per step of the EDO,
    for synths that load .scl and .kbm files instead. This applies to every preset.
  </p>

  <div class="flex flex-wrap items-center gap-2">
    <div class="flex items-center gap-1" role="radiogroup" aria-label="MIDI output">
      {#each [["mpe", "MPE"], ["scala", "Scala"]] as [id, label]}
        <button
          class="btn"
          role="radio"
          aria-checked={output.mode === id}
          style={output.mode === id ? "border-color: var(--accent); color: var(--accent)" : ""}
          onclick={() => setOutputMode(id as "mpe" | "scala")}
        >{label}</button>
      {/each}
    </div>

    {#if output.mode === "scala" && map}
      <div class="flex-1"></div>
      <button class="btn primary" onclick={downloadAll}>
        Download .scl and .kbm{banks.length > 1 ? ` (${banks.length})` : ""}
      </button>
    {/if}
  </div>

  {#if output.mode === "scala"}
    {#if !map}
      <p class="note" style="border-left-color: var(--warn)">
        Scala output needs an EDO. This tuning isn't one, so the board sends MPE until you pick an EDO.
      </p>
    {:else}
      <p class="m-0 mt-3 text-xs" style="color: var(--text-dim)">
        Load <b class="mono" style="color: var(--text)">{scalaName(map)}.scl</b> into the synth, with
        the .kbm for its channel. C4 is note 60 on channel 1.
        {#if banks.length > 1}
          This grid reaches past MIDI's 128 notes, so it uses {banks.length} channels —
          {banks.map((b) => channelOfBank(b)).join(", ")}. Run one instance of the synth per channel,
          each set to receive only its channel, with the same .scl and that channel's .kbm.
        {/if}
      </p>
      {#if unreachable > 0}
        <p class="note" style="border-left-color: var(--warn)">
          {unreachable} key{unreachable === 1 ? " is" : "s are"} beyond the sixteen channels and stay silent.
        </p>
      {/if}
      {#if stale}
        <p class="note" style="border-left-color: var(--warn); color: var(--warn)">
          The tuning or concert pitch has changed since these were downloaded — download them again and
          reload them in the synth.
        </p>
      {/if}
      {#if ui.connected && !canPushOutput()}
        <p class="note" style="border-left-color: var(--crit)">
          This firmware (<span class="mono">{ui.fwVersion ?? "unknown"}</span>) can only send MPE; update
          to <b style="color: var(--text)">{OUTPUT_FW.join(".")}</b> or newer for Scala output.
        </p>
      {/if}
    {/if}
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
  .btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-ink); font-weight: 600; }
  .note {
    margin: 12px 0 0;
    border: 1px solid var(--line);
    border-left-width: 3px;
    border-radius: 6px;
    padding: 8px 14px;
    font-size: 12px;
    color: var(--text-dim);
    background: var(--panel-2);
  }
</style>
