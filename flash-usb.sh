#!/bin/sh
# Build the firmware and flash it over USB DFU.
#
# No button press needed: if the board is running the application, this sends
# `B!` on its CDC port to reboot it into the ROM bootloader. If it is already
# in DFU — double-tapped reset, or a bad flash that never enumerates — that
# step is skipped and it flashes straight away.
set -e

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

# Build first: a compile error should not leave the board sitting in DFU.
PATH="$GCC_BIN:$PATH" make -C "$(dirname "$0")/Debug" all

if ! dfu_present; then
  if ! port=$(miso_cdc_port); then
    echo "No Miso found: neither a DFU device nor a CDC port." >&2
    echo "Plug the board in, or double-tap reset if its USB never comes up." >&2
    exit 1
  fi
  echo "Requesting DFU on $port"
  printf 'B!' > "$port"

  # The board acknowledges on CDC, holds 50 ms so that reaches the wire, then
  # resets; re-enumerating as a DFU device takes a moment longer again.
  waited=0
  while ! dfu_present; do
    if [ "$waited" -ge 50 ]; then
      echo "Board did not enter DFU within 10 s; double-tap reset instead." >&2
      exit 1
    fi
    waited=$((waited + 1))
    sleep 0.2
  done
fi

"$PROG_CLI" -c port=usb1 -w "$(dirname "$0")/Debug/Miso.elf" -v -g
