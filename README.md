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

Or send `B!` on the CDC port, which needs no button press at all. Both routes
are kept deliberately: the serial command is the everyday one, but it is only
reachable while the application still enumerates. The double tap runs from
`bootloader_check()` before any clock or peripheral is configured, so it is the
recovery path when a bad flash, a hang during init or a hard fault means USB
never comes up — precisely when DFU is most needed.

How it works: a magic word in a `.noinit` RAM section (which survives NRST
resets) is armed for the first 500 ms after boot (`BOOTLOADER_TAP_WINDOW` in
`main.c`). If a second reset arrives inside that window, `bootloader_check()`
in `main.c` jumps to system memory at `0x1FFF0000` before any clocks or
peripherals are configured. SWD via the 5-pin header still works as before for
debugging.

**The reset cause is checked first.** `.noinit` RAM only really clears when the
rail collapses to 0 V, and a supply that merely sags keeps it — so a bouncing
power connection (a pogo-pin mate, a hand-plugged USB cable) produces a burst of
brown-out resets that reads as a deliberate double tap. The board then boots
into the ROM bootloader with its LEDs dark and looks completely dead, while
actually enumerating as `DFU in FS Mode`. So the magic word is only honoured
after a pin reset or a software reset (`RCC_CSR_PINRSTF`/`SFTRSTF`) with
`BORRSTF` clear; a power-on or brown-out reset discards it. A POR asserts NRST
internally and therefore sets `PINRSTF` too, which is why `BORRSTF` has to be
tested first.

If a board ever appears dead, check what it enumerated as before assuming
hardware:

    ioreg -p IOUSB -w0 -l | grep -i '"USB Product Name"'

`DFU in FS Mode` means it is in the ROM bootloader and `./flash-usb.sh` will
talk to it right now.

### Required option bytes (every board, once)

**BOOT0 must be taken away from the pin.** The G431KBU6 samples BOOT0 at reset
from **PB8**, and this design leaves PB8 unconnected — so at the factory default
(`nSWBOOT0 = 1`) the boot mode is decided by whatever charge happens to sit on a
floating pin as the rail rises. When it floats high the ROM bootloader runs, the
LEDs stay dark, and the board reads as dead while quietly enumerating as
`DFU in FS Mode`. No firmware can intercept this; the hardware jumps before a
single instruction of the application executes. Symptoms were random failures to
start on USB replug, and worse odds when hot-plugging boards together (a pogo
mate being a messier power-up than a cable).

    STM32_Programmer_CLI -c port=usb1 -ob nSWBOOT0=0 nBOOT0=1

Option bytes are per-chip, so **each board needs this once**. On the next PCB
revision a 10 kΩ pull-down from PB8 to GND does the same job in copper, and
`nSWBOOT0 = 0` additionally frees PB8 for use as an ordinary GPIO.

**Watch the polarity.** `nBOOT0` is active-low despite CubeProgrammer describing
it as "this option bit sets the BOOT0 value", which reads as direct and is not.
Verified empirically on hardware, both directions:

| `nBOOT0` (with `nSWBOOT0 = 0`) | Boots to |
|---|---|
| `0` | system memory — permanent DFU |
| `1` | main flash — what you want |

Getting it backwards is recoverable, since DFU stays reachable and the option
bytes can just be rewritten, but it looks alarming.

Raising the brown-out threshold from its default of level 0 (~1.7 V)
(`STM32_Programmer_CLI -c port=usb1 -ob BOR_LEV=4`) hardens the power-up further
by holding the MCU in reset until VDD is properly established.

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
| `k`     | Dump the state of the four inter-board links |
| `T`     | Dump the mesh topology: offsets, port roles, known boards, health counters |
| `L` + `int16 x`, `int16 y`, 93 bytes | Set LED colors on the board at that grid origin, anywhere in the mesh |
| `B!`    | Reboot into the USB DFU bootloader (two bytes, so a stray character can't misfire) |
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

## Board linking (inter-board UART)

Boards tile through the four pogo-pin connectors, each carrying +5 V and a UART
pair. Sides map to peripherals as:

| Side | Peripheral | TX | RX |
|------|-----------|----|----|
| top | *(bit-banged, pins not assigned yet)* | — | — |
| bottom | LPUART1 | PA2 | PA3 |
| left | USART2 | PB3 | PA15 |
| right | USART1 | PA9 | PA10 |

All three run at 460800 baud. Only three full-duplex pairs are bonded out on the
UFQFPN32 package, so the fourth side will be bit-banged; its port exists in
`Core/Src/link.c` with a null UART and is skipped until then.

### Why TX pins idle in high-Z

CubeMX configures every UART TX pin as `GPIO_MODE_AF_PP`, so from ~10 ms after
boot the pin is driven push-pull to 3.3 V as UART idle — forever, whether or not
anything is plugged in. Hot-plug a second board onto a running one and that
3.3 V lands on the new board's RX pin while its own rail is still at 0 V.
Current flows through the RX pad's ESD clamp diode into the dead board's 3V3
net and parks it near 2.6 V; the STM32's POR needs VDD to rise *from* below
~1.6 V, so it never releases and the board stays dark. Connecting both boards
unpowered and *then* applying USB works, because the rails rise together.

So `HAL_UART_MspInit()` immediately re-configures each TX pin to
`GPIO_MODE_INPUT` (before `HAL_UART_Init()` even sets `TE`, so it is never
driven at all), and `link.c` only drives it once a live board has been heard.

Each RX pin gets the **internal pull-up**, which is what makes the dangerous
case detectable:

| RX reads | Meaning |
|----------|---------|
| high | nothing connected, or a powered neighbour idling |
| **low** | an unpowered board is clamping the line to ~0.7 V through its ESD diode — do not drive |

The pull-up sources ~65 µA into a dead board (harmless) and holds the line at
UART idle, so a burst from the far end never looks like a spurious start bit.

### Port state machine

Ticked from the scan loop at ms resolution, identical on every board and every
port — there is no master/slave asymmetry to deadlock.

| State | `i` char | TX pin | Leaves when |
|-------|:--------:|--------|-------------|
| `BLOCKED` | `b` | high-Z | RX high for 50 ms |
| `DOWN` | `.` | high-Z | RX goes low → `BLOCKED`; HELLO received → `HANDSHAKE`; probe timer → `PROBE` |
| `PROBE` | `p` | driven, one frame | frame drained and `TC` set → back to `DOWN`, next probe in 200 ms + UID jitter |
| `HANDSHAKE` | `h` | driven | peer ACK (or PING) → `UP`; 100 ms timeout → teardown |
| `UP` | `U` | driven | 250 ms of silence → teardown |

A purely passive "wait until RX proves someone is there" rule deadlocks when
both ends run it, so an unconfirmed port emits one HELLO every 200 ms and
returns to high-Z. At 460800 that is ~370 µs of drive in 200 ms (~0.2%), far too
little to hold a neighbour's rail up: even 1 mA of load collapses ~10 µF within
~26 ms, well inside the gap. Receiving a valid *frame* is stronger evidence than
any level check — it can only come from a powered, running board — so it
overrides `BLOCKED`.

**Teardown matters as much as the gating.** Leave TX driven after a neighbour is
unplugged and the next hot-plug fails exactly the way this whole mechanism
exists to prevent.

### Frame format

Same family as the CDC scan frame, plus a length byte because the link is
hot-pluggable and has to resync mid-stream:

    A5 5A | type | len | payload[len] | checksum     (checksum = byte sum of type, len, payload)

Types start at `0x10` to stay clear of the CDC frame types: `HELLO` (0x10),
`HELLO_ACK` (0x11), `PING` (0x12, the 50 ms keepalive). HELLO carries the 96-bit
device UID, a protocol version, the sender's port index and a `has_usb` flag.
That flag marks the board plugged into a host — the one feeding +5 V to the rest
of the chain, and the eventual MIDI/CDC master — but nothing acts on it yet:
topology discovery, addressing and key-event forwarding come next.

`k` over CDC dumps every port's state, peer UID, and RX/TX/error counts; `i`
adds a compact `links=TBLR` summary using the characters above.

### Hardware note

Firmware gating fixes the symptom. The robust fix is a ~1–4.7 kΩ series resistor
in each signal line at the connector, which limits ESD-diode injection to well
under 1 mA and kills the hazard regardless of what firmware is running — worth
adding on the next board revision, not least because it also protects against a
board flashed with an older build.

## Multi-board grid (topology, events, colours)

A tiled grid behaves as **one instrument**: the USB-connected board ("master")
sounds every key in the grid, and colour schemes can be painted onto any board
in it. Implemented in `Core/Src/mesh.c` on top of the link layer above.

### The tiling is exact

Neighbour offsets are `right = (5, 2)` and `down = (-3, 5)`. That lattice has
determinant `5·5 − 2·(−3) = 31`, exactly the key count, and the board's 31 cells
form a **complete residue system** modulo it — 31 distinct residues under
`f(x,y) = 2x + 26y mod 31`, with no two cells differing by a lattice vector. So
the shape tiles the plane with no gaps or overlaps in any arrangement.

Two consequences the design relies on: an absolute coordinate **uniquely
identifies one key in the whole grid**, so it serves directly as a key id; and
coordinate → (board, sensor) is unambiguous.

| Port | Vector to that neighbour |
|------|--------------------------|
| right | `(+5, +2)` |
| left | `(−5, −2)` |
| bottom | `(−3, +5)` |
| top | `(+3, −5)` |

### What tiling does to pitch

Geometry and tuning are separate questions, and the answer here is not obvious.
Pitch is a linear function of absolute coordinate, so a tiled grid is seamless —
but under the firmware's Wicki-Hayden basis the two tiling vectors are wildly
asymmetric:

| Direction | Wicki-Hayden (firmware) | Bosanquet |
|---|---|---|
| right `(5,2)` | **−1 step = −38.7 cents** | +1 octave exactly |
| down `(−3,5)` | −80 steps = −2.58 octaves | unison |

So a **horizontal** row adds keys but almost no range (1 board 2.52 octaves,
3 boards 2.58, and at three wide 12 of the 93 pitches duplicate), while a
vertical stack adds ~2.6 octaves per board. In Wicki-Hayden the horizontal axis
is whole tones and the vertical is fifths, and `(5,2)` nearly cancels. Switching
the firmware to the Bosanquet basis would invert this. Worth deciding before
building a wide row.

### Topology: a spanning tree

A grid contains cycles (any 2×2 block), so routing uses a tree, not a flood.
Every board with a known place in the grid beacons `ANNOUNCE` on all UP ports
every 250 ms, carrying root UID, own UID, parent UID, offset and depth.

Receiving one on port `P` implies an offset of `their_offset − direction(P)`. A
board adopts `P` as parent when it has none, when the route is shorter, or on a
depth tie by lower UID. A neighbour naming *us* as its parent makes that port a
**child**. Traffic goes up to the parent only (so each event arrives exactly
once) and down to children only (acyclic, so no loops).

Two consistency checks, both nearly free. The peer's port must be the opposite
edge — `right` must meet `left` — which catches a miswired or rotated board.
And a non-tree edge implies an offset too, which must agree with the one already
held: **cycles in the grid validate the topology rather than confusing it**.
Disagreement is counted in `geom_err`, never silently accepted.

### Events, and why notes cannot stick

Key events are sent once, fire-and-forget, with the origin board applying its own
offset so coordinates are absolute and intermediate boards forward opaquely.

The safety net is `KEYSTATE`, sent every 100 ms, carrying a 31-bit held-key mask.
It does three jobs: it is how the master learns which boards exist at all
(`ANNOUNCE` only flows root→leaves); no `KEYSTATE` for 500 ms means the board is
gone and its notes are released; and any key the master believes held but the
board reports up gets a note-off. A key held there but never seen here is
**counted, never fabricated** — the real velocity is gone, and a wrong-sounding
note is worse than a logged miss.

A link teardown releases a direct neighbour's notes immediately; the 500 ms
timeout covers boards further away.

### Commands

| Command | Effect |
|---------|--------|
| `T` | Dump topology: own offset/depth/role, port roles, known boards, counters |
| `L` + `int16 x`, `int16 y`, 93 bytes | Set colours on the board at that grid origin, wherever it is in the mesh |

`C` keeps its exact meaning ("this board"), so the companion needs no change.
`EV` lines gained trailing `x=` and `y=` fields; the existing prefix is
unchanged, so existing parsers still match.

Counters in `T` are the health readout — `missed_down`, `fixed_up`,
`lost_release`, `geom_err`, `multi_master`. On a healthy link all but
`lost_release` should stay at zero.

### Limitation: no vertical links yet

The TOP port has no UART (only three full-duplex pairs are bonded out on
UFQFPN32), so a vertical pair cannot link at all — one board's BOTTOM would meet
another's TOP. `k` and `i` show that port as `-` rather than a state. The mesh
layer is written general over all four ports and needs no change when the
bit-banged port lands; until then, only horizontal chains communicate.

### Verified on hardware

Two boards, master plus one hot-plugged to its right: link came up with
`err=0` over 8,613 frames; the remote board was discovered at `off=5,2` with
`geom=ok`; remote key events arrived with coordinates offset by exactly `(5,2)`
(sensor 0 `(2,3)`→`(7,5)`, sensor 3 `(3,3)`→`(8,5)`, sensor 24 `(4,3)`→`(9,5)`);
`L` addressed to each origin lit the intended board only; and a key held while
its board was pulled off produced a synthesised note-off, with the board removed
from the table and the port returned to high-Z.

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
