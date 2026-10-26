# Technical notes

## Architecture

```
 PC  <--USB NCM-->  [usb netif 192.168.7.1/24, DHCP server, NAPT]  <--lwIP-->  [Wi-Fi STA, DHCP client]  ))) router
                    [DNS relay :53]  [HTTP setup page :80]           [SoftAP 192.168.4.1, only during setup]
```

* **USB side:** TinyUSB CDC-NCM device. Frames from the PC are copied and handed to lwIP through a custom
  `esp_netif` (Ethernet netstack). Frames to the PC are queued and sent from the TinyUSB task.
* **Routing:** lwIP IPv4 forwarding with NAPT enabled on the USB interface. The Wi-Fi station is the default
  route.
* **DNS:** `dns_proxy.c` relays queries to the station's DNS server while online. Offline, it answers
  setup-hotspot clients with the hotspot IP (captive portal) and returns SERVFAIL to everyone else, so a PC
  with another connection falls back to it quickly.
* **Internet check:** `GET http://connectivitycheck.gstatic.com/generate_204`, expecting a 204 response.
* **Composite USB device:** CDC-ACM (log port) + CDC-NCM, VID:PID `303A:4001`. The CDC port also supports
  the Arduino "1200 baud touch" to reboot into the ROM bootloader, which the flasher uses.

## Workarounds for the TinyUSB in arduino-esp32 3.3.2

The core ships TinyUSB 0.18 precompiled with an Espressif patch to the DWC2 driver. A plain NCM device
built on it fails on current Windows and stalls under load. `usb_wifi.c` works around the following, without
rebuilding the library.

1. **Windows rejects the NCM function (Code 10, `STATUS_DEVICE_HARDWARE_ERROR`).** Windows update KB5124008
   (September 2026) makes `usbncm.sys` require `wNdbIn/OutDivisor >= 4`, and TinyUSB answers 1
   (TinyUSB issue #3913, fixed upstream in PR #3914). The firmware registers TinyUSB's own `netd_*` functions
   as an *application class driver* (`usbd_app_driver_get_cb`), which takes precedence over the built-in
   entry, and answers `GET_NTB_PARAMETERS` itself with divisor 4.
2. **The NCM class driver is not thread-safe.** All `tud_network_*` calls run in the TinyUSB task, reached
   with `usbd_defer_func`. lwIP only queues frames.
3. **Recursive receive.** `tud_network_recv_renew()` delivers the next datagram of the same NTB by
   re-entering the receive callback. Calling it inside the callback recursed up to six levels and overflowed
   the 4 KB USB task stack. It is deferred instead.
4. **TX FIFO numbering.** Espressif's patch assigns TX FIFOs to IN endpoints in activation order, while
   TinyUSB sizes FIFO *n* for endpoint *n*. Endpoints ended up on FIFOs laid out for other endpoints (or left at
   power-on defaults). Every IN endpoint is pointed back at its own FIFO (`DIEPCTLn.TxFNum = n`) while idle,
   and the NCM data endpoint gets a 4-packet FIFO.
5. **Lost FIFO data.** Occasionally the FIFO refill in the interrupt handler loses a packet. The transfer
   then owes packets that are no longer in the FIFO and never completes, freezing the link. A 5 ms software
   watchdog notices an IN transfer with no completion for 20 ms. Only then does it read the controller
   registers; if the transfer is in that state, it disables the endpoint, flushes the FIFO and reports the
   transfer as complete. That costs a packet or two, which TCP resends. Polling the registers continuously
   from the other core made the loss several times more frequent, which is why the watchdog stays off the
   hardware during normal traffic.
6. **Last resort:** if nothing has been sent for 10 s with frames queued, the device re-enumerates.

Health counters for these appear under `usb` in `/api/status` (`recoveries`, `rearms`, `fifo_fixes`, `resets`).

## Flash layout

4 MB "Huge APP" partition table: NVS at `0x9000` (Wi-Fi credentials, settings namespace `usbwifi`) and the app
at `0x10000`. The *full* image covers bootloader, partition table, NVS (blank) and app. The *app* image only
replaces the application, so settings survive.

## Throughput

The ESP32-S2/S3 USB is Full-Speed (12 Mbit/s raw). With NCM framing and TinyUSB's single transmit buffer,
real-world throughput is ~6–7 Mbit/s in each direction.
