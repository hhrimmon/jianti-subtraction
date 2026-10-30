#pragma once
/* JianTi v0.3 provisioning (prov.c).
 *
 * Scope: get the device onto the home WiFi. Account binding is a separate,
 * later slice (device shows a QR once online, parent scans in the mini-program).
 *
 * Boot flow (call order matters):
 *   1. jt_uplink_init() runs first: it inits NVS, netif, the WiFi driver and
 *      the event loop, then calls jt_prov_decide().
 *   2. jt_prov_decide() reads NVS namespace "jt", keys "ssid"/"pass":
 *      - saved creds found  -> STA mode, uplink connects with them.
 *      - no saved creds, but Kconfig JT_WIFI_SSID is filled (dev fallback)
 *        -> STA mode with the Kconfig creds, so the existing dev workflow
 *        keeps working without a provisioning round.
 *      - neither -> SETUP mode: uplink skips STA; main.c then calls
 *        jt_prov_start_setup() (captive portal + WiFi scan) and shows the
 *        setup screen.
 *   3. In SETUP mode the parent joins the on-screen hotspot and a setup page
 *      pops up by itself (DNS wildcard + probe-path redirects); the page
 *      lists scanned home WiFis, the parent picks one, enters the password,
 *      and the device saves it to NVS and restarts into STA mode.
 *      192.168.4.1 still works as a manual fallback if a phone refuses to
 *      pop the portal.
 *
 * The setup hotspot itself is OPEN (no password): it is transient and
 * disappears once provisioning finishes, so a password on it would only
 * add friction. The saved home WiFi password is never printed (wifistat
 * shows the SSID only).
 *
 * Known gap (next rev, not this slice): if STA creds turn out wrong, the
 * device retries STA forever; recovery today is the serial "prov" command
 * (erase + restart into SETUP mode). No auto-fallback to SETUP yet.
 */
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JT_PROV_MODE_STA,   /* connect with saved (or Kconfig fallback) creds */
    JT_PROV_MODE_SETUP, /* no creds: captive portal + WiFi scan */
} jt_prov_mode_t;

/* Decide the boot mode from NVS (+ Kconfig fallback). Called by uplink init
 * after NVS is up. Safe to call repeatedly; the decision is cached. */
jt_prov_mode_t jt_prov_decide(void);

/* True once decide() returned SETUP. main.c uses this for the setup screen
 * and to guard the "up" command (no upload without home WiFi). */
bool jt_prov_is_setup_mode(void);

/* Copy the STA creds chosen by decide() (NVS first, Kconfig fallback).
 * Returns false if decide() has not run or mode is SETUP. */
bool jt_prov_get_sta_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* Start SoftAP + captive portal + scan for SETUP mode. Requires uplink init
 * to have run first (netif + WiFi driver + event loop). Fills the AP info
 * used by the setup screen. Returns ESP_OK when the AP, DNS and HTTP
 * server are up. */
esp_err_t jt_prov_start_setup(void);

/* Copy the SoftAP info (valid after a successful jt_prov_start_setup()).
 * ip is always the default AP address "192.168.4.1". */
void jt_prov_ap_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len,
                     char *ip, size_t ip_len);

/* Erase the saved home WiFi creds from NVS (serial "prov" command). */
esp_err_t jt_prov_erase(void);

/* True when NVS holds saved creds (wifistat display only; never the password). */
bool jt_prov_has_saved_creds(void);

#ifdef __cplusplus
}
#endif
