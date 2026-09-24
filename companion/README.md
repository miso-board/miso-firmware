# Miso companion

The authoring and diagnostic front end for [Miso](../README.md), talking to a
board over its USB CDC port with [Web Serial](https://developer.mozilla.org/docs/Web/API/Web_Serial_API)
and listening to its MIDI output over [Web MIDI](https://developer.mozilla.org/docs/Web/API/Web_MIDI_API).

Svelte 5 + TypeScript + Tailwind, built with Vite into a single HTML file. The
only runtime dependency is [meantonal](https://meantonal.org/), which owns every
question about what a pitch *is* — the app never reimplements the firmware's
arithmetic, which is what makes the cross-checks meaningful.

It lives in the firmware repository on purpose; see
[Why this is not its own repository](#why-this-is-not-its-own-repository).

## Running it

```sh
npm install
npm run dev     # → http://localhost:5173, connects to the board
npm run build   # dist/miso-companion.html (single file, publishable as artifact demo)
npm run check:all
```

**Chrome or Edge, and it must be the top-level page.** Web Serial is not
delegated into iframes, so the published artifact copy runs in simulated mode
(`src/lib/sim.ts`) and only a local `npm run dev` talks to real hardware. The app
detects the embedded case and reports it rather than failing silently
(`ui.serialBlocked`, `App.svelte:80`).

Requirements beyond a browser:

| For | Needs |
|---|---|
| `dev`, `build`, `check`, `check:tuning` | Node 24 — `check-tuning.mjs` relies on native TypeScript type-stripping, and `scripts/ts-resolve.mjs` on `node:module`'s `registerHooks` |
| `check:firmware`, `check:colors` | additionally `python3` and `clang`, plus this being inside a firmware checkout |

## The four tabs

**Calibrate** shows live per-key levels with min/max watermarks, per-key
statistics (rest / min / max / noise σ), and auto-triggered press-waveform
capture with 20→70% transit times. It is the only tab that consumes scan frames,
and streaming them is expensive, so the app sends `s` on entering it and `x` on
leaving.

It is also **master-only**: the firmware forwards key *events* between boards,
not raw scans, and remote sensor data at full rate would be 93 kB/s against a
46 kB/s link. Live levels, stats and press capture therefore show the
USB-connected board's own keys only.

**Key colors** is a click-to-paint view of the instrument in axial hex. **Pitch
mapping** shows what every key sounds — note name, MIDI number, bend, frequency —
and pushes the tuning to the board. **MIDI** monitors the board's MPE output and
checks each received note against what meantonal says that key should sound, at
its *absolute* coordinate, so a note from a neighbouring board is verified like
any other.

## How it talks to the board

The [firmware README](../README.md#sensor-streaming-and-the-cdc-command-set) carries
the authoritative command table and frame formats. This is the client's half.

**Connecting.** `navigator.serial.requestPort()` filters on
`usbVendorId: 0x0483` (STMicroelectronics) — **the VID only, not the PID**, so
the picker also lists a board sitting in the ROM bootloader rather than hiding
it. The baud rate passed to `open()` is nominal; CDC ignores it.

**Receiving.** One byte stream carries two interleaved things, and `feed()` in
`src/lib/serial.ts` demultiplexes them: a run beginning `A5` is a 69-byte binary
scan frame, and any other printable run is an ASCII status line. A frame failing
its checksum is counted and the parser resyncs a byte at a time rather than
discarding the buffer, so a corrupted frame costs one frame.

Status lines the app parses:

| Line | Parsed in | For |
|---|---|---|
| `INFO fw=miso X.Y.Z scan_hz=… …` | `App.svelte` | the scan rate readout, and the version gate below |
| `EV <n> DOWN vel=… dt_us=… x=… y=…` | `App.svelte` | key events; `x`/`y` are optional, falling back to `SENSOR_POS` for firmware before 0.6.0 |
| `MESH self=… \| port=… \| board=… \| stat …` | `src/lib/mesh.svelte.ts` | the grid model |

**Sending.** The app uses a deliberately small subset: `i`, `s`, `x`, `d<N>`,
`T`, and the four push commands `C`, `L`, `P`, `K`. Everything else in the
firmware's table — `l`, `M`, `e`, `r`, `k`, `f`, `F`, `n`, `p<N>`, `B!` — is a
manual diagnostic, driven from a serial terminal or the flashing scripts.

## Firmware version gating

The board reports `fw=miso X.Y.Z` on its `INFO` line, from `FW_VERSION` in
[`../Core/Src/main.c`](../Core/Src/main.c). `fwAtLeast()` in
`src/lib/ui.svelte.ts` compares against two constants in
`src/lib/presets.svelte.ts`:

```ts
export const PITCH_PUSH_FW    = [0, 7, 0] as const;   // 'P'
export const COLORGEN_PUSH_FW = [0, 8, 0] as const;   // 'K'
```

**These gates are load-bearing, not polite.** A board that predates `P` does not
ignore it — it ignores the `P` byte and then reads the following 16 payload bytes
*as commands*. `0x43` (`C`) and `0x4C` (`L`) are entirely reachable values in a
millicent count, a signed matrix entry or a palette channel, so an ungated push
can drop an old board into a 97-byte colour collect that swallows the stream.
Connecting to older firmware is otherwise fine: with no topology seen the app
falls back to a single board at the origin and the original `C` colour frame.

When changing either side of a wire format, bump `FW_VERSION` and add the gate
here in the same commit. Nothing enforces this pairing.

## Presets

A **preset** is how the instrument is set up: an LED mapping *and* a pitch
mapping, with room for further preset-scoped settings. One selector serves both
tabs, because switching preset changes what the instrument looks like and what it
plays together. Both halves are keyed by **absolute grid coordinate**, not LED
index, so one scheme covers however many boards are attached and survives them
being added, removed or rearranged.

The two halves differ in one structural respect. A colour layer's generator is
**optional** — hand-painting freezes it, because an unpainted coordinate can
safely be black. A pitch map's generator is **mandatory and permanent**: a
coordinate with no pitch is a silent key, and `P` carries parameters rather than
a table, so freezing one would leave nothing to send. Pitch overrides are
additive exceptions layered on top, and retuning never discards them. Per-key
pitch entry is modelled and persisted but has no editor yet.

Stored in `localStorage` under `miso-presets-v1`, reading `miso-color-maps-v2`
and a v1 key as fallbacks and leaving both in place as backups
(`presets.svelte.ts:117`).

### Where a colour scheme is evaluated

This is the part worth understanding before changing `pushColors()`.

A **purely procedural** layer goes to the board as one 40-byte `K` frame and is
evaluated *there*, per key, on every board in the grid — so the app can be closed
and a board attached later still comes up right. A **hand-painted or mixed**
layer cannot be expressed in parameters, so it is rendered here and sent as a
93-byte `L` frame per board, after the `K`, so paint lands on top of the
generated base.

`colorgenFrame` in `src/lib/colorMaps.ts` is the one place that folds a generator
into wire bytes. It returns `null` for a palette longer than the eight entries the
frame carries, and `pushColors()` then falls back to `L` frames. Because only the
second case needs the host at all, the 1 Hz topology poll re-pushes on a grid
change *only* when `colorsAreSelfSufficient()` is false.

One rounding detail constrains the split: the C/D/E tint is applied per key
*before* brightness scaling and rounds on the way, so brightness cannot be folded
into the palette here. The palette travels at full brightness and the board
scales it.

## Grid awareness

The app polls `T` once a second and models the discovered mesh in
`src/lib/mesh.svelte.ts`, so a tiled set of boards renders as the single
continuous instrument it is — one SVG, every key placed by its absolute
coordinate, each board captioned by UID when there is more than one. The header
shows a live board count.

## Cross-checks

Four checks, because the pitch and colour pipelines each span two languages and a
wire format:

| Command | What it proves |
|---------|----------------|
| `npm run check` | `svelte-check`: types across the app |
| `npm run check:tuning` | the app's pitch maths against meantonal — golden 31-EDO regression, the anchor convention, wire-frame encoding, validation, and the 12-TET self-check (every bend exactly 8192). 8,084 assertions |
| `npm run check:firmware` | extracts `pitch_for_xy` **verbatim** from `../Core/Src/main.c`, compiles it with clang, and diffs it against the app over nine tunings × both layouts × a four-board grid — 2,232 cases, plus the byte-identical-default claim on 124 keys |
| `npm run check:colors` | the same treatment for `colorgen_for_xy`, diffed against the app's real `generateColorAt` (no JS mirror needed) over 320 generators × a four-board grid — 39,680 cases, plus the byte-identical-boot-pattern claim and eight validation cases |

The 12-TET case is the one to keep: at a 700¢ fifth a wrong coefficient would not
collapse to equal temperament, a wrong anchor would disagree with meantonal's own
`midi` accessor, and any rounding bias would show as a bend off 8192.

The two Python scripts locate the firmware as `../Core/Src/main.c`, relative to
this directory. They are the only automated check on the firmware's arithmetic,
and they run from here rather than from the firmware side because the reference
they diff against is this app's live code — `generateColorAt` and `colorgenFrame`
imported directly, not transcribed.

**One duplication nothing checks:** `LED_FOR_SENSOR` in `src/lib/layout.ts` and
`led_for_sensor[]` in `../Core/Src/main.c` are the same physical fact, traced
from the board by hand on 2026-09-08 and written down twice. Change one and you
must change the other; nothing will tell you if you don't, and the symptom is lit
keys in the wrong places rather than an error.

## Module map

| Module | Owns |
|---|---|
| `src/lib/serial.ts` | Web Serial transport, frame parsing, resync |
| `src/lib/engine.ts` | frame ingestion, per-key stats, press capture |
| `src/lib/tuning.ts` | pitch via meantonal, layout bases, note names, `pitchFrame` (`P`) |
| `src/lib/colorMaps.ts` | colour semantics, `generateColorAt`, `colorgenFrame` (`K`) |
| `src/lib/presets.svelte.ts` | the preset document, persistence, push orchestration, version gates |
| `src/lib/mesh.svelte.ts` | `T`/`MESH` parsing, the grid model |
| `src/lib/layout.ts` | the physical board: LED order, sensor positions, screen projection |
| `src/lib/ledColor.ts` | hex/RGB helpers, brightness scaling, white mixing |
| `src/lib/ui.svelte.ts` | connection state, toasts, `fwAtLeast` |
| `src/lib/sim.ts` | synthetic frames for the demo build |

`webserial.d.ts` and `webmidi.d.ts` are hand-written typings; neither API ships
with TypeScript's DOM lib.

## Why this is not its own repository

It was extracted once, in `3db6f26`, and the split was reverted. Two reasons:

1. **The work is genuinely joint.** Of the eight commits that had ever touched
   the companion, six also touched firmware source — and the two that didn't were
   still one side of a conversation with it.
2. **It broke the cross-checks**, which read the firmware's `main.c` from a
   relative path, and which are the only automated test of firmware correctness
   that exists.

The runtime decoupling is real and stays: the version gates above exist precisely
because a board in the field may be older than the app driving it. That is a
property of the protocol, not an argument about source control.
