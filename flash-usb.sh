#!/bin/sh
# Build the firmware and flash it over USB DFU.
#
# No button press needed: if the board is running the application, this sends
# `B!` on its CDC port to reboot it into the ROM bootloader. If it is already
# in DFU — double-tapped reset, or a bad flash that never enumerates — that
# step is skipped and it flashes straight away.
#
# This only writes firmware. A board also needs its option bytes set once, which
# is a separate thing flashing never touches; see ./provision-board.sh. This
# script checks them and warns, but deliberately does not change them.
set -e

HERE=$(dirname "$0")
. "$HERE/miso-lib.sh"

# Build first: a compile error should not leave the board sitting in DFU.
PATH="$GCC_BIN:$PATH" make -C "$HERE/Debug" all

enter_dfu

# Read the option bytes while we are in DFU anyway: it costs no extra reset, and
# once the -g below has run the board is out of reach either way — in practice it
# sits with USB down until it is power-cycled. Warned about at the end, not here,
# so it is the last thing on screen rather than buried in the flash output.
nswboot0=$(ob_nswboot0)

"$PROG_CLI" -c port=usb1 -w "$HERE/Debug/Miso.elf" -v -g

[ "$nswboot0" = "0x1" ] && warn_unprovisioned
exit 0
