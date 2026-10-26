# ESP USB Wi-Fi Adapter

Turn a cheap **ESP32-S3** or **ESP32-S2** development board into a **USB Wi-Fi adapter** for your PC.

The board joins your Wi-Fi network and appears to the computer as a normal USB network card. No drivers are
needed on Windows 10/11, Linux or macOS. It's handy for desktops without Wi-Fi, for when a laptop's Wi-Fi
card is dead, or for adding a second network.

```
  Wi-Fi router  ))))  ESP32-S3 / S2 board  ==USB==  PC  (sees "ESP USB Wi-Fi Adapter", gets 192.168.7.x)
```

* **Captive-portal setup.** On first start the board opens a hotspot where you pick your Wi-Fi network. You can
  also open `http://192.168.7.1` on the PC itself.
* **Auto-reconnect.** It remembers the network and reconnects on its own. If the network disappears, the
  setup hotspot comes back.
* **Status LED.** The LED shows whether Wi-Fi and internet work and whether the PC is attached. RGB and
  single-colour LEDs are supported, and the GPIO is configurable.
* **Built-in internet check.** The board checks the connection itself (and detects hotel-style login pages).
* **Speed.** About **6–7 Mbit/s** (~800 KB/s) in both directions. That's enough for browsing, chat, YouTube
  up to 720p and medium downloads.
* **One-click Windows flasher.** No Arduino IDE or Python needed.

---

## Supported boards

| Chip | Works? | Typical boards |
|---|---|---|
| **ESP32-S3** | ✅ | ESP32-S3-DevKitC-1, "N16R8"/"N8R2" boards, YD-ESP32-S3, Seeed XIAO ESP32S3, S3 Zero/Mini |
| **ESP32-S2** | ✅ | ESP32-S2-Saola-1, ESP32-S2-DevKitM-1, LOLIN S2 Mini, S2 Zero |
| ESP32 (classic "ESP32 DevKit"), ESP32-C3, ESP32-C6, ESP32-C5, ESP8266 | ❌ | impossible, see below |

**Why only S2/S3?** The board has to *be* a USB network card, which needs USB device (OTG) hardware inside
the chip. Among ESP chips with Wi-Fi, only the ESP32-S2 and ESP32-S3 have it. On the classic ESP32,
ESP8266 and most C3/C6 boards, the USB connector goes to a separate USB-to-serial chip (or a fixed
serial/JTAG unit), so the PC can only ever see a COM port. The flasher detects these chips and tells you.

Any S2/S3 board with **4 MB of flash or more** works.

---

## Quick start (Windows)

1. **Download.** Click **Code → Download ZIP** on GitHub (or grab the ZIP from *Releases*) and extract it.
2. **Plug in the board.** On boards with two USB-C ports, either one works for flashing.
3. **Flash.** Run **`release\ESP-USB-WiFi-Flasher.exe`**, pick the port and click **Flash**.
   *Windows SmartScreen may warn about an unknown publisher: click **More info → Run anyway**. The EXE is
   built from the source in `flasher/` (see [Building](#building-from-source)).*
4. **Connect the native USB port.** That's the port often labelled **USB**, as opposed to **COM**/**UART**.
   Windows adds a network adapter called **"ESP USB Wi-Fi Adapter"**.
5. **Choose your Wi-Fi network.** Either:
   * join the hotspot **`ESP-USB-WiFi-XXXX`** from your phone. The setup page opens by itself, or go to
     `http://192.168.4.1`; or
   * open **`http://192.168.7.1`** in a browser on the PC.

   Pick the network, enter the password and press **Connect**. When the LED turns **green**, you're online.

> If the flasher says it can't talk to the board, put the board in download mode: hold **BOOT**, tap
> **RESET** (or plug the cable in while holding BOOT), release **BOOT**, click **Refresh** and **Flash** again.

### Flashing without Windows (Linux / macOS / command line)

Use [esptool](https://github.com/espressif/esptool) (`pip install esptool`) with the images in `release/firmware/`:

```bash
# full install (clears saved Wi-Fi and settings)
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write-flash 0x0 release/firmware/esp-usb-wifi-esp32s3-full.bin
# update only (keeps saved Wi-Fi and settings)
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write-flash 0x10000 release/firmware/esp-usb-wifi-esp32s3-app.bin
```

Replace `esp32s3` with `esp32s2` for S2 boards. Checksums are in `release/SHA256SUMS.txt`.

The flasher can also be scripted: `ESP-USB-WiFi-Flasher.exe --list`, or
`ESP-USB-WiFi-Flasher.exe --port COM5 [--chip auto|esp32s3|esp32s2] [--update] [--erase] [--baud 921600]`.

---

## Using it

### Status LED

| RGB LED | Single-colour LED | Meaning |
|---|---|---|
| 🔵 blue, slow blink | short flash every 1.2 s | Setup hotspot on: choose a Wi-Fi network |
| 🟡 yellow, fast blink | fast blink | Connecting to Wi-Fi |
| 🟡 yellow | fast blink | Connected, getting an IP / checking internet |
| 🔴 red | double flash | Connected to Wi-Fi, but it has **no internet** (or needs a login page) |
| 🩵 cyan | slow blink | Internet OK, but **no PC** on the board's native USB port |
| 🟢 green | on | Internet OK and the PC is connected: everything works |

The default LED is GPIO48 (ESP32-S3) or GPIO18 (ESP32-S2), the usual on-board WS2812. If your board's
LED stays dark (for example ESP32-S3-DevKitC-1 **v1.1** uses GPIO38), change it under **Settings** on the
setup page.

### Setup page

Open `http://192.168.7.1` from the PC (or `http://192.168.4.1` from the setup hotspot). It lets you:

* see the connection, internet and PC status;
* scan for networks, connect, forget the saved network, or join a hidden network;
* under **Settings**: set the LED GPIO, LED type and brightness, set the **setup hotspot password** (open by
  default), and restart the device.

Machine-readable status is available at `http://192.168.7.1/api/status`.

### Behaviour

* On start-up the board connects to the saved network. If none is saved, or it fails 5 times in a row (about
  25 s), it opens the setup hotspot and keeps retrying the saved network every 20 s. Once it connects,
  the hotspot closes again.
* The PC is on a private network (`192.168.7.0/24`) and the board shares its Wi-Fi connection with it
  through NAT, like a small router. DNS goes through the board.
* If the PC also has another internet connection (such as phone tethering), Windows prefers the faster one.
  Unplug the other connection to use the adapter for everything.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| No new network adapter on the PC | Use the board's **native** USB port (often labelled **USB**), not the COM/UART one. Try another cable, since some are charge-only. |
| Adapter shows "Code 10" in Device Manager | You're running another NCM firmware. This firmware includes the fix for the Windows update that causes this (see [docs/TECHNICAL.md](docs/TECHNICAL.md)). |
| LED red | The Wi-Fi network has no internet, or needs a browser login (hotel/café portals aren't supported). |
| LED cyan | The board is online but the PC isn't attached to its native USB port. |
| LED dark | Set the right LED GPIO under Settings, or choose a single-colour LED type. |
| Can't reach `192.168.7.1` | Check that the "ESP USB Wi-Fi Adapter" network adapter exists and is enabled. Re-plug the board. |
| Forgot the hotspot password | Re-flash with **Full install**, which clears all settings. |
| Slow | The board's USB is Full-Speed (12 Mbit/s raw), so ~7 Mbit/s is the ceiling. Also check Wi-Fi signal (RSSI in `/api/status`). |

The board logs to a serial port at 115200 baud: both the COM/UART port and a serial port on the native USB.

---

## Building from source

Requirements (Windows): **Arduino IDE 2** (bundles `arduino-cli`) or `arduino-cli`, with the
**esp32 core 3.3.2** installed. The firmware works around bugs in that exact core's precompiled TinyUSB, so
use exactly 3.3.2. The flasher is compiled with the C# compiler that ships with Windows, so no SDK is needed.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -InstallCore
```

This builds both firmwares into `release/firmware/`, the flasher into `release/ESP-USB-WiFi-Flasher.exe`, and a
release ZIP into `build/dist/`.

To build in the Arduino IDE instead, open `firmware/EspUsbWifiAdapter/EspUsbWifiAdapter.ino` and use the
settings listed at the top of that file.

**Releases:** push a tag like `v1.0.1`. The GitHub Actions workflow in `.github/workflows/release.yml` builds
everything and publishes a GitHub Release with the ZIP. Bump the version in
`firmware/EspUsbWifiAdapter/version.h` and `flasher/Flasher.cs` (`BuildInfo.Version`) first; the build checks
that they match.

---

## Project layout

```
firmware/EspUsbWifiAdapter/   Arduino sketch (ESP-IDF APIs + TinyUSB)
  usb_wifi.c                  Wi-Fi manager, USB NCM network card, NAT, setup page API, LED, USB fixes
  dns_proxy.c                 DNS relay / captive-portal DNS
  portal_html.cpp             setup page
flasher/                      Windows flasher (C#, WinForms) + icon generator
scripts/build.ps1             builds firmware + flasher + release ZIP
release/                      ready-to-use flasher EXE and firmware images
docs/TECHNICAL.md             how it works, and the USB driver bugs it works around
.github/workflows/release.yml CI release pipeline
```

## License

MIT, see [LICENSE](LICENSE). The flasher embeds esptool (GPL-2.0), and the firmware includes ESP-IDF, the
arduino-esp32 core, TinyUSB and lwIP. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
