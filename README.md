This repository is the firmware for Miso, a modular, isomorphic musical keyboard with the following features:
- An STM32G431KBU6 microcontroller as its brain.
- Two CD74HC4067 analog multiplex chips connected to the 31 Texas Instruments DRV5055 Hall effect sensors that power the velocity-sensitive key detection.
- A chain of 31 SK6812mini-e RGB LEDs that backlight the keys and allow users to see the key mapping they are currently using visually.
- Four pogo pin connectors, 2 male and 2 female, to the top, bottom, left and right sides of the device which transmit +5V power and allow for UART communication with adjacent boards when connected together. The STM32G431KBU6 only has 3 built-in UART capable pin pairs, so one of these four UART connections will be achieved via bit-banging.
