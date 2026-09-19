This repository is the firmware for Miso, a modular, isomorphic musical keyboard with the following features:
- An STM32G431KBU6 microcontroller as its brain.
- Two CD74HC4067 analog multiplex chips connected to the 31 Texas Instruments DRV5055 Hall effect sensors that power the velocity-sensitive key detection.
- A chain of 31 SK6812mini-e RGB LEDs that backlight the keys and allow users to see the key mapping they are currently using visually.
- Four pogo pin connectors, 2 male and 2 female, to the top, bottom, left and right sides of the device which transmit +5V power and allow for UART communication with adjacent boards when connected together. The STM32G431KBU6 only has 3 built-in UART capable pin pairs, so the fourth (top) is a timer-and-DMA software UART.

## Bringing up a new board

A new board needs **two independent things**, and flashing only does one of
them. The firmware lives in main flash; the boot configuration lives in the
option bytes, a separate area of the chip that `./flash-usb.sh` never touches.
So a board running the newest firmware can still be temperamental on USB and
pogo connection, because the fix for that is an option byte. Every chip needs it
once, and a board that has only ever been flashed does not have it.

The whole sequence is one command:

    ./provision-board.sh

It builds, flashes, then sets and verifies the option bytes, and is safe to
re-run — the option bytes are only written when they are actually wrong. What it
does, and what to do if you are working by hand instead:

1. **Get it into DFU.** A board with firmware on it: send `B!` on the CDC port
   (the scripts do this for you) or double-tap reset. A board with empty flash
   has neither route, so use the 5-pin SWD header, or check whether it already
   came up in DFU on its own — with the factory option bytes and a floating PB8
   it often does.

2. **Flash the firmware.**

        ./flash-usb.sh

   Builds and flashes in one step. See
   [Flashing firmware over USB-C](#flashing-firmware-over-usb-c-no-swd-header-needed).

3. **Set the option bytes.** The step flashing does *not* do. Put the board back
   into DFU first, then:

        STM32_Programmer_CLI -c port=usb1 -ob nSWBOOT0=0 nBOOT0=1

   See [Required option bytes](#required-option-bytes-every-board-once) for the
   polarity trap and why this is needed at all. Note the write prints alarming
   errors that are expected — that section explains them.

   **Do the firmware first, as above.** Writing the option bytes triggers an
   option-byte-load reset, and with `nSWBOOT0 = 0` in effect that reset boots
   main flash. On a board with empty flash nothing then runs: no CDC port for
   `B!`, and no double-tap either, since `bootloader_check()` is itself firmware.
   USB DFU is gone and only the SWD header can recover it. Flashing first means
   that same reset lands in a working application.

4. **Verify both.** Power-cycle the board — you will need to anyway, see
   [Flashing firmware over USB-C](#flashing-firmware-over-usb-c-no-swd-header-needed),
   as it does not reliably start the application by itself after a DFU session.
   Then power-cycle a few more times, including a pogo hot-plug, since that was
   always the worse case. It should enumerate as `Miso` every time:

        ioreg -p IOUSB -w0 -l | grep -i '"USB Product Name"'

   Read the option bytes back too (step 3's section shows how). Note that doing
   so leaves the board in DFU again.

If you skip step 3, the symptom is not an obvious failure: the board works most
of the time and randomly comes up dark and apparently dead on replug.

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

`-g` leaves DFU by **jumping** into the application rather than resetting, so
the application starts on top of the bootloader's configuration: its PLL, which
makes `SystemClock_Config()` fail into `Error_Handler()`, and its USB interrupt,
which can fire before the driver behind the handler exists. `main()` therefore
silences the NVIC and calls `HAL_RCC_DeInit()` before configuring anything
(`main.c`, just before `SystemClock_Config()`), which is a no-op on a normal
reset boot.

**In practice you still have to power-cycle the board after flashing.** That
mitigation is not sufficient on its own: CubeProgrammer reports
`Start operation achieved successfully`, and the board then sits there with USB
down and the LEDs still dim green from the `B!` handoff — green being proof the
application never ran, since its startup wave would have overwritten it. Unplug
and replug and it comes straight up. Treat "no USB after flashing" as the normal
outcome rather than a failed flash; it is not a bad flash and not an option-byte
problem.

This is a known rough edge, not a solved one. Diagnosing it properly means
finding what the application inherits from the bootloader that surviving
`HAL_RCC_DeInit()` does not cover — the USB peripheral and its clock being the
obvious suspects, since USB is the thing that never comes back.

Or send `B!` on the CDC port, which needs no button press at all. That route
paints every LED dim green before resetting: the ROM bootloader never touches
the LED chain and the reset does not cut its power, so the board glows green for
the whole DFU session instead of looking dead. Both routes
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

**Check what a chip currently has** (with it in DFU):

    STM32_Programmer_CLI -c port=usb1 -ob displ

An unfixed board shows `nSWBOOT0 : 0x1 (BOOT0 taken from PB8/BOOT0 pin)`; a
fixed one shows `0x0 (BOOT0 taken from the option bit nBOOT0)`. `nBOOT0` is
usually already `0x1` from the factory, so `nSWBOOT0` is the byte that actually
changes — which is why the symptom is easy to misread as a firmware problem.

**The write reports errors even when it succeeds.** Expect something like:

    Unable to reconnect the target device: time out expired
    Error: Downloading Option Bytes Data failed
    Error: Uploading Option Bytes bank: 0 failed

Writing option bytes triggers an option-byte-load reset. With `nSWBOOT0 = 0` now
in effect the board reboots straight into main flash and leaves the DFU bus
mid-operation, so CubeProgrammer's reconnect finds nothing and reports failure
for a write that already landed. Ignore the errors and confirm with a readback:
re-enter DFU and run `-ob displ` again. Do not re-run the write on the strength
of the error message alone.

Note that after an `-ob displ` the board stays in the ROM bootloader until
something resets it, so seeing `DFU in FS Mode` right after a read is expected
and is not evidence that the fix failed.

Raising the brown-out threshold from its default of level 0 (~1.7 V)
(`STM32_Programmer_CLI -c port=usb1 -ob BOR_LEV=4`) hardens the power-up further
by holding the MCU in reset until VDD is properly established.

## Sensor streaming & the companion app

The firmware scans all 31 Hall sensors in a tight loop — **~2276 Hz measured**,
439 µs per scan, reported by the `i` command — and exposes a USB CDC serial
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
| `P` + 16 bytes | Set the pitch map: `int32` fifth in millicents, `int16` m00 m01 m10 m11 (grid → `(w, h)` basis change), `int16` anchor w, h. Little-endian, validated before it is installed |

Scan frame format: `A5 5A 01` · `u32 t_µs` · `31×u16 raw` · `u8 checksum`
(little-endian; checksum = byte sum of the payload).

### The LED data line: flicker while the companion streams

Symptom: while the companion is streaming, part of the chain on the USB board
flickers, with the corrupt region appearing at a random point and moving
around; it stops the instant streaming stops. It surfaced when `-Os` doubled
the scan rate and with it the stream frame rate (decimation is per *scan*).

**The cause was a type.** `dma_buffer` was declared `uint32_t` while the DMA
channel is configured for **half-word** memory reads (`MemDataAlignment =
DMA_MDATAALIGN_HALFWORD`, from the `.ioc`). So the DMA walked the array two
bytes at a time and every second beat it fetched was the zero upper half of a
word. `Length` is in beats, so 1494 beats consumed only the first 747 of the
1494 entries. What actually went down the wire:

| | intended | what the old code sent |
|---|---|---|
| per data bit | 1 beat, 1.25 µs | 2 beats, **2.5 µs** (pulse, then a forced low) |
| trailing reset | 750 beats, 937 µs | **3 words = 7.5 µs** of driven low |
| `led_critical()` covered | all 744 data bits | the **first half** only |
| total refresh | 1867.5 µs | 1867.5 µs |

The total duration is identical either way, which is why the "~1.9 ms per
refresh" in the comments matched measurement and the bug stayed invisible.

The damage is the **latch**. An SK6812 commits a frame when the line is held
low for ≥80 µs, and only 7.5 µs of that was ever *driven*. The rest came from
what happened next: `HAL_TIM_PWM_Stop_DMA()` clears the channel enable and the
main output, and with OSSI off TIM1 then releases PA8 altogether -- high
impedance, and the board has no pull on the line. So every frame was latched by
a **floating** node that happened to sit low for long enough, ~28 ms of every
30 ms, on a pin two away from the USB pair (PA11/PA12). That is a latch with no
noise margin at all, and it is the USB correlation: more bus activity, more
chances for the undriven latch window to be spoiled.

Both halves are fixed. The buffer is `uint16_t`, so bits are 1.25 µs and the
reset is a full 937 µs. And the timer is never stopped -- it runs on at
CCR1 = 0, which holds the output actively driven low, with only the DMA request
switched off (`led_line_hold_low()` in `main.c`). PA8 also carries a pull-down
for the moments the timer does not own it.

Also kept from the first attempt: **PA8 is `GPIO_SPEED_FREQ_VERY_HIGH`**, not
CubeMX's default `LOW` (`stm32g4xx_hal_msp.c`, `TIM1_MspPostInit`). That took
the worst case from constant flicker to occasional on its own.

The level margin is still genuinely thin -- the SK6812's input-high threshold
is 0.7 x VDD = **3.5 V** and PA8 drives 3.3 V -- so the next board revision
should still get a level shifter (74AHCT125 or similar) on the data line, or a
series Schottky in the LED chain's 5 V feed to drop it to ~4.4 V (sized for the
whole chain's current, not a signal diode). But that is now a margin problem on
a properly driven signal, not a marginal signal on top of an undriven latch.

#### What was ruled out, and how

A theory worth recording because it is wrong: that the floating line was
*picking up USB edges directly*, each one read by the LED as a data bit.

`f` on the CDC port toggles a diagnostic that restores the old behaviour
exactly -- PA8 released between refreshes, no pull -- but with the pin switched
to an input that counts every edge on it. `i` then reports `float=` and
`idleedges=`. `F` is the control that makes those numbers mean anything: it
drives 50 known pulses through the same routing, mask, NVIC entry and callback,
and prints `SELFTEST pulses=50 edges=100 expect=100 OK`. Without it, a zero
reading is indistinguishable from a diagnostic that never ran.

Measured with the counter validated before and after, 15 s per phase:

| condition | idle edges |
|---|---|
| line floating, bus quiet | 0 |
| line floating, board streaming ~1100 frames/s | 0 |

So the floating line picks up **nothing** a logic input can see. The
disturbance that spoils an 80 µs latch on a high-impedance node is far smaller
than a full swing across the STM32's Schmitt trigger, which is why the latch
was fragile while the edge count stayed at zero. Keep `F` in mind generally:
`ledchurn` and `dmachurn` stayed at zero through two minutes of flickering and
were read as "the bytes are right, so the fault is on the wire" -- correct as
far as it went, but it pointed at the voltage level when the defect was in the
waveform. `p<N>` throttles the scan loop and `n` stops the Hall scan, which
separate "how fast we run" from "what we run" without a reflash.

### Build optimisation level matters more than anything else

The project builds `Debug` at **`-Os`**, not `-O0`. This is not a detail: at
`-O0` the scan loop runs at ~1129 Hz, and `keys_process()` is float-heavy enough
that the optimiser roughly doubles it. Measured on one board with nothing
attached, six samples each, all within two counts:

| Build | scan_hz | µs/scan | flash |
|---|---|---|---|
| `c813fa9` @ `-O0` (before the link layer) | 1129 | 886 | 59,808 |
| `f34193d` @ `-O0` (link + mesh) | 1079 | 927 | 70,760 |
| `f34193d` @ `-Os` | **2276** | 439 | 37,608 |

So the inter-board work costs ~4.4%, and the optimisation level is worth ~2x —
the earlier "target ≥1.5 kHz" was never met at `-O0` and is comfortably beaten at
`-Os`. Flash drops by 47% as a bonus.

Set in `.cproject` on the Debug configuration (the Release configuration was
always `-Os`). `-g3` is kept, so SWD debugging still works, with the usual
caveats about stepping through optimised code.

**One consequence to remember when tuning the velocity engine:** `KEY_EMA_ALPHA`
is a per-*sample* coefficient, so doubling the scan rate halves the smoothing
filter's time constant — roughly 2.6 ms of averaging becomes 1.3 ms. Keys feel
more responsive and are more exposed to sensor noise. `VEL_DT_FAST_US` and
`VEL_DT_SLOW_US` are in real microseconds and so are unaffected, and transit
timing gets *more* accurate with finer sampling — but the velocity curve was
fitted at ~1.1 kHz and is worth re-checking with the companion's press capture.

## MIDI + MPE output

The board enumerates as a **composite USB device**: the CDC serial port the
companion uses *and* a USB-MIDI port named "Miso", simultaneously — so colours
can be retuned while playing. The composite descriptor and class driver are
hand-written in `USB_DEVICE/App/usbd_composite.c` (CubeMX only generates
single-class devices); CDC keeps its original endpoints and its app-layer API,
so the companion protocol was unaffected.

The pitch map is **nine numbers pushed over `P`** and held in RAM. Two things
are kept apart, because they are independent:

A **layout** is a 2×2 integer basis change from the board's grid axes into
meantonal's `(w, h)` — whole tones and diatonic semitones. The board's axial
grid *already is* that basis (`+x` a whole tone, `+y` a diatonic semitone), so
**Bosanquet is the identity** and only the anchor is left to choose.
**Wicki-Hayden** is meantonal's `WICKI_FROM` composed with a vertical flip of
the board, `w = x - 2y, h = -y`, i.e. the matrix `(1, -2, 0, -1)` — note that a
vertical flip in a skewed axial basis is `(x, y) -> (x + y, -y)`, not `(x, -y)`;
plain negation breaks hex adjacency and turns one diagonal into a major 6th.
Because a layout lives in pitch space, it means the same thing in every tuning.

A **tuning** is *one* number: the width of its fifth. Every tuning in the
meantone family is determined by it, so cents are linear in it —

```
(w, h) = M·(x, y) + anchor
cents  = fifth·(2w - 5h) + 1200·(3h - w)      meantonal's GENERATORS_TO, written out
note   = round(cents / 100)                   nearest 12-EDO MIDI note
bend   = 8192 + round((cents - note*100) * 8192 / 4800)
```

— and an EDO is just one way to pick the fifth, `round(log2(1.5)·edo)·1200/edo`.
That is why there is no per-EDO step lattice and no per-key table here: 16 bytes
of parameters retune the whole instrument, evaluated at absolute coordinates so
one frame covers every board in the mesh. The firmware carries `cents` in
**millicents** to stay in integers; MIDI wants a note number and a bend, never a
frequency, so no floating point is needed anywhere.

The default is **Bosanquet in 31-EDO with D4 on the centre key** — fifth 696774
millicents, identity matrix, anchor (23, 7) — which is **byte-identical to the
hardcoded formula it replaced** for every key, so a board that never hears from
the companion sounds exactly as it always did.

`P` needs no mesh propagation, and deliberately gets none: MIDI is generated
only by the USB-connected board, which already plays every key in the grid from
absolute coordinates (`Miso_EmitKeyDown`), so the master is the only board that
needs a tuning at all. Broadcasting would only create a way for a child's copy
to drift from the master's.

Retuning under a held key is safe without special handling: `mpe_voice_t` caches
each voice's note number precisely so a note-off sends the note that was
actually started.

The music theory lives in the companion's `src/lib/tuning.ts`, which asks
[meantonal](https://meantonal.org/) rather than reimplementing the firmware's
arithmetic — so when the MIDI monitor finds them agreeing, that means the push
arrived and was applied. The two are cross-checked on the host by
`companion/scripts/check-firmware-pitch.py`, which extracts `pitch_for_xy`
verbatim from `main.c`, compiles it, and diffs it against the companion over
nine tunings, both layouts and a four-board grid.

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
| top | software UART: TIM16 + TIM3 + DMA (`softuart.c`) | PB4 | PB5 |
| bottom | LPUART1 | PA2 | PA3 |
| left | USART2 | PB3 | PA15 |
| right | USART1 | PA9 | PA10 |

All four run at 460800 baud. Only three full-duplex pairs are bonded out on the
UFQFPN32 package, so the top side is a software UART -- see
[The top port is a software UART](#the-top-port-is-a-software-uart). Above the
driver seam in `link.c` (rings, frame parser, state machine, hot-plug gating)
the four ports are identical.

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

### The top port is a software UART

TX is PB4 and RX is PB5, neither of which reaches a USART on this package. Both
directions are driven by timers and DMA so that no interrupt fires per bit: at
460800 baud that would be one every 2.2 µs, and the Hall scan rate would pay for
it. Sending costs one interrupt per 32 bytes; receiving costs none until
something decodes.

**TX: TIM16 in PWM mode, one period per bit.** PB4 is TIM16_CH1. `CCR1 = 0`
holds the pin low for the whole period and `CCR1 > ARR` holds it high, so a
byte is ten CCR1 values (start, eight data bits LSB first, stop). A DMA transfer
on each update event writes the next value into the preloaded CCR1; the CPU
encodes up to 32 bytes per burst and hears about it once, when the burst
completes. Each burst ends with two idle words because a value transferred at
update *k* is on the pin during period *k+1*: without them "complete" would
arrive while the stop bit was still on the wire and `link.c` would put the pin
back to high-Z half a bit early.

**RX: TIM3 free-running, capturing every edge.** PB5 is TIM3's TI2. Channel 2
captures both edges and a circular DMA streams the timestamps into a 256-entry
ring; nothing else happens until something looks at it. The decoder
(`softuart_decode.h`) runs from the link tick and, as a backstop, from the DMA
half/complete interrupts and a 2.2 ms compare on TIM3 -- so it always runs
well inside the counter's 17.7 ms wrap, and cannot lose the ring at full line
rate if the main loop stalls.

The ring holds edge *times* but not levels; two parity facts recover them. The
level at any instant after a start edge is the parity of the edges since it,
which is how bits are sampled. And whether a candidate edge was *falling* is
the current line level flipped once per edge captured after it, which is what
makes hunting after a framing error safe: without it a rising edge followed by
idle decodes as a plausible byte and eats the real frame behind it. The current
level comes from the timer rather than the pin -- channel 1 captures rising
edges only, so the last edge was rising exactly when CCR1 and CCR2 agree.
Reading the pin instead races the DMA, which lags each capture by a few bus
cycles.

The decoder is pure arithmetic with no hardware access, so it has a host test:

    cc -I Core/Inc -o /tmp/test_decode tools/test_decode.c -lm && /tmp/test_decode

covering back-to-back bytes, ±2% baud mismatch, ring and counter wrap, glitches
before and between frames, and a lost edge (which corrupts what was captured
before it and re-syncs after).

Costs: 644 B for the TX burst buffer, 512 B for the edge ring, DMA1 channels 2
and 3, TIM16 and TIM3. None of it is in the `.ioc`, deliberately, so a CubeMX
regeneration cannot revert it -- but the `.ioc` therefore still shows those
resources and PB4/PB5 as free. Do not assign them to anything else there.

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

Geometry and tuning are separate questions. A board displacement is a vector in
the two-dimensional pitch space `(w, h)` — whole tones and diatonic semitones —
which specifies a **spelled** interval exactly. Reducing it to 12-TET semitones
or MIDI note numbers is lossy: that map has a non-trivial null space, and
different tunings temper out different intervals, so the projection hides
something different in each case.

| Layout | Board move | `(dw, dh)` | 31-EDO | 12-TET | Example |
|---|---|---|---|---|---|
| Wicki-Hayden | right `(5,2)` | `(1,−2)` | −1 step | **0** | C4 → B♯3, an enharmonic diesis |
| Wicki-Hayden | down `(−3,5)` | `(−13,−5)` | −80 steps | −31 | C6 → F3 |
| Bosanquet | right `(5,2)` | `(5,2)` | +31 steps | +12 | C4 → C5, an octave |
| Bosanquet | down `(−3,5)` | `(−3,5)` | **0** | −1 | C4 → E♭♭♭♭♭4 |

The two bold entries pull in opposite directions, which is the whole point: the
diesis is a real 31-EDO interval that *vanishes* in 12-TET, while Bosanquet's
vertical step is a real 12-TET semitone that *31-EDO* tempers to nothing. Either
one described by its MIDI note number would be a description of the projection
artefact rather than of the interval.

Practically, under Bosanquet — the default — a horizontal row gives exact
octaves, so range extends sideways, while a vertical column gives boards that
sound alike but are spelled five flats apart. Under Wicki-Hayden the two
directions swap roles: a horizontal row transposes each successive board down by
one enharmonic diesis, deepening the enharmonic resources rather than extending
range (three boards wide gives 93 keys sounding 81 distinct 31-EDO pitches), and
the range lies vertically instead at two octaves and a fifth per board.

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

### Vertical links

The TOP port runs on the software UART (see
[The top port is a software UART](#the-top-port-is-a-software-uart)), so a
vertical pair links exactly as a horizontal one does: one board's BOTTOM
(LPUART1) meets another's TOP. `k` and `i` report it with the same states as
the other three ports, and the mesh layer never distinguished it.

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

### Startup ripple

The board does not switch into its colour mapping, it wakes up into it. From a
dark chain a bright whitened wavefront sweeps left to right over ~900 ms,
leaving the mapped colour behind it: each key ramps up from black to a white
crest, then decays into its own colour with a squared falloff. `boot_wave_play()`
in `main.c`; the crest level, sweep duration and the widths of the leading ramp
and trailing decay are the `WAVE_*` constants above it.

**"Left to right" is `4x + 3y`, not the column index.** The grid is a sheared
axial hex lattice, and the board is physically laid out in the standard
Bosanquet orientation — the octave direction `(5, 2)` horizontal. Rotating the
axial coordinates by that amount gives, exactly,

    screen_x = (4.5 / sqrt(117)) * (4x + 3y)
    screen_y = (1.5 * sqrt(3) / sqrt(117)) * (5y - 2x)

so the sweep needs one integer per LED and no trigonometry. Cross-checked
against `LED_PIXEL` in `companion/src/lib/layout.ts`, which derives the rotation
independently: the ratio is constant to nine decimal places across all 31 keys.
The keys span `4x + 3y` = 8 (at `(2,0)`) to 34 (at `(4,6)`) — 26 units over 26
distinct values, so nearly every key arrives at its own moment.

The crest whitens towards a level that is never below the colour's own
brightest channel, so it stays a *lightened* version of what it leaves behind
even for a bright mapping pushed by the host, rather than dipping below it.

This replaced the old red/green/blue/white flash test at boot, and covers the
same ground: the crest is white, so all three channels of every LED are driven
as it passes, and a dead LED now reads as a gap in a moving wave rather than in
a static field.

The animation is **blocking, deliberately**. It runs before the scan loop, so
the per-key rest calibration that opens that loop (`REST_CAL_SCANS`, ~56 ms)
still happens under a settled, static pattern. Calibrating while a bright crest
swept the board would fold the chain's own current draw — which rides the same
3V3 rail as the Hall sensors — into every key's rest level.

Two things it does not do. Boards in a tiled set each ripple **independently**,
because links and topology are not up yet when it plays, so a wide grid shows
several parallel sweeps rather than one crossing the whole instrument; a
mesh-wide version would have to wait for discovery and then start on a shared
deadline. And it plays at boot only — pushing a new colour map over `C`/`L`
still swaps in place. `boot_wave_play()` reads `led_background[]` live, so
re-triggering it on a colour push is one call if that turns out to be wanted.

**Companion app** (`companion/`): Svelte 5 + TypeScript + Tailwind + [meantonal](https://meantonal.org/),
built with Vite. It shows live per-key levels with min/max watermarks, per-key stats
(rest / min / max / noise σ), and auto-triggered press-waveform capture with
20→70% transit times — plus the two tabs that set the instrument up.

A **preset** is how the instrument is set up: an LED mapping *and* a pitch
mapping, with room for further preset-scoped settings. One selector, shared by
both tabs, because switching preset changes what the instrument looks like and
what it plays together. The **Key colors** tab is a click-to-paint view of the
board (axial hex rendering); the **Pitch mapping** tab shows what every key
sounds — note name, MIDI number, bend, frequency — and pushes the tuning to the
board over `P`. Both halves are keyed by absolute coordinate, generated
procedurally or set per key, and evaluated lazily so a board attached later
simply resolves.

The two halves differ in one structural respect. A colour layer's generator is
optional: hand-painting freezes it, because an unpainted coordinate can safely be
black. A pitch map's generator is **mandatory and permanent** — a coordinate with
no pitch is a silent key, and `P` carries parameters rather than a table, so
freezing one would leave nothing to send. Pitch overrides are additive
exceptions layered on top, and retuning never discards them.

Storage moved to `miso-presets-v1`; a `miso-color-maps-v2` payload reads as a
preset list whose pitch maps are the 31-EDO Bosanquet default — which is what the
firmware already played, so migrating changes nothing about how a board sounds.
The superseded keys are left in place as backups.

Per-key pitch entry is modelled and persisted but has no editor yet: `P` pushes
parameters, so an overridden key cannot be expressed in it, and carrying them
needs either a sparse second command or a full per-board table. The procedural
path came first deliberately.

**It is grid-aware.** The app polls `T` once a second and models the discovered
mesh in `companion/src/lib/mesh.svelte.ts`, so a tiled set of boards renders as
the single continuous instrument it is — one SVG, every key placed by its
absolute coordinate, with each board captioned by UID when there is more than
one. Key events carry their coordinate, the MIDI monitor checks incoming notes
against the pitch at the *absolute* coordinate (so a note from a neighbouring
board is verified like any other), and the header shows a live board count.

Both mappings are keyed by **absolute grid coordinate**, not LED index, so one
scheme covers however many boards are attached and survives them being added,
removed or rearranged — which also matches how the procedural generator always
worked, colouring by the accidental of the pitch at a coordinate. Pushing sends
one `L` frame per board. Generated schemes are evaluated lazily rather than
materialised, so attaching another board needs no regeneration.

Colour storage passed through `miso-color-maps-v2` on the way here, which read a
v1 31-entry LED-indexed array as a board at the origin; both older keys are still
read as a fallback and still left in place as backups.

Two limits worth knowing. The **Calibrate tab is master-only** — the firmware
forwards key events, not raw scans, and remote sensor data at full rate would be
93 kB/s against a 46 kB/s link, so live levels, stats and press capture show only
the USB board's own keys. And connecting to older firmware still works: with no
topology seen, the app falls back to a single board at the origin and the
original `C` colour frame, and `P` is withheld below 0.7.0 — that gate is
load-bearing rather than polite, because a board that does not know `P` would
read its 16 payload bytes as commands, and `C` (0x43) or `L` (0x4C) are entirely
reachable values in a millicent count or a signed matrix entry.

The **procedural colour generator** colours each key by the accidental of the
note that lands on it — the key's Bosanquet row — via meantonal. That library
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
npm run check:all   # types, then the pitch cross-checks below
```

Three checks, because the pitch pipeline spans two languages and a wire format:

| Command | What it proves |
|---------|----------------|
| `npm run check` | `svelte-check`: types across the app |
| `npm run check:tuning` | the app's pitch maths against meantonal — golden 31-EDO regression, the anchor convention, wire-frame encoding, validation, and the 12-TET self-check (every bend exactly 8192) |
| `npm run check:firmware` | extracts `pitch_for_xy` **verbatim** from `main.c`, compiles it with clang, and diffs it against the app over nine tunings × both layouts × a four-board grid — plus the byte-identical-default claim |

The 12-TET case is the one to keep: at a 700¢ fifth a wrong coefficient would not
collapse to equal temperament, a wrong anchor would disagree with meantonal's own
`midi` accessor, and any rounding bias would show as a bend off 8192.
