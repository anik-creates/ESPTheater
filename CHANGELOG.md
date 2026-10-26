# Changelog

## 1.0.0 (2026-10-26)

First public release.

- ESP32-S3 and ESP32-S2 act as a USB (CDC-NCM) network card sharing their Wi-Fi connection (NAT).
- Setup hotspot with captive portal, plus a setup page at `http://192.168.7.1` from the PC.
- Auto-reconnect to the saved network; the hotspot returns when it's unavailable.
- Internet check, and a status LED (RGB or single colour, configurable GPIO and brightness).
- Settings: LED, setup hotspot password, restart.
- Works on Windows 11 after update KB5124008 (NCM divisor fix) and stays stable under sustained load
  (TinyUSB DWC2 workarounds, see docs/TECHNICAL.md).
- Windows flasher: chip auto-detection, full install or settings-preserving update, automatic bootloader
  entry, command-line mode.
