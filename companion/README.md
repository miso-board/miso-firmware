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
npm run build   # dist/miso-companion.html (single file)
npm run check:all
```

**Chrome or Edge, and it must be the top-level page.** Web Serial is not
delegated into iframes, so an embedded copy cannot talk to hardware. The app
detects that case and reports it rather than failing silently
(`ui.serialBlocked`). Without a board the Mapping tab still works — presets are
edited and saved in the browser — and the Calibrate tab simply waits for one.

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

**Mapping**, the first tab, sets up a preset. At the top the presets are a row of
icons — each a 2×2 tiling of boards in that preset's own colours and layout —
with *Create new* at the end, a bin on the selected one, and Save, Duplicate,
Copy JSON, Import and Send to board beneath.
Below that, in the order each depends on the last: the **tuning** (a row of
common EDOs, a list of the rest, a ruler for the width of the fifth and a cents
box, all showing and setting the same number, plus concert pitch as a note and
a frequency); the **layout**, picked from live icons — 2×2 board tilings drawn in
the preset's own colour rows — with the interval each direction makes, buttons to
turn it in 60° steps, and the anchor note that places it on the keys;
and the **colour rows**. Last, because it is used least, comes the keyboard
itself: every key in its LED colour, labelled by what it sounds (note name, MIDI
number, bend or frequency), with a brush for painting individual keys over the
generated colours and a button to reset them. Keys are drawn
in a single flat colour derived from the LED value (hue kept legible, washed
toward white with brightness). **MIDI** monitors the board's MPE output and
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
`T`, and the push commands `C`, `L`, `Q` and `G` (or `P` and `K` for firmware before 0.10.0). Everything else in the
firmware's table — `l`, `M`, `e`, `r`, `k`, `f`, `F`, `n`, `p<N>`, `B!` — is a
manual diagnostic, driven from a serial terminal or the flashing scripts.

## Firmware version gating

The board reports `fw=miso X.Y.Z` on its `INFO` line, from `FW_VERSION` in
[`../Core/Src/main.c`](../Core/Src/main.c). `fwAtLeast()` in
`src/lib/ui.svelte.ts` compares against five constants in
`src/lib/presets.svelte.ts`:

```ts
export const PITCH_PUSH_FW    = [0, 7, 0] as const;   // 'P'
export const PITCH_REFERENCE_FW = [0, 10, 0] as const;  // 'Q': 'P' plus a concert pitch
export const COLORGEN_PUSH_FW = [0, 8, 0] as const;   // 'K'
export const COLORGEN_PARTITIONS_FW = [0, 9, 0] as const;   // 'K' carrying partitions
export const COLORGEN_LIGHTEN_FW = [0, 10, 0] as const;     // 'G': landmark letters and amount
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
plays together. A layout can be **rotated** in sixths of a turn about the anchor key
(`rotation`, `layoutMatrix` in `tuning.ts`). It is the layout's matrix composed
with a 60° turn of the grid, so it is still a 2×2 integer matrix: the firmware
needs nothing new, and the colours turn with the notes because both take their
geometry from the same place.

Both halves are keyed by **absolute grid coordinate**, not LED
index, so one scheme covers however many boards are attached and survives them
being added, removed or rearranged.

Both halves have the same shape: a **generator that is always present**, with
per-key overrides layered on top. A pitch generator has to be permanent — a
coordinate with no pitch is a silent key, and `P` carries parameters rather than
a table. Pitch overrides are additive exceptions, and retuning never discards
them; per-key pitch entry is modelled and persisted but has no editor yet.

The colour generator holds only what the rows *look like* — palette, first
accidental, brightness, the landmark tint. The landmark lightens some keys toward
white by a chosen amount and is the layout's: C, D and E under Wicki-Hayden, every
key of one chosen letter under Bosanquet, none under the Harmonic Table
(`landmarkFor`, `lightenMask`). Each keeps its own switch and its own amount, so
changing layout loses neither. *Where* they fall is the pitch
generator's layout and anchor, so a key is always coloured by the accidental of
the note it plays; there is no way to colour by one layout while sounding
another.

How the rows are cut is a **partition of the chain of fifths** (… B♭ F C G D A E
B F♯ …): consecutive runs, each with a colour, repeating end to end. There are
three ways to say it. *Accidental* is equal runs of seven starting on F — naturals,
sharps, flats — one palette row each. *Piano* is seven then five, repeating every
twelve fifths: one colour for the naturals and one for the sharps and flats, with
an option to pick out the middle key of each group of three (D, and G♯/A♭) in two
more. *Partitions* is a list of runs of any widths, pinned by naming the natural
that starts one of them. The first two are special cases of the third
(`partitionsFromAccidentals`, `pianoPartitions`), kept as modes of their own
because they are what people mean; Piano with its centre keys is six partitions,
exactly what the firmware's frame carries.
`colorgenFrame` sends a scheme that *is* accidental rows in that form however it
was authored (`asAccidentalScheme` decides), and otherwise as a partition frame,
which needs firmware 0.9.0 and at most six partitions. Anything else is rendered
here and sent as `L` frames, so it needs the app connected.

The colours themselves can **follow the tuning** (`autoColors`, off by default so
a new preset matches a board's boot colours). `src/lib/autoColors.ts` maps the
width of the fifth to a base hue through fixed points — red at 7-EDO, deep orange
at 19, gold at 31, green at a pure fifth, blue at 22, magenta at 5 — and spreads a
scheme's rows around it, the naturals (or the anchored partition) taking the hue
itself. Rows are spaced by how different they *look*, not by hue angle, and by
how they look in two places: on a screen (Oklab distance), and as LED light,
which is linear in the number sent and has purer primaries, so colours bunch
around yellow, cyan and magenta and it is hue that separates two keys. Each row
sits at the first point far enough from the last by all three measures
(`rowSpacing`), which is what widens the steps through the greens, cyans and
purples. The LED primaries are approximate (`LED_PRIMARIES`), not measured. It is a pure function of the fifth and the row, so any number of rows gets
the same colours every time. The preset store writes the result into the stored
palette whenever the tuning or the rows change, so drawing and the wire frames
read plain colours and know nothing of it; editing a swatch by hand turns it off.

A preset starts procedural. Painting a key adds a colour override and
makes it **custom**; **Reset to generated colours** drops the overrides. Older
documents whose colour generator carried its own `layout` and `offset` load with
those dropped, and hand-painted ones with no generator get the default
underneath their painted keys.

Edits are **not saved as they are made**. They show on screen and stream to the
boards at once, but a preset is written to storage only by *Save changes*; the
store keeps each preset's saved form alongside its working copy
(`presetState.saved`), which is what lets adding, deleting or selecting a preset
be written immediately without sweeping another preset's unsaved edits along, and
what `isDirty` compares against. Leaving the page with unsaved edits prompts.

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
into wire bytes, taking its matrix and anchor from the pitch generator. Changing
the pitch layout or anchor therefore re-pushes colours as well as tuning. It returns `null` for a palette longer than the eight entries the
frame carries, and `pushColors()` then falls back to `L` frames. Because only the
second case needs the host at all, a change of grid re-pushes colours *only* when
`colorsAreSelfSufficient()` is false.

**The boards reflecting the app is an invariant, not a chore.** One effect in
`App.svelte` calls `keepBoardsInSync()`, which compares what was last pushed with
what the active preset now asks for — `pitchIsStale()` for the tuning,
`colorsAreStale()` for the colours — and pushes whichever is behind, debounced.
It re-runs whenever either side of those comparisons changes, which covers edits,
preset switches, a board being attached, and the firmware version or topology
arriving a moment after connecting (the push made at connect time happens before
either is known, so it used to be able to send less than the board could take).
*Send to board* remains as a way to force a push.

One rounding detail constrains the split: the landmark tint is applied per key
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
| `npm run check:tuning` | the app's pitch maths against meantonal — golden 31-EDO regression, the anchor convention, wire-frame encoding, validation, and the 12-TET self-check (every bend exactly 8192). 12,495 assertions |
| `npm run check:firmware` | extracts `pitch_for_xy` **verbatim** from `../Core/Src/main.c`, compiles it with clang, and diffs it against the app over nine tunings × three layouts × six concert-pitch offsets × a four-board grid — 20,088 cases, plus the byte-identical-default claim on 124 keys |
| `npm run check:colors` | the same treatment for `colorgen_for_xy`, diffed against the app's real `generateColorAt` (no JS mirror needed) over 1,992 generators × a four-board grid — accidental rows, the piano scheme, partition schemes and landmark tints, 247,008 cases — plus the byte-identical-boot-pattern claim and nineteen validation cases |

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
| `src/lib/autoColors.ts` | colours worked out from the width of the fifth |
| `src/lib/presets.svelte.ts` | the preset document, persistence, push orchestration, version gates |
| `src/lib/mesh.svelte.ts` | `T`/`MESH` parsing, the grid model |
| `src/lib/layout.ts` | the physical board: LED order, sensor positions, screen projection |
| `src/lib/ledColor.ts` | hex/RGB helpers, brightness scaling, white mixing |
| `src/lib/ui.svelte.ts` | connection state, toasts, `fwAtLeast` |

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
