#!/bin/sh
# Build the firmware and flash it over USB DFU.
# Prerequisite: the board must be in DFU mode first — double-tap the reset
# button (two quick presses), then check it enumerated:
#   system_profiler SPUSBDataType | grep -i bootloader
set -e

IDE=/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins
GCC_BIN=$(echo "$IDE"/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin)
PROG_CLI=$(echo "$IDE"/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/STM32_Programmer_CLI)

PATH="$GCC_BIN:$PATH" make -C "$(dirname "$0")/Debug" all
"$PROG_CLI" -c port=usb1 -w "$(dirname "$0")/Debug/Miso.elf" -v -g
