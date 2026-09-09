This repository is the firmware for Miso, a modular, isomorphic musical keyboard with the following features:
- An STM32G431KBU6 microcontroller as its brain.
- Two CD74HC4067 analog multiplex chips connected to the 31 Texas Instruments DRV5055 Hall effect sensors that power the velocity-sensitive key detection.
- A chain of 31 SK6812mini-e RGB LEDs that backlight the keys and allow users to see the key mapping they are currently using visually.
- Four pogo pin connectors, 2 male and 2 female, to the top, bottom, left and right sides of the device which transmit +5V power and allow for UART communication with adjacent boards when connected together. The STM32G431KBU6 only has 3 built-in UART capable pin pairs, so one of these four UART connections will be achieved via bit-banging.

## Flashing firmware over USB-C (no SWD header needed)

The firmware supports entering the STM32G4's built-in ROM bootloader, which
enumerates as a USB DFU device — so updates can be flashed over the USB-C port
without opening the case.

To enter DFU mode, **double-tap the reset button** (two quick presses, within
~half a second). The LEDs will stay dark and the board shows up as
"STM32 BOOTLOADER" over USB:

    system_profiler SPUSBDataType | grep -i bootloader

Then build and flash in one step:

    ./flash-usb.sh

(Uses the ARM GCC and STM32CubeProgrammer CLI bundled inside STM32CubeIDE.app;
the `-g` flag restarts the application after flashing.)

Firmware can also reboot itself into DFU mode by calling
`Bootloader_RequestDFU()` — intended for a future "enter DFU" command over
MIDI/serial so updates need no button press at all.

How it works: a magic word in a `.noinit` RAM section (which survives NRST
resets) is armed for the first 500 ms after boot (`BOOTLOADER_TAP_WINDOW` in
`main.c`). If a second reset arrives inside that window, `bootloader_check()`
in `main.c` jumps to system memory at `0x1FFF0000` before any clocks or
peripherals are configured. SWD via the 5-pin header still works as before for
debugging.

## Sensor streaming & the companion app

The firmware scans all 31 Hall sensors in a tight loop (target ≥1.5 kHz; the
measured rate is reported by the `i` command) and exposes a USB CDC serial
interface on the same USB-C port:

| Command | Effect |
|---------|--------|
| `i`     | Info line: firmware version, measured scan Hz, stream state |
| `s` / `x` | Start / stop streaming scan frames |
| `d<N>`  | Stream every Nth scan (default 2) |
| `l`     | Toggle the LED velocity feedback (off for clean noise measurements) |
| `M`     | Re-send the MPE configuration (zone + pitch-bend range) |
| `e`     | Toggle the `EV` text event lines |
| `r`     | Redo the boot-time rest calibration (hands off the keys for ~0.1 s) |
| `C` + 93 bytes | Set all LED background colors: 31 × RGB, LED-chain order (a stalled frame aborts after 200 ms) |

Scan frame format: `A5 5A 01` · `u32 t_µs` · `31×u16 raw` · `u8 checksum`
(little-endian; checksum = byte sum of the payload).

## MIDI + MPE output

The board enumerates as a **composite USB device**: the CDC serial port the
companion uses *and* a USB-MIDI port named "Miso", simultaneously — so colours
can be retuned while playing. The composite descriptor and class driver are
hand-written in `USB_DEVICE/App/usbd_composite.c` (CubeMX only generates
single-class devices); CDC keeps its original endpoints and its app-layer API,
so the companion protocol was unaffected.

Layout is **Wicki-Hayden in 31-EDO**. Each key's pitch is derived with integer
arithmetic only — MIDI wants a note number and a bend, never a frequency:

```
w = x - 2y + 34 ,  h = -y + 15      Wicki-Hayden basis change + anchor
step = 5w + 3h                      31-EDO step (whole tone 5, semitone 3)
note = round(step * 12 / 31)        nearest 12-EDO MIDI note
bend = 8192 + round((step*1200 - note*3100) * 8192 / (4800 * 31))
```

Whole tones run left to right, fifths up-right and fourths up-left. That basis
is meantonal's `WICKI_FROM` composed with a vertical flip of the board; note
that a vertical flip in a skewed axial basis is `(x, y) -> (x + y, -y)`, not
`(x, -y)` — plain negation breaks hex adjacency and turns one diagonal into a
major 6th.

The music theory behind those constants lives in the companion's
`src/lib/tuning.ts`, which uses [meantonal](https://meantonal.org/) — the
firmware only needs the reduced formulas. Both were cross-checked: identical
note numbers for all 31 keys and identical bend values.

Output is **MPE lower zone** — master channel 1, member channels 2–16, so 15
simultaneous notes each with their own pitch bend carrying the microtonal
offset. The zone is announced (MCM + RPN 0 pitch-bend sensitivity, ±48
semitones) once the host configures the device. Note-on sends the bend first
so notes never start out of tune, and channels are allocated least-recently-
used, stealing the oldest voice when all 15 are busy.

The companion's **MIDI tab** monitors this over Web MIDI and checks each
received note against what meantonal says that key should sound.

### After regenerating code with CubeMX

Four edits to generated files must be re-applied (CubeMX will revert them):

1. `USB_DEVICE/Target/usbd_conf.h` — `USBD_MAX_NUM_INTERFACES` to `4U`
2. `USB_DEVICE/Target/usbd_conf.c` — the whole PMA block. CubeMX regenerates
   buffers starting at `0x18`, which only clears a 3-endpoint-pair buffer
   descriptor table; with MIDI on EP3 the table reaches `0x20` and silently
   corrupts the EP0 OUT buffer (symptom: the device enumerates and CDC works,
   but MIDI never comes online). Buffers must start at `0x40`, and the two
   MIDI endpoints `0x83`/`0x03` must be added.
3. `USB_DEVICE/App/usbd_desc.c` — device class `0xEF/0x02/0x01`, PID 22337,
   product string "Miso"
4. `USB_DEVICE/App/usb_device.c` — register `USBD_Composite` and
   `USBD_Composite_RegisterCDCInterface` instead of the CDC equivalents

## Board layout

The 31 keys sit on a sparse 7×7 grid of **axial hex coordinates** — (x, y),
x = column from left, y = row from top — forming the hexagonal board outline.
Axial (not offset) coordinates: the grid is sheared so integer coordinates
map onto hexagon centerpoints; each key has six neighbors. The LED chain
snakes column-by-column (serpentine); each cell's Hall sensor and LED share a
coordinate. LED chain indices on the grid (`·` = no key):

```
·   ·  11  12   ·   ·   ·
·   ·  10  13  24  25   ·
·   2   9  14  23  26  30
1   3   8  15  22  27  29
0   4   7  16  21  28   ·
·   5   6  17  20   ·   ·
·   ·   ·  18  19   ·   ·
```

Sensor indices follow the mux wiring (sensor = M1 channel for 0–15, 16 + M2
channel for 16–30) and are *not* the same ordering. The mapping lives in two
places kept in sync: `led_for_sensor[]` in `Core/Src/main.c` (firmware) and
`companion/src/lib/layout.ts` (companion, including full grid coordinates).

**Velocity events**: each key runs a two-threshold state machine on its
normalized travel (0 = rest, measured at boot; 1 = calibrated full press,
baked per key into `key_cal_max[]` in `main.c`). Crossing 10% of travel arms
the timer, crossing 70% fires `EV <key> DOWN vel=<1..127> dt_us=<transit>` on
the CDC port (velocity is a log map of the transit time; raw `dt_us` is always
included so the curve can be refit offline), and falling below 30% fires
`EV <key> UP`. A held key's LED glows green with brightness proportional to
strike velocity. Tuning constants (`KEY_*_POS`, `VEL_DT_FAST_US`,
`VEL_DT_SLOW_US`) live at the top of `main.c`.

**Companion app** (`companion/`): Svelte 5 + TypeScript + Tailwind + [meantonal](https://meantonal.org/),
built with Vite. It shows live per-key levels with min/max watermarks, per-key stats
(rest / min / max / noise σ), and auto-triggered press-waveform capture with
20→70% transit times — plus the **Key colors** tab: a click-to-paint view of
the physical board (axial hex rendering) with named color mappings saved in
the browser and streamed to the board live, either painted by hand or generated
procedurally. Pitch mapping is next.

The **procedural generator** colours each key by the accidental of the note
that lands on it — the key's Bosanquet row — via meantonal. That library
represents a pitch as a vector of whole steps and diatonic semitones above
C₋₁, with the octave as (5,2) and a sharp as (1,−1): exactly this board's axial
basis, so a grid coordinate is a pitch vector plus an anchor and
`pitch.accidental` *is* the row. A palette of N colours covers N contiguous
accidentals and wraps for rows beyond it (3 colours over the Miso's 5 rows
gives red green blue red green). Layout (Bosanquet, or Wicki-Hayden via
meantonal's own `WICKI_FROM` basis change), the starting accidental, an (x, y)
placement offset and LED brightness are all adjustable, and the whole scheme
streams to the board as you tweak it. Board rendering, note names and the
layout bases live in `companion/src/lib/tuning.ts`.

For live board data, run it locally in Chrome (Web Serial needs a top-level
secure page; the published Claude artifact is wrapped in an iframe that
doesn't delegate serial access, so the artifact copy is a simulated demo
only):

```sh
cd companion
npm install
npm run dev     # → http://localhost:5173, connects to the board
npm run build   # dist/miso-companion.html (single file, publishable as artifact demo)
```
