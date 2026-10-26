#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true while queries should be relayed upstream
typedef bool (*dns_proxy_online_fn)(void);

// Starts the DNS server on port 53 of every interface
void dns_proxy_start(dns_proxy_online_fn online);

#ifdef __cplusplus
}
#endif
