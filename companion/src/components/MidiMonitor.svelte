<script lang="ts">
  import { onMount } from "svelte";
  import { mesh, gridCells } from "../lib/mesh.svelte";
  import {
    LAYOUTS, pitchAt, noteName, midiFor, centsFromBend, MPE_BEND_SEMITONES,
  } from "../lib/tuning";
  import { toast } from "../lib/ui.svelte";

  interface Row {
    at: string;
    kind: "on" | "off" | "bend" | "cc" | "other";
    channel: number;
    text: string;
    /** Set when a note-on's note+bend matched (or failed to match) a key. */
    match?: { key: string; name: string; ok: boolean; deltaCents: number };
  }

  let supported = $state(true);
  let connected = $state(false);
  let portName = $state("");
  let rows = $state<Row[]>([]);
  let lastBend = $state<number[]>(Array(16).fill(8192));
  let access: MisoMIDIAccess | null = null;

  // What each key in the grid should produce, computed independently via
  // meantonal — so a note from a neighbouring board is checked against the
  // pitch at its ABSOLUTE coordinate, which is the whole point of the mesh.
  const expected = $derived(
    gridCells().map((c) => {
      const p = pitchAt(LAYOUTS["wicki-hayden"], c.x, c.y);
      return {
        label: mesh.boards.length > 1 ? `${c.x},${c.y}` : `S${String(c.sensor).padStart(2, "0")}`,
        name: noteName(p),
        ...midiFor(p),
      };
    }),
  );

  function stamp() {
    return new Date().toISOString().slice(11, 23);
  }

  function push(r: Row) {
    rows = [r, ...rows].slice(0, 16);
  }

  function onMessage(e: MisoMIDIMessageEvent) {
    const [status, d1, d2] = e.data;
    const type = status & 0xf0;
    const channel = (status & 0x0f) + 1;

    if (type === 0xe0) {
      const bend = d1 | (d2 << 7);
      lastBend[channel - 1] = bend;
      push({
        at: stamp(), kind: "bend", channel,
        text: `bend ${bend} (${centsFromBend(bend).toFixed(1)} cents)`,
      });
    } else if (type === 0x90 && d2 > 0) {
      const bend = lastBend[channel - 1];
      const cents = centsFromBend(bend);
      // Which key should have produced this note+bend?
      let best = expected[0];
      let bestErr = Infinity;
      for (const ex of expected) {
        const err = Math.abs(ex.note - d1) * 1000 + Math.abs(ex.cents - cents);
        if (err < bestErr) { bestErr = err; best = ex; }
      }
      const deltaCents = cents - best.cents;
      push({
        at: stamp(), kind: "on", channel,
        text: `note ${d1} vel ${d2}`,
        match: {
          key: best.label, name: best.name,
          ok: best.note === d1 && Math.abs(deltaCents) < 1.5,
          deltaCents,
        },
      });
    } else if (type === 0x80 || (type === 0x90 && d2 === 0)) {
      push({ at: stamp(), kind: "off", channel, text: `note ${d1} off` });
    } else if (type === 0xb0) {
      push({ at: stamp(), kind: "cc", channel, text: `CC ${d1} = ${d2}` });
    }
  }

  function attach(acc: MisoMIDIAccess) {
    let found = false;
    for (const input of acc.inputs.values()) {
      input.onmidimessage = onMessage;
      if (!found && /miso|stm/i.test(input.name ?? "")) {
        portName = input.name ?? "";
        found = true;
      }
    }
    if (!found) {
      const first = [...acc.inputs.values()][0];
      portName = first ? (first.name ?? "unnamed") : "";
    }
    connected = acc.inputs.size > 0;
  }

  async function connect() {
    try {
      access = await navigator.requestMIDIAccess({ sysex: false });
    } catch {
      toast("MIDI access was refused.");
      return;
    }
    attach(access);
    access.onstatechange = () => attach(access!);
    if (!connected) toast("No MIDI inputs found — is the board plugged in?");
  }

  onMount(() => {
    supported = typeof navigator.requestMIDIAccess === "function";
  });
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    MIDI monitor {portName ? `— ${portName}` : ""}
  </h2>
  <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
    Listens to the board's MPE output and checks each note against what meantonal says that key
    should sound. Bend is read at ±{MPE_BEND_SEMITONES} semitones, matching the firmware.
  </p>

  {#if !supported}
    <div class="rounded-md border-l-[3px] px-3.5 py-2.5 text-sm" style="background: var(--panel-2); border-color: var(--crit)">
      This browser has no Web MIDI support — use Chrome or Edge.
    </div>
  {:else}
    <div class="mb-3 flex items-center gap-2">
      <button class="btn primary" onclick={connect} disabled={connected}>
        {connected ? "Listening" : "Connect MIDI"}
      </button>
      <button class="btn" onclick={() => (rows = [])}>Clear</button>
    </div>

    <div class="grid gap-3.5 max-[900px]:grid-cols-1 min-[900px]:grid-cols-2">
      <div>
        <h3 class="m-0 mb-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">Incoming</h3>
        <ul class="mono m-0 flex list-none flex-col gap-0.5 p-0 text-xs">
          {#each rows as r}
            <li class="flex flex-wrap items-center gap-2" style="color: var(--text-dim)">
              <span>{r.at}</span>
              <span style="color: var(--text)">ch{String(r.channel).padStart(2, "0")}</span>
              <span style="color: {r.kind === 'on' ? 'var(--accent)' : r.kind === 'off' ? 'var(--good)' : 'var(--text-dim)'}">{r.text}</span>
              {#if r.match}
                <span style="color: {r.match.ok ? 'var(--good)' : 'var(--crit)'}">
                  {r.match.ok ? "✓" : "✗"} key {r.match.key} {r.match.name}
                  {#if !r.match.ok}({r.match.deltaCents.toFixed(1)}¢ off){/if}
                </span>
              {/if}
            </li>
          {:else}
            <li style="color: var(--text-dim)">Nothing yet — connect, then press a key.</li>
          {/each}
        </ul>
      </div>

      <div>
        <h3 class="m-0 mb-1 text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">
          Expected (Wicki-Hayden, 31-EDO)
        </h3>
        <div class="max-h-[300px] overflow-auto rounded-md border" style="border-color: var(--line)">
          <table class="mono w-full border-collapse text-xs">
            <thead>
              <tr>
                {#each ["key", "note", "midi", "bend", "cents"] as h, i}
                  <th class="sticky top-0 px-2 py-1 font-sans text-[10px] uppercase tracking-widest {i === 0 ? 'text-left' : 'text-right'}"
                      style="background: var(--panel-2); color: var(--text-dim); border-bottom: 1px solid var(--line)">{h}</th>
                {/each}
              </tr>
            </thead>
            <tbody>
              {#each expected as e}
                <tr>
                  <td class="px-2 py-0.5" style="border-bottom: 1px solid var(--grid)">{e.label}</td>
                  <td class="px-2 py-0.5 text-right" style="border-bottom: 1px solid var(--grid); color: var(--text)">{e.name}</td>
                  <td class="px-2 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{e.note}</td>
                  <td class="px-2 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{e.bend}</td>
                  <td class="px-2 py-0.5 text-right" style="border-bottom: 1px solid var(--grid)">{e.cents.toFixed(1)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="m-0 mt-1 text-xs" style="color: var(--text-dim)">
          {expected.length} keys{#if mesh.boards.length > 1} across {mesh.boards.length} boards{/if}
        </p>
      </div>
    </div>
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
</style>
