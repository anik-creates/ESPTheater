#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Adds the USB NCM network interface; call before USB.begin()
esp_err_t usb_wifi_register_usb(void);

// Starts Wi-Fi, the Wi-Fi <-> USB bridge and the setup portal manager
esp_err_t usb_wifi_start(void);

#ifdef __cplusplus
}
#endif
