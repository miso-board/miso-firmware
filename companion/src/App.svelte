<script lang="ts">
  import { onMount } from "svelte";
  import * as engine from "./lib/engine";
  import * as serial from "./lib/serial";
  import { startSim, stopSim } from "./lib/sim";
  import { ui, toast, refreshRows, refreshTraces, pushEvent } from "./lib/ui.svelte";
  import LiveBars from "./components/LiveBars.svelte";
  import PressScope from "./components/PressScope.svelte";
  import StatsTable from "./components/StatsTable.svelte";

  let decim = $state("2");

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
          const info = line.match(/scan_hz=(\d+)/);
          if (info) ui.scanHz = +info[1];
          const ev = line.match(/^EV (\d+) (DOWN|UP)(?: vel=(\d+) dt_us=(\d+))?/);
          if (ev) {
            pushEvent({
              key: +ev[1],
              kind: ev[2] as "DOWN" | "UP",
              vel: ev[3] ? +ev[3] : null,
              dtUs: ev[4] ? +ev[4] : null,
              at: new Date().toISOString().slice(11, 23),
            });
          }
          console.log("[miso]", line);
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
    stopSim();
    engine.resetTimeline();
    ui.connected = true;
    ui.scanHz = null;
    await serial.send("i");
    await serial.send(`d${decim}\n`);
    await serial.send("s");
  }

  async function disconnect() {
    ui.connected = false;
    await serial.disconnect();
    engine.resetTimeline();
    startSim();
  }

  async function copyCalibration() {
    const text = engine.calibrationJson(ui.connected);
    try {
      await navigator.clipboard.writeText(text);
      toast("Calibration JSON copied — paste it into the chat.");
    } catch {
      window.prompt("Copy the calibration JSON:", text);
    }
  }

  onMount(() => {
    ui.serialSupported = serial.serialSupported();
    engine.onTracesChanged(refreshTraces);
    startSim();
    const restTimer = setTimeout(captureRest, 800);

    let lastCount = 0;
    const secTimer = setInterval(() => {
      ui.fps = engine.frameCounter - lastCount;
      lastCount = engine.frameCounter;
      ui.badFrames = engine.badFrames;
      if (ui.connected) void serial.send("i");
    }, 1000);
    const rowTimer = setInterval(refreshRows, 200);

    return () => {
      clearTimeout(restTimer);
      clearInterval(secTimer);
      clearInterval(rowTimer);
      stopSim();
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
  <div class="text-lg font-extrabold tracking-[0.18em]">
    MISO
    <small class="block text-[11px] font-normal tracking-[0.14em]" style="color: var(--text-dim)">COMPANION</small>
  </div>
  <span
    class="inline-flex items-center gap-2 rounded-full border px-3 py-1 text-xs tracking-wide"
    style="border-color: var(--line)"
  >
    <span
      class="h-2 w-2 rounded-full"
      style="background: {ui.connected ? 'var(--good)' : 'var(--warn)'}"
    ></span>
    {ui.connected ? "Live" : "Simulated data"}
  </span>
  <div class="flex-1"></div>
  {@render readout(ui.connected ? String(ui.scanHz ?? "…") : "sim", "scan Hz")}
  {@render readout(String(ui.fps), "frames/s")}
  {@render readout(String(ui.badFrames), "bad frames")}
  <button
    class="cursor-pointer rounded-md px-4 py-1.5 font-semibold"
    style="background: var(--accent); color: var(--accent-ink)"
    disabled={!ui.serialSupported}
    onclick={() => (ui.connected ? disconnect() : connect())}
  >{ui.connected ? "Disconnect" : "Connect board"}</button>
</header>

<nav class="flex gap-0.5 border-b px-5" style="background: var(--panel); border-color: var(--line)">
  <button class="border-b-2 px-3.5 py-2 font-semibold" style="border-color: var(--accent)">Calibrate</button>
  {#each ["Key colors", "Pitch mapping"] as t}
    <button class="cursor-default border-b-2 border-transparent px-3.5 py-2 font-semibold opacity-50" disabled style="color: var(--text-dim)">
      {t}<span class="ml-1.5 text-[9px] uppercase tracking-widest" style="color: var(--accent)">soon</span>
    </button>
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
        The claude.ai artifact page wraps this app in a frame that doesn't allow serial access, so
        live data can't work here — this page stays a simulated demo. For live board data, run the
        companion locally: <code class="mono">cd companion && npm run dev</code>, then open
        <b>http://localhost:5173</b> in Chrome.
      {/if}
    </div>
  {/if}
  {#if !ui.connected}
    <div
      class="mb-3.5 flex items-center gap-2.5 rounded-md border border-l-[3px] px-3.5 py-2 text-[13px]"
      style="background: var(--panel-2); border-color: var(--line); border-left-color: var(--warn); color: var(--text-dim)"
    >
      <strong class="tracking-wide" style="color: var(--warn)">SIMULATED</strong>
      Showing generated sensor data so you can see how the instrument works. Connect the board over USB-C for live readings.
    </div>
  {/if}

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
      <button class="btn" onclick={copyCalibration}>Copy calibration JSON</button>
      <div class="flex-1"></div>
      <label class="mono" style="color: var(--text-dim)">
        stream ÷
        <select
          class="btn"
          bind:value={decim}
          onchange={() => serial.send(`d${decim}\n`)}
        >
          {#each ["1", "2", "4", "8"] as d}<option value={d}>{d}</option>{/each}
        </select>
      </label>
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

      {#if ui.events.length > 0}
        <h2 class="m-0 mt-4 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
          Key events (from firmware)
        </h2>
        <ul class="mono m-0 flex list-none flex-col gap-0.5 p-0 text-xs">
          {#each ui.events as ev}
            <li style="color: var(--text-dim)">
              <span>{ev.at}</span>
              key <b class="font-medium" style="color: var(--text)">{String(ev.key).padStart(2, "0")}</b>
              {#if ev.kind === "DOWN"}
                <b class="font-semibold" style="color: var(--accent)">DOWN</b>
                vel <b class="font-medium" style="color: var(--text)">{ev.vel}</b>
                · dt <b class="font-medium" style="color: var(--text)">{((ev.dtUs ?? 0) / 1000).toFixed(1)} ms</b>
              {:else}
                <b style="color: var(--good)">UP</b>
              {/if}
            </li>
          {/each}
        </ul>
      {/if}
    </section>
  </div>
</main>

<footer class="mx-auto max-w-[1180px] px-5 pb-8 text-xs leading-relaxed" style="color: var(--text-dim)">
  Protocol: commands <code class="chip">i</code> info · <code class="chip">s</code>/<code class="chip">x</code>
  stream on/off · <code class="chip">d&lt;N&gt;</code> decimation · <code class="chip">l</code> LED toggle ·
  <code class="chip">r</code> redo rest calibration.
  Scan frame: <code class="chip">A5 5A 01 · u32 t_µs · 31×u16 · u8 checksum</code> (little-endian,
  checksum = byte sum of payload). Firmware: <code class="chip">Core/Src/main.c</code> in the Miso repo.
</footer>

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
  .chip {
    font-family: "IBM Plex Mono", ui-monospace, monospace;
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 4px;
    padding: 1px 5px;
    font-size: 11px;
  }
</style>
