# Third-party notices

This project's own source code is MIT licensed (see `LICENSE`). The distributed binaries also contain the
following components.

## Windows flasher (`ESPTheater-Flasher.exe`)

| Component | License | Source |
|---|---|---|
| esptool (`esptool.exe`, embedded unmodified and run as a separate process) | GPL-2.0-or-later | https://github.com/espressif/esptool |

esptool is Copyright (C) Fredrik Ahlberg, Angus Gratton, Espressif Systems (Shanghai) CO LTD and other
contributors. The complete corresponding source code is available at the URL above, matching the version
printed by the flasher's log (`esptool v...`).

## Android flasher (`ESPTheater-Flasher.apk`)

No third-party code: it uses only the Android framework. The firmware images it contains are covered below.

## Firmware images (`release/firmware/*.bin`)

| Component | License | Source |
|---|---|---|
| ESP-IDF (via arduino-esp32 precompiled libraries) | Apache-2.0 | https://github.com/espressif/esp-idf |
| arduino-esp32 core 3.3.2 | LGPL-2.1-or-later | https://github.com/espressif/arduino-esp32 |
| TinyUSB | MIT | https://github.com/hathach/tinyusb |
| lwIP | BSD-3-Clause | https://savannah.nongnu.org/projects/lwip/ |
| FreeRTOS | MIT | https://www.freertos.org |

To relink the firmware against a modified arduino-esp32 core (LGPL), build from source with
`scripts/build.ps1` (see README).
