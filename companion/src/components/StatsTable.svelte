<script lang="ts">
  import { ui } from "../lib/ui.svelte";

  const fmt = (v: number, empty = "—") => (isFinite(v) && !isNaN(v) ? String(v) : empty);
</script>

<div class="max-h-[380px] overflow-auto rounded-md border" style="border-color: var(--line)">
  <table class="mono w-full border-collapse text-xs">
    <thead>
      <tr>
        {#each ["key", "now", "rest", "min", "max", "span", "σ"] as h, i}
          <th
            class="sticky top-0 px-2.5 py-1.5 font-sans text-[10px] font-semibold uppercase tracking-widest {i === 0 ? 'text-left' : 'text-right'}"
            style="background: var(--panel-2); color: var(--text-dim); border-bottom: 1px solid var(--line)"
          >{h}</th>
        {/each}
      </tr>
    </thead>
    <tbody>
      {#each ui.rows as row, i}
        {@const span = row.min <= row.max ? row.max - row.min : NaN}
        <tr>
          <td class="px-2.5 py-0.5 text-left" style="border-bottom: 1px solid var(--grid)">S{String(i).padStart(2, "0")}</td>
          <td
            class="px-2.5 py-0.5 text-right {row.active ? 'font-semibold' : ''}"
            style="border-bottom: 1px solid var(--grid); {row.active ? 'color: var(--accent)' : ''}"
          >{row.cur}</td>
          <td class="px-2.5 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{fmt(row.rest)}</td>
          <td class="px-2.5 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{fmt(row.min)}</td>
          <td class="px-2.5 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{fmt(row.max)}</td>
          <td
            class="px-2.5 py-0.5 text-right"
            style="border-bottom: 1px solid var(--grid); {ui.connected && span > 0 && span < 100 ? 'color: var(--crit)' : ''}"
          >{fmt(span)}</td>
          <td class="px-2.5 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{row.sigma ? row.sigma.toFixed(1) : "—"}</td>
        </tr>
      {/each}
    </tbody>
  </table>
</div>
