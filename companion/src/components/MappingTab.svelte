<script lang="ts">
  // Setting up the instrument, top to bottom in the order each part depends on
  // the last: which preset, its tuning, the layout over that tuning, the colours
  // that follow from both — and finally the keyboard itself, for the rare key
  // that needs painting by hand.

  import PresetPicker from "./PresetPicker.svelte";
  import TuningPanel from "./TuningPanel.svelte";
  import LayoutPicker from "./LayoutPicker.svelte";
  import GeneratorPanel from "./GeneratorPanel.svelte";
  import KeysPanel from "./KeysPanel.svelte";
  import { ui } from "../lib/ui.svelte";
</script>

{#if !ui.connected}
  <p
    class="mb-3.5 mt-0 rounded-md border border-l-[3px] px-3.5 py-2 text-[13px]"
    style="background: var(--panel-2); border-color: var(--line); border-left-color: var(--warn); color: var(--text-dim)"
  >
    Not connected — colours and tuning reach the board once you connect.
  </p>
{/if}

<PresetPicker />
<TuningPanel />
<LayoutPicker />
<div class="mb-3.5">
  <GeneratorPanel />
</div>
<KeysPanel />
