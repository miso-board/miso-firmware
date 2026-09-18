#!/bin/sh
# Bring a new board fully up: firmware + option bytes, verified.
#
# Run this once per new board. Safe to re-run: the option bytes are only
# written when they are actually wrong.
#
# Order matters. The firmware goes on FIRST, because writing the option bytes
# triggers an option-byte-load reset, and with nSWBOOT0=0 in effect that reset
# boots main flash. On a board with empty flash that means nothing runs: no CDC
# port for `B!`, and no double-tap either (bootloader_check() is itself
# firmware), so USB DFU is gone and only the SWD header can recover it. Flashing
# first means that same reset lands in a working application.
#
# Leaving DFU is done by a real reset, never a bare `-g`. `-g` is only reliable
# chained onto the same `-w` invocation (what flash-usb.sh does); issued on its
# own it can leave the board hung and off USB until it is power-cycled.
set -e

HERE=$(dirname "$0")
. "$HERE/miso-lib.sh"

PATH="$GCC_BIN:$PATH" make -C "$HERE/Debug" all >/dev/null
echo "  Building firmware...          ok"

enter_dfu >/dev/null
before=$(ob_nswboot0)

if [ "$before" != "0x1" ]; then
  # Nothing to change, so this is just a flash: use the proven -w ... -g path.
  "$PROG_CLI" -c port=usb1 -w "$HERE/Debug/Miso.elf" -v -g >/dev/null
  echo "  Flashing firmware...          ok"
  echo "  Option bytes: nSWBOOT0=$before    already set, nothing to do"
  echo "  Board provisioned."
  exit 0
fi

# No -g: stay in DFU so the option bytes can be written in the same session,
# and let their reset be what starts the application.
"$PROG_CLI" -c port=usb1 -w "$HERE/Debug/Miso.elf" -v >/dev/null
echo "  Flashing firmware...          ok"
echo "  Option bytes: nSWBOOT0=$before    -> fixing"

# This reports failure even when it succeeds: the option-byte-load reset drops
# the board off the DFU bus mid-operation, so CubeProgrammer's reconnect finds
# nothing. The exit status is therefore meaningless here — the readback below is
# the only thing that decides whether it worked.
"$PROG_CLI" -c port=usb1 -ob nSWBOOT0=0 nBOOT0=1 >/dev/null 2>&1 || true

# That reset boots the firmware flashed above. If it does not come back on USB,
# the write has most likely still succeeded — this board often needs a power
# cycle after a DFU session — so say so rather than reporting failure. Re-running
# is safe and will verify the option bytes.
if ! wait_for_app 2>/dev/null; then
  echo "  Option bytes written, but the board did not re-enumerate."
  echo "  Power-cycle it and re-run to verify; the write likely succeeded."
  exit 0
fi

enter_dfu >/dev/null
after=$(ob_nswboot0)

if [ "$after" != "0x0" ]; then
  echo "  Verifying...  nSWBOOT0=$after    FAILED" >&2
  echo "  Option bytes did not take. Board left in DFU; safe to re-run." >&2
  exit 1
fi
echo "  Verifying...  nSWBOOT0=$after    ok"

# Back out of DFU with a real reset. With nSWBOOT0=0 this is deterministic:
# BOOT0 comes from nBOOT0=1, so it boots main flash every time.
#
# Not coming back on USB here is normal, not a failure: in practice this board
# usually needs a power cycle to start the application after a DFU session. The
# option bytes are already written and verified by this point either way.
"$PROG_CLI" -c port=usb1 -rst >/dev/null 2>&1 || true

echo "  Board provisioned."
if ! wait_for_app 2>/dev/null; then
  echo "  Power-cycle it to start the application."
fi
