/*
 * Small DNS server for the USB network and the setup hotspot.
 *
 * - While the ESP has an upstream connection, queries are relayed to the upstream DNS server.
 * - Otherwise, A queries from the setup hotspot are answered with the hotspot's own IP (captive
 *   portal), and everything else gets SERVFAIL so the PC quickly falls back to its other adapters.
 */

#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "lwip/sockets.h"

#include "dns_proxy.h"

#define DNS_PORT        53
#define FWD_SLOTS       64      // outstanding relayed queries
#define MAX_PKT         1500
#define CAPTIVE_TTL     10

typedef struct {
    bool used;
    uint16_t relay_id;
    uint16_t client_id;
    struct sockaddr_in client;
} fwd_slot_t;

static const char *TAG = "dns_proxy";
static dns_proxy_online_fn s_online;

static int udp_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static uint32_t upstream_dns(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns;
    if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK
            && dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr) {
        return dns.ip.u_addr.ip4.addr;
    }
    return ESP_IP4TOADDR(8, 8, 8, 8);
}

// The hotspot IP if the client is on the setup hotspot, otherwise 0
static uint32_t captive_ip_for(uint32_t client)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && ((client ^ info.ip.addr) & info.netmask.addr) == 0) {
        return info.ip.addr;
    }
    return 0;
}

// Length of header + question section, or 0 if the query is malformed
static int question_end(const uint8_t *pkt, int len)
{
    if (len < 12 || pkt[4] != 0 || pkt[5] != 1) {     // exactly one question
        return 0;
    }
    int i = 12;
    while (i < len && pkt[i] != 0) {
        if (pkt[i] & 0xC0) {                        // no compression in questions
            return 0;
        }
        i += pkt[i] + 1;
    }
    i += 1 + 4;                                     // terminating zero, QTYPE, QCLASS
    return i <= len ? i : 0;
}

// Turns the query in pkt into a local reply: A record with captive_ip, or SERVFAIL if it is 0
static int build_local_reply(uint8_t *pkt, int len, uint32_t captive_ip)
{
    int qend = question_end(pkt, len);
    if (!qend) {
        return 0;
    }
    uint16_t qtype = (pkt[qend - 4] << 8) | pkt[qend - 3];
    pkt[2] = 0x84 | (pkt[2] & 0x01);                // response, authoritative, keep RD
    pkt[3] = 0x80;                                  // recursion available, NOERROR
    memset(pkt + 6, 0, 6);                          // no answer/authority/additional records
    if (!captive_ip) {
        pkt[3] |= 2;                                // SERVFAIL
        return qend;
    }
    if (qtype != 1) {                               // not an A query: empty answer
        return qend;
    }
    static const uint8_t answer_head[] = {
        0xC0, 0x0C,                                 // name: pointer to the question
        0x00, 0x01, 0x00, 0x01,                     // type A, class IN
        0x00, 0x00, 0x00, CAPTIVE_TTL,
        0x00, 0x04,
    };
    memcpy(pkt + qend, answer_head, sizeof(answer_head));
    memcpy(pkt + qend + sizeof(answer_head), &captive_ip, 4);
    pkt[7] = 1;                                     // one answer
    return qend + sizeof(answer_head) + 4;
}

static void dns_task(void *arg)
{
    int srv = udp_socket(DNS_PORT);
    int up = udp_socket(0);
    if (srv < 0 || up < 0) {
        ESP_LOGE(TAG, "Cannot open DNS sockets");
        vTaskDelete(NULL);
    }
    static fwd_slot_t slots[FWD_SLOTS];
    static uint8_t pkt[MAX_PKT];
    uint16_t next_id = esp_random();

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        FD_SET(up, &fds);
        if (select(MAX(srv, up) + 1, &fds, NULL, NULL, NULL) <= 0) {
            continue;
        }

        if (FD_ISSET(srv, &fds)) {
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            int len = recvfrom(srv, pkt, sizeof(pkt), 0, (struct sockaddr *)&client, &client_len);
            if (len >= 12 && s_online()) {
                fwd_slot_t *slot = &slots[next_id % FWD_SLOTS];
                slot->used = true;
                slot->relay_id = next_id;
                slot->client_id = (pkt[0] << 8) | pkt[1];
                slot->client = client;
                pkt[0] = next_id >> 8;
                pkt[1] = next_id & 0xFF;
                next_id++;
                struct sockaddr_in dst = {
                    .sin_family = AF_INET,
                    .sin_port = htons(DNS_PORT),
                    .sin_addr.s_addr = upstream_dns(),
                };
                sendto(up, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
            } else if (len >= 12) {
                int n = build_local_reply(pkt, len, captive_ip_for(client.sin_addr.s_addr));
                if (n) {
                    sendto(srv, pkt, n, 0, (struct sockaddr *)&client, client_len);
                }
            }
        }

        if (FD_ISSET(up, &fds)) {
            int len = recvfrom(up, pkt, sizeof(pkt), 0, NULL, NULL);
            if (len >= 12) {
                uint16_t id = (pkt[0] << 8) | pkt[1];
                fwd_slot_t *slot = &slots[id % FWD_SLOTS];
                if (slot->used && slot->relay_id == id) {
                    pkt[0] = slot->client_id >> 8;
                    pkt[1] = slot->client_id & 0xFF;
                    sendto(srv, pkt, len, 0, (struct sockaddr *)&slot->client, sizeof(slot->client));
                    slot->used = false;
                }
            }
        }
    }
}

void dns_proxy_start(dns_proxy_online_fn online)
{
    s_online = online;
    xTaskCreate(dns_task, "dns_proxy", 4096, NULL, 5, NULL);
}
