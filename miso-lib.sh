#!/bin/sh
# Shared helpers for flash-usb.sh and provision-board.sh.
# Sourced, not executed.

IDE=/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins
GCC_BIN=$(echo "$IDE"/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin)
PROG_CLI=$(echo "$IDE"/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/STM32_Programmer_CLI)

# 0xDF11 is ST's DFU product id. Matched on the number rather than the product
# string because that string is not the same across ST's bootloader revisions.
dfu_present() {
  ioreg -p IOUSB -l -w0 | grep -q '"idProduct" = 57105'
}

# CDC port of an attached Miso running the application. Its name is
# /dev/cu.usbmodem<serial><interface>, so the serial from the USB registry
# finds it without guessing at whatever else is plugged in. -d 1 keeps the
# query to the device node itself; the interface nodes under it repeat the
# same serial.
miso_cdc_port() {
  for serial in $(ioreg -r -n Miso -l -w0 -d 1 |
                  sed -n 's/.*"USB Serial Number" = "\(.*\)".*/\1/p' | sort -u); do
    for port in /dev/cu.usbmodem"$serial"*; do
      if [ -c "$port" ]; then echo "$port"; return 0; fi
    done
  done
  return 1
}

# Leave the board in DFU. No-op if it is already there.
enter_dfu() {
  dfu_present && return 0
  if ! port=$(miso_cdc_port); then
    echo "No Miso found: neither a DFU device nor a CDC port." >&2
    echo "Plug the board in, or double-tap reset if its USB never comes up." >&2
    return 1
  fi
  echo "Requesting DFU on $port"
  printf 'B!' > "$port"

  # The board acknowledges on CDC, holds 50 ms so that reaches the wire, then
  # resets; re-enumerating as a DFU device takes a moment longer again.
  waited=0
  while ! dfu_present; do
    if [ "$waited" -ge 50 ]; then
      echo "Board did not enter DFU within 10 s; double-tap reset instead." >&2
      return 1
    fi
    waited=$((waited + 1))
    sleep 0.2
  done
}

# Wait for the application to come back up on USB.
wait_for_app() {
  waited=0
  while ! miso_cdc_port >/dev/null 2>&1; do
    if [ "$waited" -ge 75 ]; then
      echo "Application did not re-enumerate within 15 s." >&2
      return 1
    fi
    waited=$((waited + 1))
    sleep 0.2
  done
}

# Current nSWBOOT0 option bit, as 0x0 or 0x1. Needs the board in DFU.
# 0x1 = BOOT0 read from the floating PB8 pin (the bug); 0x0 = from nBOOT0.
# Greps rather than strips ANSI colour, which carries no "0x" to confuse it.
ob_nswboot0() {
  "$PROG_CLI" -c port=usb1 -ob displ 2>/dev/null |
    grep -o 'nSWBOOT0[^(]*' | grep -o '0x[0-9a-fA-F]*' | head -1
}

# Loud, because the failure it predicts looks like flaky hardware, not config.
warn_unprovisioned() {
  echo
  echo "  ============================================================"
  echo "  WARNING: this board's option bytes are not set."
  echo
  echo "  nSWBOOT0 = 0x1, so BOOT0 is read from PB8, which this design"
  echo "  leaves floating. The board will randomly come up in the ROM"
  echo "  bootloader with dark LEDs and look dead - most often on USB"
  echo "  replug or a pogo hot-plug. Firmware cannot intercept it."
  echo
  echo "  Fix it once with:  ./provision-board.sh"
  echo "  ============================================================"
  echo
}
