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
