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
| `r`     | Redo the boot-time rest calibration (hands off the keys for ~0.1 s) |

Scan frame format: `A5 5A 01` · `u32 t_µs` · `31×u16 raw` · `u8 checksum`
(little-endian; checksum = byte sum of the payload).

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

**Companion app** (`companion/`): Svelte 5 + TypeScript + Tailwind, built with
Vite. It shows live per-key levels with min/max watermarks, per-key stats
(rest / min / max / noise σ), and auto-triggered press-waveform capture with
20→70% transit times — the groundwork for velocity calibration. Today it is
the calibration instrument; it is structured to grow into the end-user
configurator (key colors, pitch mappings).

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
