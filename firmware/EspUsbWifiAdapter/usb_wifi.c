/*
 * ESP USB Wi-Fi Adapter (ESP32-S2 / ESP32-S3)
 *
 * The ESP joins a Wi-Fi network as a normal client and routes (NAT) between it and a USB NCM
 * network card on the PC. The PC gets 192.168.7.x from the ESP's DHCP server; DNS goes through
 * the ESP (dns_proxy.c).
 *
 * Boot: connect to the saved network. If there is none, or it fails MAX_STA_FAILS times in a row,
 * a setup hotspot ("ESP-USB-WiFi-XXXX") is started. The setup page is served on the hotspot
 * (http://192.168.4.1) and on the USB network (http://192.168.7.1). While the hotspot is up the
 * saved network keeps being retried; once connected the hotspot shuts down again.
 *
 * Status LED (GPIO, type and brightness are settings, see settings_t):
 *   blue, slow blink   setup hotspot active, waiting for a network to be chosen
 *   yellow, fast blink connecting to Wi-Fi
 *   yellow             connected to Wi-Fi, getting an IP / checking internet
 *   red                connected to Wi-Fi but there is no internet access
 *   cyan               internet OK, but no PC on the USB port
 *   green              internet OK and the PC is connected over USB
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "device/dcd.h"
#include "class/net/ncm.h"
#include "esp32-hal-tinyusb.h"
#include "esp32-hal-rgb-led.h"
#include "soc/usb_struct.h"

#include "esp32-hal-gpio.h"
#include "nvs.h"

#include "dns_proxy.h"
#include "usb_wifi.h"
#include "version.h"

#if !CONFIG_IDF_TARGET_ESP32S2 && !CONFIG_IDF_TARGET_ESP32S3
#error "Only the ESP32-S2 and ESP32-S3 have the USB device (OTG) hardware this firmware needs"
#endif

#define AP_SSID_PREFIX      "ESP-USB-WiFi-"
#define HOSTNAME_PREFIX     "esp-usb-wifi-"
#define AP_CHANNEL          1

#if CONFIG_IDF_TARGET_ESP32S2
#define CHIP_NAME           "ESP32-S2"
#define DEFAULT_LED_PIN     18      // WS2812 on ESP32-S2-Saola-1 / DevKitM-1
#else
#define CHIP_NAME           "ESP32-S3"
#define DEFAULT_LED_PIN     48      // WS2812 on ESP32-S3-DevKitC-1 v1.0 and most clones (v1.1: GPIO38)
#endif
#define DEFAULT_LED_LEVEL   40      // RGB brightness 1-255
#define SETTINGS_NS         "usbwifi"

#define MAX_STA_FAILS       5       // consecutive failures before the hotspot opens
#define RETRY_DELAY_MS      3000
#define PORTAL_RETRY_MS     20000   // saved network retry interval while the hotspot is up
#define PORTAL_LINGER_MS    8000    // keep the hotspot up after connecting so the page can show success
#define CONNECT_TIMEOUT_MS  30000
#define MAX_SCAN_RESULTS    24
#define USB_TX_QUEUE_LEN    20      // frames buffered towards the PC (~30 KB, ~30 ms at full USB speed)
#define USB_STALL_MS        10000   // last resort: PC not taking data this long with frames queued -> reconnect USB
#define USB_FIFO_WATCHDOG_US 5000   // see usb_fifo_watchdog()
#define USB_STALL_RESET_GAP_MS 60000

#define PROBE_HOST          "connectivitycheck.gstatic.com"
#define PROBE_OK_MS         15000   // internet check interval while online
#define PROBE_FAIL_MS       5000    // ... and while offline

static const char *TAG = "usb_wifi";

extern const char portal_html[];

typedef enum {
    EV_STA_CONNECTED,
    EV_STA_DISCONNECTED,
    EV_NEW_CREDS,
    EV_FORGET,
} mgr_event_type_t;

typedef struct {
    mgr_event_type_t type;
    uint16_t reason;
    char ssid[33];
    char pass[65];
} mgr_event_t;

typedef struct {
    bool portal_active;
    bool connecting;
    bool has_creds;
    int fail_count;
    uint16_t last_reason;
    char ssid[33];
} mgr_state_t;

typedef enum {
    LED_RGB,                        // WS2812 / "NeoPixel"
    LED_PLAIN,                      // single-colour LED, on = high
    LED_PLAIN_INVERTED,             // single-colour LED, on = low
} led_type_t;

typedef struct {
    int8_t led_pin;                 // -1 = no LED
    uint8_t led_type;               // led_type_t
    uint8_t led_level;              // RGB brightness 1-255
    char ap_pass[65];               // setup hotspot password, empty = open network
} settings_t;

typedef enum {
    INET_UNKNOWN,
    INET_OK,
    INET_DOWN,
} inet_state_t;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_wifi_lock;       // serializes scan / connect / mode changes
static volatile bool s_sta_connected;
static volatile bool s_sta_has_ip;
static volatile inet_state_t s_inet;
static bool s_expect_leave;                 // next disconnect event was caused by us

static mgr_state_t s_st;                    // written only by the manager task
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static TickType_t s_connect_started;

static char s_ap_ssid[32];
static settings_t s_cfg;                    // guarded by s_st_mux
static esp_netif_t *s_sta_netif;
static TaskHandle_t s_probe_task;

static esp_netif_t *s_usb_netif;
static char s_usb_mac_str[13];
typedef struct {
    uint8_t *data;
    uint16_t len;
} usb_frame_t;

static QueueHandle_t s_usb_txq;             // frames waiting for the USB link (ESP -> PC)
static volatile bool s_usb_pump_pending;
static volatile TickType_t s_usb_last_tx_done;
static bool s_usb_mounted;
static volatile uint32_t s_usb_tx_drops, s_usb_rx_drops, s_usb_resets, s_usb_rearms, s_usb_fifo_fixes;
static volatile uint32_t s_usb_in_recoveries;
static volatile bool s_usb_fifo_fix_pending, s_usb_recover_pending;
static uint8_t s_ncm_ep_in;                 // NCM bulk IN endpoint address
static volatile uint32_t s_usb_in_done;     // completed NCM IN transfers

/* ------------------------------------------------------------------ USB network card */

/*
 * Windows fix: since update KB5124008 (Sept 2026) Windows' NCM driver requires wNdbIn/OutDivisor
 * to be >= 4, and fails the device with "Code 10" otherwise. TinyUSB's built-in driver (precompiled
 * into the Arduino core) answers 1 (fixed upstream in TinyUSB PR #3914). We register TinyUSB's own
 * NCM driver functions as an application driver, which takes precedence over the built-in one, and
 * answer GET_NTB_PARAMETERS ourselves with the corrected values.
 */
static const ntb_parameters_t s_ntb_parameters = {
    .wLength = sizeof(ntb_parameters_t),
    .bmNtbFormatsSupported = 0x01,      // 16-bit NTB
    .dwNtbInMaxSize = CFG_TUD_NCM_IN_NTB_MAX_SIZE,
    .wNdbInDivisor = 4,
    .wNdbInPayloadRemainder = 0,
    .wNdbInAlignment = 4,
    .wReserved = 0,
    .dwNtbOutMaxSize = CFG_TUD_NCM_OUT_NTB_MAX_SIZE,
    .wNdbOutDivisor = 4,
    .wNdbOutPayloadRemainder = 0,
    .wNdbOutAlignment = 4,
    .wNtbOutMaxDatagrams = CFG_TUD_NCM_OUT_MAX_DATAGRAMS_PER_NTB,
};

static void usb_fix_tx_fifos(void *arg);

static void ncm_init(void)
{
    // the built-in driver entry already initializes and resets the shared NCM state
}

static bool ncm_deinit(void)
{
    return true;
}

static void ncm_reset(uint8_t rhport)
{
}

static uint16_t ncm_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
    // App drivers are offered every interface first; only take the NCM one
    if (itf_desc->bInterfaceClass != TUSB_CLASS_CDC
            || itf_desc->bInterfaceSubClass != CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL) {
        return 0;
    }
    return netd_open(rhport, itf_desc, max_len);
}

static bool ncm_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage == CONTROL_STAGE_SETUP && request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS
            && request->bRequest == NCM_GET_NTB_PARAMETERS) {
        uint16_t len = tu_min16(request->wLength, sizeof(s_ntb_parameters));
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)&s_ntb_parameters, len);
    }
    bool ret = netd_control_xfer_cb(rhport, stage, request);
    if (stage == CONTROL_STAGE_SETUP && request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD
            && request->bRequest == TUSB_REQ_SET_INTERFACE) {
        usb_fix_tx_fifos(NULL);         // the data endpoints were just (re)activated, still idle
    }
    return ret;
}

static void usb_tx_pump(void *arg);

// A finished IN transfer frees TinyUSB's (single) transmit buffer: send the next queued frame
static bool ncm_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    bool ret = netd_xfer_cb(rhport, ep_addr, result, xferred_bytes);
    if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
        s_usb_in_done++;
        s_usb_last_tx_done = xTaskGetTickCount();
        usb_tx_pump(NULL);
    }
    return ret;
}

static const usbd_class_driver_t s_ncm_driver = {
    .name = "NCM",
    .init = ncm_init,
    .deinit = ncm_deinit,
    .reset = ncm_reset,
    .open = ncm_open,
    .control_xfer_cb = ncm_control_xfer_cb,
    .xfer_cb = ncm_xfer_cb,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_ncm_driver;
}

static void usb_rx_renew(void *arg)
{
    tud_network_recv_renew();
}

// PC -> ESP. The frame is copied because TinyUSB reuses its buffer after recv_renew.
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (s_usb_netif && size) {
        void *frame = malloc(size);
        if (frame) {
            memcpy(frame, src, size);
            esp_netif_receive(s_usb_netif, frame, size, frame);    // freed via usb_netif_free_rx
        } else {
            s_usb_rx_drops++;
        }
    }
    // recv_renew delivers the next datagram of the same transfer by calling us again; doing it
    // here would recurse once per datagram (up to 6) and overflow the USB task's stack
    usbd_defer_func(usb_rx_renew, NULL, false);
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    memcpy(dst, ref, arg);
    return arg;
}

void tud_network_init_cb(void)
{
}

/*
 * ESP -> PC. TinyUSB's NCM driver is not thread-safe, so lwIP only queues frames here and every
 * tud_network_* call happens in the TinyUSB task (usb_tx_pump). Calling it from the lwIP thread
 * raced with transfer completions and could jam the transmit side for good.
 */

// Runs in the TinyUSB task: hands queued frames to the NCM driver while it has room
static void usb_tx_pump(void *arg)
{
    s_usb_pump_pending = false;
    usb_frame_t frame;
    while (xQueuePeek(s_usb_txq, &frame, 0) == pdTRUE) {
        if (tud_mounted() && !tud_network_can_xmit(frame.len)) {
            break;                          // resumes when the current transfer completes
        }
        xQueueReceive(s_usb_txq, &frame, 0);
        if (tud_mounted()) {
            tud_network_xmit(frame.data, frame.len);    // copies the frame via tud_network_xmit_cb
        }
        free(frame.data);
    }
}

static void usb_tx_kick(void)
{
    if (!s_usb_pump_pending) {
        s_usb_pump_pending = true;
        usbd_defer_func(usb_tx_pump, NULL, false);
    }
}

/*
 * USB controller fixes for the precompiled TinyUSB in the Arduino core (S3 has IN endpoints/TX FIFOs 1-4).
 *
 * 1. Espressif's patch to the DWC2 driver hands TX FIFOs to IN endpoints in activation order
 *    (CDC 0x84 -> FIFO 1, NCM notify 0x83 -> FIFO 2, NCM data 0x81 -> FIFO 3), while the driver
 *    sizes/places FIFO n and checks its free space for endpoint n. Endpoints then write into FIFOs
 *    laid out for someone else (or left at overlapping power-on defaults); under load packet data
 *    is lost, the transfer never finishes and the link to the PC freezes. Every IN endpoint n
 *    already has a correctly sized FIFO n, so point it there (only while the endpoint is idle).
 *
 * 2. DIEPEMPMSK (TX FIFO empty interrupt enable) is updated with unprotected read-modify-writes from
 *    the interrupt handler and the USB task on different cores; re-enable it if it gets lost while
 *    an IN transfer still has bytes to load.
 *
 * 3. The interrupt handler that refills the TX FIFO occasionally overflows it (it can see a stale
 *    free-space value), dropping a packet: the transfer then owes packets that are no longer in the
 *    FIFO and can never finish. The NCM data endpoint gets a 4-packet FIFO (4x fewer refill
 *    boundaries, and fewer refill gaps) and a stuck transfer is aborted and reported complete after
 *    ~20 ms, costing a packet or two (TCP resends) instead of freezing the link.
 */
#define DEPCTL_EPENA        (1u << 31)
#define DEPCTL_EPDIS        (1u << 30)
#define DEPCTL_SNAK         (1u << 27)
#define DEPCTL_USBACTEP     (1u << 15)
#define DEPCTL_TXFNUM_S     22
#define DEPCTL_TXFNUM_M     (0xFu << DEPCTL_TXFNUM_S)
#define DIEPINT_EPDISD      (1u << 1)
#define DIEPINT_CLEAR       0x2052u     // EPDISD, IN token with FIFO empty, NAK effective, NAK
#define GRSTCTL_TXFFLSH     (1u << 5)
#define GRSTCTL_TXFNUM_S    6
#define NCM_TX_FIFO_START   64          // words; above TinyUSB's RX FIFO, below its TX FIFOs
#define NCM_TX_FIFO_WORDS   64          // 4 packets of 64 bytes
#define NCM_TX_FIFO_CFG     ((NCM_TX_FIFO_WORDS << 16) | NCM_TX_FIFO_START)
#define IN_STUCK_MS         20

static inline uint32_t ep_tx_fifo(uint32_t ctl)
{
    return (ctl & DEPCTL_TXFNUM_M) >> DEPCTL_TXFNUM_S;
}

static void usb_flush_tx_fifo(uint32_t fifo)
{
    USB0.grstctl = (fifo << GRSTCTL_TXFNUM_S) | GRSTCTL_TXFFLSH;
    for (int i = 0; i < 10000 && (USB0.grstctl & GRSTCTL_TXFFLSH); i++) {
    }
}

static bool usb_tx_fifo_mismatch(void)
{
    for (int n = 1; n <= 4; n++) {
        uint32_t ctl = USB0.in_ep_reg[n].diepctl;
        if ((ctl & DEPCTL_USBACTEP) && ep_tx_fifo(ctl) != n) {
            return true;
        }
    }
    uint8_t ep = s_ncm_ep_in & 0x7F;
    return ep >= 1 && ep <= 4 && (USB0.in_ep_reg[ep].diepctl & DEPCTL_USBACTEP) && USB0.dieptxf[ep - 1] != NCM_TX_FIFO_CFG;
}

// Runs in the TinyUSB task: aborts the stuck NCM IN transfer and lets the NCM driver carry on
static void usb_recover_in(void *arg)
{
    s_usb_recover_pending = false;
    uint8_t ep = s_ncm_ep_in & 0x7F;
    usb_in_endpoint_t *in = &USB0.in_ep_reg[ep];
    if (!(in->diepctl & DEPCTL_EPENA) || (in->dieptsiz & 0x7FFFF)) {
        return;                         // finished meanwhile, or not the stuck state
    }
    in->diepctl |= DEPCTL_SNAK | DEPCTL_EPDIS;
    for (int i = 0; i < 100000 && !(in->diepint & DIEPINT_EPDISD); i++) {
    }
    in->diepint = DIEPINT_CLEAR;
    usb_flush_tx_fifo(ep_tx_fifo(in->diepctl));
    s_usb_in_recoveries++;
    // 1 byte: not a multiple of the packet size, so the NCM driver won't queue a ZLP
    dcd_event_xfer_complete(0, s_ncm_ep_in, 1, XFER_RESULT_SUCCESS, false);
}

// Must run in the TinyUSB task, so no transfer can be started while an endpoint is being moved
static void usb_fix_tx_fifos(void *arg)
{
    s_usb_fifo_fix_pending = false;
    for (int n = 1; n <= 4; n++) {
        uint32_t ctl = USB0.in_ep_reg[n].diepctl;
        if (!(ctl & DEPCTL_USBACTEP) || ep_tx_fifo(ctl) == n || (ctl & DEPCTL_EPENA)) {
            continue;
        }
        bool fifo_busy = false;         // FIFO n still serving a transfer of another endpoint
        for (int m = 1; m <= 4; m++) {
            uint32_t other = USB0.in_ep_reg[m].diepctl;
            fifo_busy |= m != n && (other & DEPCTL_EPENA) && ep_tx_fifo(other) == n;
        }
        if (fifo_busy) {
            continue;
        }
        USB0.in_ep_reg[n].diepctl = (ctl & ~DEPCTL_TXFNUM_M) | ((uint32_t)n << DEPCTL_TXFNUM_S);
        usb_flush_tx_fifo(n);
        s_usb_fifo_fixes++;
    }

    uint8_t ep = s_ncm_ep_in & 0x7F;
    if (ep >= 1 && ep <= 4) {
        uint32_t ctl = USB0.in_ep_reg[ep].diepctl;
        if ((ctl & DEPCTL_USBACTEP) && ep_tx_fifo(ctl) == ep && !(ctl & DEPCTL_EPENA)
                && USB0.dieptxf[ep - 1] != NCM_TX_FIFO_CFG && (USB0.grxfsiz & 0xFFFF) <= NCM_TX_FIFO_START) {
            USB0.dieptxf[ep - 1] = NCM_TX_FIFO_CFG;
            usb_flush_tx_fifo(ep);
            s_usb_fifo_fixes++;
        }
    }
}

/*
 * Every 5 ms, using only software state: has the NCM IN endpoint had a transfer pending with no
 * completion for IN_STUCK_MS? Only then are the controller registers read to classify and fix it.
 * (Polling the registers continuously, while the interrupt handler loads the FIFO from the other
 * core, made the overflow several times more frequent.)
 */
static void usb_fifo_watchdog(void *arg)
{
    static uint32_t last_in_done, ticks;
    static int idle_ms;
    uint8_t ep = s_ncm_ep_in & 0x7F;
    ticks++;
    if (ep >= 1 && ep <= 4) {
        bool waiting = usbd_edpt_busy(0, s_ncm_ep_in) && s_usb_in_done == last_in_done;
        idle_ms = waiting ? idle_ms + USB_FIFO_WATCHDOG_US / 1000 : 0;
        last_in_done = s_usb_in_done;
        if (idle_ms >= IN_STUCK_MS && !s_usb_recover_pending) {
            idle_ms = 0;
            uint32_t ctl = USB0.in_ep_reg[ep].diepctl;
            uint32_t tsiz = USB0.in_ep_reg[ep].dieptsiz;
            uint32_t fifo = ep_tx_fifo(ctl);
            uint32_t depth = (fifo >= 1 && fifo <= 4) ? USB0.dieptxf[fifo - 1] >> 16 : 16;
            uint32_t bit = 1u << ep;
            if ((ctl & DEPCTL_EPENA) && (tsiz & 0x7FFFF) && !(USB0.dtknqr4_fifoemptymsk & bit)) {
                USB0.dtknqr4_fifoemptymsk |= bit;       // lost FIFO-empty interrupt enable (fix 2)
                s_usb_rearms++;
            } else if ((ctl & DEPCTL_EPENA) && (tsiz & 0x7FFFF) == 0  // every byte handed to the FIFO,
                       && ((tsiz >> 19) & 0x3FF)                      // packets still owed,
                       && (USB0.in_ep_reg[ep].dtxfsts & 0xFFFF) >= depth) {   // yet the FIFO is empty (fix 3)
                s_usb_recover_pending = true;
                usbd_defer_func(usb_recover_in, NULL, false);
            }
            // otherwise the PC simply isn't reading right now; nothing to fix
        }
    }
    if (ticks % (1000000 / USB_FIFO_WATCHDOG_US) == 0 && !s_usb_fifo_fix_pending && usb_tx_fifo_mismatch()) {
        s_usb_fifo_fix_pending = true;  // once a second: endpoints still on the wrong FIFO (fix 1)
        usbd_defer_func(usb_fix_tx_fifos, NULL, false);
    }
}

static void usb_set_link_up(void *arg)
{
    tud_network_link_state(0, true);
}

// Drops off the bus and comes back, which resets the NCM driver and the PC's side of the link
static void usb_reenumerate(void *arg)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    tud_connect();
}

static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    if (!tud_mounted() || len > CFG_TUD_NET_MTU) {
        return ESP_FAIL;
    }
    usb_frame_t frame = { .data = malloc(len), .len = len };
    if (!frame.data) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(frame.data, buffer, len);
    if (xQueueSend(s_usb_txq, &frame, 0) != pdTRUE) {
        free(frame.data);                   // PC isn't keeping up: drop, TCP will resend
        s_usb_tx_drops++;
        return ESP_FAIL;
    }
    usb_tx_kick();
    return ESP_OK;
}

static esp_err_t usb_netif_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return usb_netif_transmit(h, buffer, len);
}

static void usb_netif_free_rx(void *h, void *buffer)
{
    free(buffer);
}

static uint16_t ncm_descriptor_cb(uint8_t *dst, uint8_t *itf)
{
    uint8_t str_index = tinyusb_add_string_descriptor(FW_NAME);
    uint8_t mac_index = tinyusb_add_string_descriptor(s_usb_mac_str);
    uint8_t ep_notif = tinyusb_get_free_in_endpoint();
    uint8_t ep_in = tinyusb_get_free_in_endpoint();
    uint8_t ep_out = tinyusb_get_free_out_endpoint();
    if (!ep_notif || !ep_in || !ep_out) {
        return 0;
    }
    s_ncm_ep_in = 0x80 | ep_in;
    uint8_t descriptor[TUD_CDC_NCM_DESC_LEN] = {
        TUD_CDC_NCM_DESCRIPTOR(*itf, str_index, mac_index, (uint8_t)(0x80 | ep_notif), 64,
                               ep_out, (uint8_t)(0x80 | ep_in), CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU)
    };
    *itf += 2;
    memcpy(dst, descriptor, sizeof(descriptor));
    return sizeof(descriptor);
}

// Locally administered MACs derived from the station MAC: ESP side of the USB link, and the PC side
static void usb_link_mac(uint8_t mac[6], bool pc_side)
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[0] |= 0x02;
    if (pc_side) {
        mac[5] ^= 0x01;
    }
}

esp_err_t usb_wifi_register_usb(void)
{
    uint8_t mac[6];
    usb_link_mac(mac, true);
    snprintf(s_usb_mac_str, sizeof(s_usb_mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_usb_txq = xQueueCreate(USB_TX_QUEUE_LEN, sizeof(usb_frame_t));
    return tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, ncm_descriptor_cb);
}

// The USB side: 192.168.7.1/24 with a DHCP server, NAT towards the Wi-Fi station
static esp_err_t usb_netif_init(void)
{
    static const esp_netif_ip_info_t ip_info = {
        .ip = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
    };
    static int driver_handle;   // the driver needs no state, but the handle must be non-NULL

    esp_netif_inherent_config_t base = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "USB_NCM",
        .if_desc = "usb",
        .route_prio = 5,
    };
    usb_link_mac(base.mac, false);
    esp_netif_driver_ifconfig_t driver = {
        .handle = &driver_handle,
        .transmit = usb_netif_transmit,
        .transmit_wrap = usb_netif_transmit_wrap,
        .driver_free_rx_buffer = usb_netif_free_rx,
    };
    esp_netif_config_t cfg = {
        .base = &base,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_usb_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_usb_netif, ESP_FAIL, TAG, "Cannot create USB network interface");
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    return esp_netif_napt_enable(s_usb_netif);
}

// Logs USB attach/detach, keeps the link reported as up, and restarts a stalled transmit queue
static void usb_poll(void)
{
    static TickType_t last_reset;
    TickType_t now = xTaskGetTickCount();
    bool mounted = tud_mounted();
    if (mounted != s_usb_mounted) {
        s_usb_mounted = mounted;
        if (mounted) {
            ESP_LOGI(TAG, "PC connected on the USB port");
            s_usb_last_tx_done = now;
            usbd_defer_func(usb_set_link_up, NULL, false);
        } else {
            ESP_LOGW(TAG, "No PC on the USB port");
        }
    }
    if (uxQueueMessagesWaiting(s_usb_txq)) {
        if (mounted && !tud_suspended() && now - s_usb_last_tx_done > pdMS_TO_TICKS(USB_STALL_MS)
                && (last_reset == 0 || now - last_reset > pdMS_TO_TICKS(USB_STALL_RESET_GAP_MS))) {
            ESP_LOGW(TAG, "USB link stalled for %d s, reconnecting USB", USB_STALL_MS / 1000);
            last_reset = now;
            s_usb_last_tx_done = now;
            s_usb_resets++;
            usbd_defer_func(usb_reenumerate, NULL, false);
        }
        s_usb_pump_pending = false;         // a lost kick must never stall the queue
        usb_tx_kick();
    }
}

/* ------------------------------------------------------------------ helpers */

static const char *reason_text(uint16_t reason)
{
    switch (reason) {
    case 0:
        return "";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Wrong password";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connection failed";
    default:
        return "Disconnected";
    }
}

static const char *reset_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout (power dip)";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
    }
}

static const char *inet_text(void)
{
    if (!s_sta_has_ip) {
        return "";
    }
    return s_inet == INET_OK ? "ok" : s_inet == INET_DOWN ? "down" : "checking";
}

static bool is_online(void)
{
    return s_sta_has_ip;
}

static mgr_state_t state_snapshot(void)
{
    mgr_state_t st;
    taskENTER_CRITICAL(&s_st_mux);
    st = s_st;
    taskEXIT_CRITICAL(&s_st_mux);
    return st;
}

static void state_set_ssid(const char *ssid)
{
    taskENTER_CRITICAL(&s_st_mux);
    strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    s_st.has_creds = ssid[0] != '\0';
    taskEXIT_CRITICAL(&s_st_mux);
}

// Appends a JSON string literal (with quotes) for src to dst
static void json_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(dst);
    if (n + 2 >= size) {
        return;
    }
    dst[n++] = '"';
    for (; *src && n + 8 < size; src++) {
        unsigned char c = *src;
        if (c == '"' || c == '\\') {
            dst[n++] = '\\';
            dst[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(dst + n, size - n, "\\u%04x", c);
        } else {
            dst[n++] = c;
        }
    }
    dst[n++] = '"';
    dst[n] = '\0';
}

// Decodes an application/x-www-form-urlencoded value; returns false if it doesn't fit
static bool url_decode(const char *src, char *dst, size_t size)
{
    size_t n = 0;
    while (*src) {
        if (n + 1 >= size) {
            return false;
        }
        if (*src == '+') {
            dst[n++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            dst[n++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[n++] = *src++;
        }
    }
    dst[n] = '\0';
    return true;
}

/* ------------------------------------------------------------------ settings */

static settings_t settings_snapshot(void)
{
    settings_t cfg;
    taskENTER_CRITICAL(&s_st_mux);
    cfg = s_cfg;
    taskEXIT_CRITICAL(&s_st_mux);
    return cfg;
}

static void settings_load(void)
{
    settings_t cfg = { .led_pin = DEFAULT_LED_PIN, .led_type = LED_RGB, .led_level = DEFAULT_LED_LEVEL };
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i8(h, "led_pin", &cfg.led_pin);
        nvs_get_u8(h, "led_type", &cfg.led_type);
        nvs_get_u8(h, "led_level", &cfg.led_level);
        size_t len = sizeof(cfg.ap_pass);
        if (nvs_get_str(h, "ap_pass", cfg.ap_pass, &len) != ESP_OK) {
            cfg.ap_pass[0] = '\0';
        }
        nvs_close(h);
    }
    s_cfg = cfg;
}

static esp_err_t settings_save(const settings_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NS, NVS_READWRITE, &h), TAG, "settings open");
    esp_err_t err = nvs_set_i8(h, "led_pin", cfg->led_pin);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "led_type", cfg->led_type);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "led_level", cfg->led_level);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ap_pass", cfg->ap_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_st_mux);
        s_cfg = *cfg;
        taskEXIT_CRITICAL(&s_st_mux);
    }
    return err;
}

// GPIOs that exist and can drive an LED: not 22-25 (absent), USB (19/20) or SPI flash/PSRAM (26-32)
static bool led_pin_valid(int pin)
{
    if (pin == -1) {
        return true;
    }
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT || (pin >= 19 && pin <= 32 && pin != 21)) {
        return false;
    }
#if CONFIG_IDF_TARGET_ESP32S2
    return pin != 46;                       // input only
#else
    return true;
#endif
}

/* ------------------------------------------------------------------ internet check & LED */

// HTTP request to Google's connectivity check; a captive login page or no route counts as offline
static bool internet_probe(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(PROBE_HOST, "80", &hints, &res) != 0 || !res) {
        return false;
    }
    bool ok = false;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // Non-blocking connect so an unreachable host can't stall for minutes
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        bool connected = connect(s, res->ai_addr, res->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            int err = 0;
            socklen_t err_len = sizeof(err);
            connected = select(s + 1, NULL, &wfds, NULL, &tv) == 1
                        && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0;
        }
        fcntl(s, F_SETFL, flags);

        static const char request[] = "GET /generate_204 HTTP/1.1\r\nHost: " PROBE_HOST "\r\nConnection: close\r\n\r\n";
        char reply[16] = { 0 };
        if (connected && send(s, request, sizeof(request) - 1, 0) > 0
                && recv(s, reply, sizeof(reply) - 1, MSG_WAITALL) >= 12) {
            ok = strncmp(reply + 8, " 204", 4) == 0;    // "HTTP/1.1 204"
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

static void probe_task(void *arg)
{
    for (;;) {
        uint32_t wait_ms = PROBE_FAIL_MS;
        if (s_sta_has_ip) {
            bool ok = internet_probe();
            inet_state_t now = ok ? INET_OK : INET_DOWN;
            if (s_sta_has_ip && now != s_inet) {
                if (ok) {
                    ESP_LOGI(TAG, "Internet access OK");
                } else {
                    ESP_LOGW(TAG, "Wi-Fi is connected but there is no internet access");
                }
            }
            if (s_sta_has_ip) {
                s_inet = now;
            }
            wait_ms = ok ? PROBE_OK_MS : PROBE_FAIL_MS;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));   // woken early when a new IP arrives
    }
}

typedef enum {
    LED_STATE_PORTAL,                       // blue, slow blink
    LED_STATE_CONNECTING,                   // yellow, fast blink
    LED_STATE_CHECKING,                     // yellow
    LED_STATE_NO_INTERNET,                  // red
    LED_STATE_ONLINE_NO_PC,                 // cyan
    LED_STATE_ONLINE,                       // green
} led_state_t;

static led_state_t led_state(void)
{
    if (s_sta_has_ip && s_inet == INET_OK) {
        return s_usb_mounted ? LED_STATE_ONLINE : LED_STATE_ONLINE_NO_PC;
    }
    if (s_sta_has_ip && s_inet == INET_DOWN) {
        return LED_STATE_NO_INTERNET;
    }
    if (s_sta_connected) {
        return LED_STATE_CHECKING;
    }
    return state_snapshot().portal_active ? LED_STATE_PORTAL : LED_STATE_CONNECTING;
}

// Full-scale 0xRRGGBB colour for an RGB LED, or on (1) / off (0) for a single-colour LED
static uint32_t led_output(led_state_t state, uint32_t tick, bool rgb)
{
    bool slow = (tick / 6) % 2 == 0, fast = tick % 2 == 0;
    uint32_t phase = tick % 12;             // 1.2 s cycle for single-colour patterns
    switch (state) {
    case LED_STATE_PORTAL:          return rgb ? (slow ? 0x0000FF : 0) : phase == 0;
    case LED_STATE_CONNECTING:      return rgb ? (fast ? 0xFFAA00 : 0) : fast;
    case LED_STATE_CHECKING:        return rgb ? 0xFFAA00 : fast;
    case LED_STATE_NO_INTERNET:     return rgb ? 0xFF0000 : (phase == 0 || phase == 2);
    case LED_STATE_ONLINE_NO_PC:    return rgb ? 0x00FFFF : slow;
    case LED_STATE_ONLINE:          return rgb ? 0x00FF00 : 1;
    }
    return 0;
}

static void led_set(const settings_t *cfg, uint32_t value)
{
    if (cfg->led_type == LED_RGB) {
        uint32_t l = cfg->led_level ? cfg->led_level : 1;
        rgbLedWrite(cfg->led_pin, ((value >> 16) & 0xFF) * l / 255, ((value >> 8) & 0xFF) * l / 255,
                    (value & 0xFF) * l / 255);
    } else {
        bool on = value != 0;
        digitalWrite(cfg->led_pin, (cfg->led_type == LED_PLAIN_INVERTED) ? !on : on);
    }
}

static void led_task(void *arg)
{
    settings_t active = { .led_pin = -1 };
    uint32_t last = UINT32_MAX;
    for (uint32_t tick = 0;; tick++) {      // 100 ms per tick
        settings_t cfg = settings_snapshot();
        if (cfg.led_pin != active.led_pin || cfg.led_type != active.led_type || cfg.led_level != active.led_level) {
            if (active.led_pin >= 0) {      // release the previous LED
                led_set(&active, 0);
                pinMode(active.led_pin, INPUT);
            }
            if (cfg.led_pin >= 0 && cfg.led_type != LED_RGB) {
                pinMode(cfg.led_pin, OUTPUT);
            }
            active = cfg;
            last = UINT32_MAX;
        }
        if (active.led_pin >= 0) {
            uint32_t value = led_output(led_state(), tick, active.led_type == LED_RGB);
            if (value != last) {
                led_set(&active, value);
                last = value;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------------ web portal */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, portal_html);
}

static esp_err_t status_get(httpd_req_t *req)
{
    mgr_state_t st = state_snapshot();
    esp_netif_ip_info_t ip = { 0 };
    if (s_sta_has_ip) {
        esp_netif_get_ip_info(s_sta_netif, &ip);
    }
    wifi_ap_record_t ap_info = { 0 };
    if (s_sta_connected) {
        esp_wifi_sta_get_ap_info(&ap_info);
    }
    char buf[640] = "{\"ssid\":";
    json_str(buf, sizeof(buf), st.ssid);
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n,
             ",\"connected\":%s,\"connecting\":%s,\"saved\":%s,\"fails\":%d,\"reason\":\"%s\","
             "\"ip\":\"" IPSTR "\",\"rssi\":%d,\"internet\":\"%s\",\"pc\":%s,\"hotspot\":%s,\"ap\":\"%s\","
             "\"version\":\"" FW_VERSION "\",\"chip\":\"" CHIP_NAME "\","
             "\"uptime\":%lld,\"reset\":\"%s\",\"heap\":%u,\"heap_min\":%u,"
             "\"usb\":{\"tx_drops\":%lu,\"rx_drops\":%lu,\"recoveries\":%lu,\"rearms\":%lu,\"fifo_fixes\":%lu,\"resets\":%lu}}",
             s_sta_connected ? "true" : "false", st.connecting ? "true" : "false",
             st.has_creds ? "true" : "false", st.fail_count, reason_text(st.last_reason),
             IP2STR(&ip.ip), ap_info.rssi, inet_text(), s_usb_mounted ? "true" : "false",
             st.portal_active ? "true" : "false", s_ap_ssid,
             esp_timer_get_time() / 1000000, reset_text(esp_reset_reason()),
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned long)s_usb_tx_drops, (unsigned long)s_usb_rx_drops, (unsigned long)s_usb_in_recoveries,
             (unsigned long)s_usb_rearms, (unsigned long)s_usb_fifo_fixes, (unsigned long)s_usb_resets);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static int rssi_desc(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // A scan is refused while the station is busy connecting
    if (state_snapshot().connecting || xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return httpd_resp_sendstr(req, "{\"busy\":true,\"aps\":[]}");
    }
    uint16_t count = MAX_SCAN_RESULTS;
    wifi_ap_record_t *recs = calloc(count, sizeof(*recs));
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = recs ? esp_wifi_scan_start(&scan_cfg, true) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&count, recs);
    } else {
        esp_wifi_clear_ap_list();
    }
    xSemaphoreGive(s_wifi_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        free(recs);
        return httpd_resp_sendstr(req, "{\"busy\":true,\"aps\":[]}");
    }

    qsort(recs, count, sizeof(*recs), rssi_desc);
    httpd_resp_sendstr_chunk(req, "{\"busy\":false,\"aps\":[");
    bool first = true;
    for (int i = 0; i < count; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;   // same network seen from a stronger access point
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp(ssid, (const char *)recs[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        char item[256] = "";
        strcat(item, first ? "{\"ssid\":" : ",{\"ssid\":");
        json_str(item, sizeof(item), ssid);
        size_t n = strlen(item);
        snprintf(item + n, sizeof(item) - n, ",\"rssi\":%d,\"secure\":%s}",
                 recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    free(recs);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Reads a whole urlencoded request body into buf; replies 400 and returns false if it doesn't fit
static bool read_form(httpd_req_t *req, char *buf, size_t size)
{
    int len = req->content_len;
    if (len <= 0 || len >= (int)size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return false;
    }
    for (int got = 0; got < len;) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    buf[len] = '\0';
    return true;
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[512];
    if (!read_form(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    mgr_event_t ev = { .type = EV_NEW_CREDS };
    char raw[200];
    if (httpd_query_key_value(body, "ssid", raw, sizeof(raw)) != ESP_OK
            || !url_decode(raw, ev.ssid, sizeof(ev.ssid)) || ev.ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid network name");
    }
    if (httpd_query_key_value(body, "pass", raw, sizeof(raw)) == ESP_OK
            && !url_decode(raw, ev.pass, sizeof(ev.pass))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too long");
    }
    xQueueSend(s_events, &ev, portMAX_DELAY);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    mgr_event_t ev = { .type = EV_FORGET };
    xQueueSend(s_events, &ev, portMAX_DELAY);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Every unknown URL (phone connectivity checks) is redirected to the setup page on the
// address the client used to reach us (hotspot or USB)
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    char location[32] = "http://192.168.4.1/";
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &addr_len) == 0) {
        esp_ip4_addr_t local = { 0 };
        if (addr.ss_family == AF_INET) {
            local.addr = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
        } else if (addr.ss_family == AF_INET6) {
            local.addr = ((struct sockaddr_in6 *)&addr)->sin6_addr.un.u32_addr[3];  // IPv4-mapped
        }
        if (local.addr) {
            snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&local));
        }
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    // iOS needs a body to detect the captive portal
    return httpd_resp_sendstr(req, "Redirect to the setup page");
}

static esp_err_t settings_get(httpd_req_t *req)
{
    settings_t cfg = settings_snapshot();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"led_pin\":%d,\"led_type\":%u,\"led_level\":%u,\"ap_secured\":%s,\"default_led_pin\":%d}",
             cfg.led_pin, cfg.led_type, cfg.led_level, cfg.ap_pass[0] ? "true" : "false", DEFAULT_LED_PIN);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[512], raw[200];
    if (!read_form(req, body, sizeof(body))) {
        return ESP_FAIL;
    }
    settings_t cfg = settings_snapshot();
    if (httpd_query_key_value(body, "led_pin", raw, sizeof(raw)) == ESP_OK) {
        int pin = atoi(raw);
        if (!led_pin_valid(pin)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That GPIO can't drive an LED on this chip");
        }
        cfg.led_pin = pin;
    }
    if (httpd_query_key_value(body, "led_type", raw, sizeof(raw)) == ESP_OK) {
        int type = atoi(raw);
        if (type < LED_RGB || type > LED_PLAIN_INVERTED) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown LED type");
        }
        cfg.led_type = type;
    }
    if (httpd_query_key_value(body, "led_level", raw, sizeof(raw)) == ESP_OK) {
        int level = atoi(raw);
        cfg.led_level = level < 1 ? 1 : level > 255 ? 255 : level;
    }
    if (httpd_query_key_value(body, "ap_pass", raw, sizeof(raw)) == ESP_OK) {   // only sent when changed
        char pass[65];
        size_t pass_len = url_decode(raw, pass, sizeof(pass)) ? strlen(pass) : 99;
        if (pass_len != 0 && (pass_len < 8 || pass_len > 63)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Hotspot password must be 8-63 characters, or empty");
        }
        strlcpy(cfg.ap_pass, pass, sizeof(cfg.ap_pass));
    }
    if (settings_save(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
    }
    ESP_LOGI(TAG, "Settings saved: LED GPIO %d type %u level %u, hotspot %s", cfg.led_pin, cfg.led_type,
             cfg.led_level, cfg.ap_pass[0] ? "secured" : "open");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static esp_err_t restart_post(httpd_req_t *req)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    if (!timer && esp_timer_create(&args, &timer) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Restart failed");
    }
    ESP_LOGW(TAG, "Restart requested from the setup page");
    esp_timer_start_once(timer, 500 * 1000);    // let the reply go out first
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "Failed to start web server");
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/api/forget",  .method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);
    return ESP_OK;
}

static void portal_start(void)
{
    if (s_st.portal_active) {
        return;
    }
    ESP_LOGW(TAG, "Setup hotspot on: join Wi-Fi \"%s\" and open http://192.168.4.1 "
             "(or open http://192.168.7.1 on the PC)", s_ap_ssid);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    settings_t cfg = settings_snapshot();
    if (strlen(cfg.ap_pass) >= 8) {
        strlcpy((char *)ap_cfg.ap.password, cfg.ap_pass, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    xSemaphoreGive(s_wifi_lock);
    s_st.portal_active = true;
}

static void portal_stop(void)
{
    if (!s_st.portal_active) {
        return;
    }
    ESP_LOGI(TAG, "Setup hotspot off (setup page stays at http://192.168.7.1 on the PC)");
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    xSemaphoreGive(s_wifi_lock);
    s_st.portal_active = false;
}

/* ------------------------------------------------------------------ connection manager */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    mgr_event_t ev = { 0 };
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED:
        s_sta_connected = true;
        ev.type = EV_STA_CONNECTED;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_connected = false;
        s_sta_has_ip = false;
        s_inet = INET_UNKNOWN;
        ev.type = EV_STA_DISCONNECTED;
        ev.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Device joined the setup hotspot");
        return;
    default:
        return;
    }
    xQueueSend(s_events, &ev, 0);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Got IP " IPSTR " from the Wi-Fi router, checking internet...", IP2STR(&event->ip_info.ip));
        s_inet = INET_UNKNOWN;
        s_sta_has_ip = true;
        xTaskNotifyGive(s_probe_task);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        s_inet = INET_UNKNOWN;
    } else if (id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = data;
        ESP_LOGI(TAG, "%s got IP " IPSTR, event->esp_netif == s_usb_netif ? "PC (USB)" : "Hotspot device",
                 IP2STR(&event->ip));
    }
}

// Call with s_wifi_lock held
static void sta_connect(void)
{
    s_st.connecting = true;
    s_connect_started = xTaskGetTickCount();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void apply_credentials(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "New network selected: \"%s\"", ssid);
    wifi_config_t cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);     // waits for a running scan
    if (s_sta_connected) {
        s_expect_leave = true;
        esp_wifi_disconnect();
    }
    // Stored in flash by the Wi-Fi driver, so it survives reboots
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    state_set_ssid(ssid);
    s_st.fail_count = 0;
    s_st.last_reason = 0;
    sta_connect();
    xSemaphoreGive(s_wifi_lock);
}

static void forget_credentials(void)
{
    ESP_LOGI(TAG, "Forgetting saved network");
    wifi_config_t cfg = { 0 };
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (s_sta_connected) {
        s_expect_leave = true;
    }
    esp_wifi_disconnect();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    xSemaphoreGive(s_wifi_lock);
    state_set_ssid("");
    s_st.connecting = false;
    s_st.fail_count = 0;
    s_st.last_reason = 0;
    portal_start();
}

static void manager_task(void *arg)
{
    bool retry_pending = s_st.has_creds;
    TickType_t next_retry = xTaskGetTickCount();
    TickType_t portal_close_at = 0;

    if (!s_st.has_creds) {
        ESP_LOGW(TAG, "No saved Wi-Fi network");
        portal_start();
    }

    for (;;) {
        mgr_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(250)) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            switch (ev.type) {
            case EV_STA_CONNECTED:
                ESP_LOGI(TAG, "Wi-Fi connected to \"%s\"", s_st.ssid);
                s_st.connecting = false;
                s_st.fail_count = 0;
                s_st.last_reason = 0;
                retry_pending = false;
                if (s_st.portal_active) {
                    portal_close_at = now + pdMS_TO_TICKS(PORTAL_LINGER_MS);
                }
                break;

            case EV_STA_DISCONNECTED:
                if (s_expect_leave) {
                    s_expect_leave = false;
                    break;
                }
                s_st.connecting = false;
                s_st.last_reason = ev.reason;
                s_st.fail_count++;
                portal_close_at = 0;
                ESP_LOGW(TAG, "Wi-Fi \"%s\": %s (reason %d), failure %d",
                         s_st.ssid, reason_text(ev.reason), ev.reason, s_st.fail_count);
                if (!s_st.portal_active && s_st.fail_count >= MAX_STA_FAILS) {
                    portal_start();
                }
                if (s_st.has_creds) {
                    retry_pending = true;
                    next_retry = now + pdMS_TO_TICKS(s_st.portal_active ? PORTAL_RETRY_MS : RETRY_DELAY_MS);
                }
                break;

            case EV_NEW_CREDS:
                apply_credentials(ev.ssid, ev.pass);
                retry_pending = false;
                break;

            case EV_FORGET:
                forget_credentials();
                retry_pending = false;
                break;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (s_st.connecting && now - s_connect_started > pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Connection attempt timed out");
            s_st.connecting = false;
            retry_pending = s_st.has_creds;
            next_retry = now;
        }
        if (retry_pending && !s_st.connecting && !s_sta_connected && (int32_t)(now - next_retry) >= 0
                && xSemaphoreTake(s_wifi_lock, 0) == pdTRUE) {   // skip while a scan is running
            retry_pending = false;
            ESP_LOGI(TAG, "Connecting to \"%s\"...", s_st.ssid);
            sta_connect();
            xSemaphoreGive(s_wifi_lock);
        }
        if (portal_close_at && s_sta_connected && (int32_t)(now - portal_close_at) >= 0) {
            portal_close_at = 0;
            portal_stop();
        }
        usb_poll();
    }
}

/* ------------------------------------------------------------------ startup */

static esp_err_t wifi_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL),
                        TAG, "ip event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");

    wifi_config_t saved = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &saved);
    state_set_ssid((const char *)saved.sta.ssid);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), HOSTNAME_PREFIX "%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(s_sta_netif, hostname);  // name shown in the router's client list

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    esp_wifi_set_ps(WIFI_PS_NONE);          // lowest latency / highest throughput
    return ESP_OK;
}

esp_err_t usb_wifi_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init");
    settings_load();
    ESP_LOGI(TAG, FW_NAME " v" FW_VERSION " (" CHIP_NAME ")");

    // Connectivity checks from phones produce lots of harmless request errors
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    s_events = xQueueCreate(8, sizeof(mgr_event_t));
    s_wifi_lock = xSemaphoreCreateMutex();
    xTaskCreate(led_task, "status_led", 3072, NULL, 3, NULL);
    xTaskCreate(probe_task, "inet_probe", 4096, NULL, 4, &s_probe_task);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {    // may already exist
        return ret;
    }
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(usb_netif_init(), TAG, "USB network init failed");
    const esp_timer_create_args_t watchdog_args = { .callback = usb_fifo_watchdog, .name = "usb_fifo_wd" };
    esp_timer_handle_t watchdog;
    ESP_RETURN_ON_ERROR(esp_timer_create(&watchdog_args, &watchdog), TAG, "USB watchdog");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(watchdog, USB_FIFO_WATCHDOG_US), TAG, "USB watchdog");
    ESP_RETURN_ON_ERROR(start_webserver(), TAG, "web server");
    dns_proxy_start(is_online);

    ESP_LOGI(TAG, "USB network card MAC %s, PC side network 192.168.7.0/24", s_usb_mac_str);
    if (s_st.has_creds) {
        ESP_LOGI(TAG, "Saved network: \"%s\"", s_st.ssid);
    }
    xTaskCreate(manager_task, "wifi_mgr", 6144, NULL, 5, NULL);
    return ESP_OK;
}
