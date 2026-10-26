/*
 * ESP USB Wi-Fi Adapter - turns an ESP32-S2 / ESP32-S3 board into a USB network card that shares
 * its Wi-Fi internet connection with the PC. See README.md.
 *
 * Arduino IDE settings (Tools menu):
 *   Board:            ESP32S3 Dev Module  (or ESP32S2 Dev Module)
 *   USB Mode:         USB-OTG (TinyUSB)          (ESP32-S3 only)
 *   USB CDC On Boot:  Disabled
 *   Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 *   Flash Size:       4MB (works on any board with >= 4 MB)
 *
 * Requires arduino-esp32 3.3.2 exactly: usb_wifi.c works around bugs in the TinyUSB build that ships
 * precompiled with that core (see docs/TECHNICAL.md).
 *
 * The board's native USB port provides the network card plus a serial port that carries the log
 * (115200 baud); the UART port, if the board has one, carries the same log.
 */

#include "USB.h"
#include "USBCDC.h"
#include "tusb.h"
#include "usb_wifi.h"
#include "version.h"

#if ESP_ARDUINO_VERSION != ESP_ARDUINO_VERSION_VAL(3, 3, 2)
#warning "Validated with arduino-esp32 3.3.2 only; the USB fixes in usb_wifi.c depend on its precompiled TinyUSB"
#endif

USBCDC USBSerial;

// Mirrors the log onto the USB serial port, next to the UART port
static void log_to_usb(char c)
{
  if (tud_cdc_n_connected(0)) {
    tud_cdc_n_write_char(0, c);
    if (c == '\n') {
      tud_cdc_n_write_flush(0);
    }
  }
}

void setup()
{
  Serial.begin(115200);

  usb_wifi_register_usb();
  USBSerial.begin();
  USB.VID(0x303A);
  USB.PID(0x4001);
  USB.manufacturerName("Anik Sarker");
  USB.productName(FW_NAME);
  USB.firmwareVersion(0x0100);
  USB.begin();
  ets_install_putc2(log_to_usb);

  if (usb_wifi_start() != ESP_OK) {
    Serial.println("USB Wi-Fi adapter failed to start");
  }
}

void loop()
{
  vTaskDelete(NULL);  // everything runs in its own tasks
}
