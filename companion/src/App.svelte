<script lang="ts">
  import { onMount } from "svelte";
  import * as engine from "./lib/engine";
  import * as serial from "./lib/serial";
  import { ui, toast, refreshRows, refreshTraces, pushEvent, clearHeld } from "./lib/ui.svelte";
  import { mesh, parseMeshLine, resetMesh } from "./lib/mesh.svelte";
  import { SENSOR_POS } from "./lib/layout";
  import LiveBars from "./components/LiveBars.svelte";
  import PressScope from "./components/PressScope.svelte";
  import StatsTable from "./components/StatsTable.svelte";
  import MappingTab from "./components/MappingTab.svelte";
  import logo from "./assets/logo.svg";
  import MidiMonitor from "./components/MidiMonitor.svelte";
  import {
    load as loadPresets,
    pushAll,
    resetPushed,
    keepBoardsInSync,
  } from "./lib/presets.svelte";

  // Scan frames are streamed at half the scan rate: plenty for the Calibrate
  // tab's plots, and half the USB traffic.
  const STREAM_DECIMATION = 2;
  let activeTab = $state<"mapping" | "calibrate" | "midi">("mapping");
  // Rest levels are captured once per connection, the first time the Calibrate
  // tab has live scans to take them from.
  let restCaptured = false;

  // The boards always reflect the active preset. This re-runs whenever the
  // preset, the grid or what the firmware can be sent changes.
  $effect(() => keepBoardsInSync());

  // Only the Calibrate tab consumes scan frames, and streaming them is what
  // makes the LED chain flicker (the data line has no voltage margin — see the
  // README). So stream only while that tab is showing.
  $effect(() => {
    if (!ui.connected) return;
    const calibrating = activeTab === "calibrate";
    void serial.send(calibrating ? "s" : "x");
    if (calibrating && !restCaptured) {
      restCaptured = true;
      setTimeout(captureRest, 800);
    }
  });

  function captureRest() {
    toast("Capturing rest — hands off the keys…");
    engine.captureRest(1000, (n) =>
      toast(n >= 10 ? `Rest captured from ${n} scans.` : "Not enough data to capture rest."),
    );
  }

  async function connect() {
    let ok = false;
    try {
      ok = await serial.connect({
        onLine(line) {
          if (parseMeshLine(line)) return;
          // Capability gate for `P`: retuning needs 0.7.0. Sending a binary
          // command to firmware that lacks it is worse than not sending it —
          // the payload bytes get re-read as commands.
          const fw = line.match(/fw=miso (\d+\.\d+\.\d+)/);
          if (fw) ui.fwVersion = fw[1];
          const ev = line.match(
            /^EV (\d+) (DOWN|UP)(?: vel=(\d+) dt_us=(\d+))?(?: x=(-?\d+) y=(-?\d+))?/,
          );
          if (ev) {
            const sensor = +ev[1];
            // Firmware before 0.6.0 omits the coordinate; assume the attached
            // board, which is the only one such firmware can see anyway.
            const local = SENSOR_POS[sensor] ?? [0, 0];
            pushEvent({
              key: sensor,
              kind: ev[2] as "DOWN" | "UP",
              vel: ev[3] ? +ev[3] : null,
              dtUs: ev[4] ? +ev[4] : null,
              x: ev[5] !== undefined ? +ev[5] : local[0],
              y: ev[6] !== undefined ? +ev[6] : local[1],
            });
          }
        },
        onDisconnect() {
          void disconnect();
          toast("Board disconnected.");
        },
      });
    } catch {
      ui.serialBlocked = true; // SecurityError: embedded context — open in own tab
      return;
    }
    if (!ok) return;
    engine.resetTimeline();
    ui.connected = true;
    ui.fwVersion = null;
    resetPushed();
    await serial.send("i");
    await serial.send("T"); // discover the grid before the first colour push
    await serial.send(`d${STREAM_DECIMATION}\n`);
    if (activeTab === "calibrate") await serial.send("s");
    // Restore the active preset: tuning first, so the first note played is
    // already in tune, then the LEDs.
    await pushAll();
  }

  async function disconnect() {
    ui.connected = false;
    await serial.disconnect();
    engine.resetTimeline();
    clearHeld(); // no key-up is coming for anything still down
    resetMesh();
    ui.fwVersion = null;
    resetPushed(); // the board's tuning may be anything by the time we return
    restCaptured = false;
  }

  onMount(() => {
    ui.serialSupported = serial.serialSupported();
    engine.onTracesChanged(refreshTraces);
    loadPresets();
    const secTimer = setInterval(() => {
      if (ui.connected) {
        void serial.send("i");
        void serial.send("T"); // boards can be hot-plugged at any time
      }
    }, 1000);
    const rowTimer = setInterval(refreshRows, 200);

    return () => {
      clearInterval(secTimer);
      clearInterval(rowTimer);
    };
  });
</script>

{#snippet readout(value: string, label: string)}
  <div class="flex min-w-[86px] flex-col items-end gap-px">
    <span class="mono text-base font-semibold">{value}</span>
    <span class="text-[10px] uppercase tracking-widest" style="color: var(--text-dim)">{label}</span>
  </div>
{/snippet}

<header
  class="flex items-center gap-4 border-b px-5 py-3"
  style="background: var(--panel); border-color: var(--line)"
>
  <div class="flex items-center gap-2.5">
    <img src={logo} alt="Miso" class="-my-2 block h-12 w-12" />
    <span class="text-[13px] font-semibold tracking-[0.18em]" style="color: var(--text-dim)">COMPANION</span>
  </div>
  <span
    class="inline-flex items-center gap-2 rounded-full border px-3 py-1 text-xs tracking-wide"
    style="border-color: var(--line)"
  >
    <span
      class="h-2 w-2 rounded-full"
      style="background: {ui.connected ? 'var(--good)' : 'var(--text-dim)'}"
    ></span>
    {ui.connected ? "Connected" : "Not connected"}
  </span>
  <div class="flex-1"></div>
  {@render readout(ui.connected && mesh.seen ? String(mesh.boards.length) : "—", "boards")}
  <button
    class="cursor-pointer rounded-md px-4 py-1.5 font-semibold"
    style="background: var(--accent); color: var(--accent-ink)"
    disabled={!ui.serialSupported}
    onclick={() => (ui.connected ? disconnect() : connect())}
  >{ui.connected ? "Disconnect" : "Connect board"}</button>
</header>

<nav class="flex gap-0.5 border-b px-5" style="background: var(--panel); border-color: var(--line)">
  {#each [["mapping", "Mapping"], ["calibrate", "Calibrate"], ["midi", "MIDI"]] as [id, label]}
    <button
      class="cursor-pointer border-b-2 px-3.5 py-2 font-semibold"
      style="border-color: {activeTab === id ? 'var(--accent)' : 'transparent'};
             color: {activeTab === id ? 'var(--text)' : 'var(--text-dim)'}"
      role="tab"
      aria-selected={activeTab === id}
      onclick={() => (activeTab = id as typeof activeTab)}
    >{label}</button>
  {/each}
</nav>

<main class="mx-auto max-w-[1180px] px-5 pt-4 pb-7">
  {#if !ui.serialSupported || ui.serialBlocked}
    <div
      class="mb-3.5 rounded-md border-l-[3px] px-3.5 py-2.5 text-sm"
      style="background: var(--panel-2); border-color: var(--crit)"
    >
      {#if !ui.serialSupported}
        This browser has no Web Serial support — open this page in <b>Chrome or Edge</b> to connect the board.
      {:else}
        This page is embedded in a frame that doesn't allow serial access — open the companion in
        its own browser tab to connect the board.
      {/if}
    </div>
  {/if}
  {#if activeTab === "mapping"}
    <MappingTab />
  {:else if activeTab === "calibrate" && !ui.connected}
    <section
      class="rounded-lg border p-4 text-sm"
      style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow); color: var(--text-dim)"
    >
      Connect the board over USB-C to see its live sensor levels, per-key statistics and press
      captures.
    </section>
  {:else if activeTab === "calibrate"}
  <section
    class="mb-3.5 rounded-lg border p-4"
    style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
  >
    <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
      Live levels — 31 keys
    </h2>
    <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
      Raw 12-bit ADC counts per DRV5055 sensor (wire order = LED chain order). Copper ticks mark the
      min/max watermarks; the dashed line is the captured rest level.
    </p>
    <LiveBars />
    <div class="mt-2.5 flex flex-wrap items-center gap-2">
      <button class="btn" onclick={captureRest}>Capture rest <span class="mono">(1 s median)</span></button>
      <button class="btn" onclick={() => engine.resetWatermarks()}>Reset watermarks</button>
      <div class="flex-1"></div>
      <button class="btn" onclick={() => serial.send("l")} disabled={!ui.connected}>LEDs on/off</button>
    </div>
  </section>

  <div class="grid items-start gap-3.5 max-[900px]:grid-cols-1 min-[900px]:grid-cols-[minmax(340px,5fr)_minmax(380px,6fr)]">
    <section
      class="rounded-lg border p-4"
      style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
    >
      <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
        Per-key statistics
      </h2>
      <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
        σ over the last 128 samples — read it with hands off the keys as the noise floor.
      </p>
      <StatsTable />
    </section>

    <section
      class="rounded-lg border p-4"
      style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
    >
      <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
        Press capture
      </h2>
      <p class="m-0 mb-2.5 text-xs" style="color: var(--text-dim)">
        Triggers when any key leaves its rest band; keeps the last 8 presses. Transit = time from 20%
        to 70% of the excursion — the number the velocity engine will be built on.
      </p>
      <PressScope />
      <div class="mt-2.5 flex gap-2">
        <button class="btn" onclick={() => engine.clearTraces()}>Clear captures</button>
      </div>
    </section>
  </div>
  {:else}
    <MidiMonitor />
  {/if}
</main>

{#if ui.toast}
  <div
    class="fixed bottom-4 left-1/2 -translate-x-1/2 rounded-md border px-4 py-2 text-[13px]"
    style="background: var(--panel); border-color: var(--accent-dim); box-shadow: var(--shadow)"
    role="status"
  >{ui.toast}</div>
{/if}

<style>
  .btn {
    font: inherit;
    color: var(--text);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 7px 14px;
    cursor: pointer;
  }
  .btn:hover:not(:disabled) {
    border-color: var(--accent-dim);
  }
  .btn:disabled {
    opacity: 0.45;
    cursor: default;
  }
</style>
