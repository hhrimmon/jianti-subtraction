#pragma once
/* Tiny wildcard DNS responder for the provisioning captive portal.
 * Every DNS query from a phone on the SoftAP answers 192.168.4.1, so the
 * phone's captive-portal probes (iOS/Android/Windows) land on our HTTP
 * server instead of timing out. One UDP task, no stop needed: the device
 * restarts out of setup mode once WiFi is saved.
 */
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dns_server_start(void);

#ifdef __cplusplus
}
#endif
