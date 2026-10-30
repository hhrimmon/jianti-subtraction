/* JianTi v0.3 provisioning v2: captive portal + WiFi scan, one flow.
 * Flow (product): phone joins the SoftAP -> a setup page pops up by itself
 * -> the page lists scanned home WiFis -> pick one, enter password, save ->
 * device restarts into STA mode. No manual IP typing; 192.168.4.1 still
 * works as a fallback if a phone refuses to pop the portal.
 *
 * Mechanism (same three pieces as tonyp7/esp32-wifi-manager, rewritten for
 * this project's uplink/NVS layout instead of vendoring that component,
 * which brings its own WiFi manager and would fight uplink.c):
 *   1. DNS wildcard (dns_server.c): every DNS query from the phone answers
 *      192.168.4.1, so the phone's captive-portal probes land on us.
 *   2. Probe paths (/generate_204, /hotspot-detect.html, /connecttest.txt,
 *      /ncsi.txt, /redirect) answer 302 to "/", which makes iOS/Android/
 *      Windows show the portal popup.
 *   3. WiFi scan (GET /scan, blocking) lists nearby SSIDs; the page renders
 *      them as a pick list plus a manual-SSID fallback for hidden networks.
 * Setup mode runs WiFi in APSTA (AP for the portal + STA idle for scanning);
 * a scan in pure AP mode is not reliable, hence APSTA. After save+restart
 * the device boots into normal STA mode (uplink.c). The AP's DHCP server
 * hands the phone DNS 192.168.4.1 by default, which is what makes piece 1
 * reachable; if a phone still refuses to pop the portal, the manual IP works.
 */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "prov.h"
#include "dns_server.h"

static const char *TAG = "jt_prov";

#define JT_NVS_NS "jt"
#define JT_NVS_KEY_SSID "ssid"
#define JT_NVS_KEY_PASS "pass"

static bool s_decided = false;
static jt_prov_mode_t s_mode = JT_PROV_MODE_SETUP;
static bool s_has_saved = false;
static char s_sta_ssid[33] = {0};
static char s_sta_pass[65] = {0};

static bool s_setup_started = false;
static httpd_handle_t s_httpd = NULL;
static char s_ap_ssid[33] = {0};
static char s_ap_pass[16] = {0};

jt_prov_mode_t jt_prov_decide(void)
{
    if (s_decided) return s_mode;
    s_decided = true;

    nvs_handle_t h = 0;
    if (nvs_open(JT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t ssid_len = sizeof(s_sta_ssid);
        size_t pass_len = sizeof(s_sta_pass);
        esp_err_t e1 = nvs_get_str(h, JT_NVS_KEY_SSID, s_sta_ssid, &ssid_len);
        esp_err_t e2 = nvs_get_str(h, JT_NVS_KEY_PASS, s_sta_pass, &pass_len);
        nvs_close(h);
        if (e1 == ESP_OK && s_sta_ssid[0] != '\0') {
            if (e2 != ESP_OK) s_sta_pass[0] = '\0'; /* open network */
            s_has_saved = true;
            s_mode = JT_PROV_MODE_STA;
            ESP_LOGI(TAG, "NVS WiFi found, STA mode (ssid=%s)", s_sta_ssid);
            return s_mode;
        }
        s_sta_ssid[0] = '\0';
        s_sta_pass[0] = '\0';
    }

    /* Dev fallback: Kconfig creds filled locally at build time. */
    if (CONFIG_JT_WIFI_SSID[0] != '\0') {
        strncpy(s_sta_ssid, CONFIG_JT_WIFI_SSID, sizeof(s_sta_ssid) - 1);
        strncpy(s_sta_pass, CONFIG_JT_WIFI_PASS, sizeof(s_sta_pass) - 1);
        s_mode = JT_PROV_MODE_STA;
        ESP_LOGI(TAG, "no NVS WiFi, Kconfig fallback STA mode (ssid=%s)", s_sta_ssid);
        return s_mode;
    }

    s_mode = JT_PROV_MODE_SETUP;
    ESP_LOGI(TAG, "no WiFi creds anywhere, SETUP mode");
    return s_mode;
}

bool jt_prov_is_setup_mode(void)
{
    return s_decided && s_mode == JT_PROV_MODE_SETUP;
}

bool jt_prov_get_sta_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (!s_decided || s_mode != JT_PROV_MODE_STA) return false;
    if (ssid && ssid_len > 0) strncpy(ssid, s_sta_ssid, ssid_len - 1);
    if (pass && pass_len > 0) strncpy(pass, s_sta_pass, pass_len - 1);
    return true;
}

bool jt_prov_has_saved_creds(void)
{
    return s_has_saved;
}

void jt_prov_ap_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len,
                     char *ip, size_t ip_len)
{
    if (ssid && ssid_len > 0) strncpy(ssid, s_ap_ssid, ssid_len - 1);
    if (pass && pass_len > 0) strncpy(pass, s_ap_pass, pass_len - 1);
    if (ip && ip_len > 0) strncpy(ip, "192.168.4.1", ip_len - 1);
}

esp_err_t jt_prov_erase(void)
{
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(JT_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, JT_NVS_KEY_SSID);
    nvs_erase_key(h, JT_NVS_KEY_PASS);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ---------------- form parsing ---------------- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place URL-decode: %XX and '+' -> space. */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && hex_val(r[1]) >= 0 && hex_val(r[2]) >= 0) {
            *w++ = (char)(hex_val(r[1]) * 16 + hex_val(r[2]));
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Split "ssid=..&ssid2=..&pass=.." into values (decoded). ssid2 is the
 * manual-SSID field and wins when non-empty; ssid is the picked list value.
 * Returns false when no SSID survives. */
static bool parse_form(char *body, char *ssid, size_t ssid_len,
                       char *pass, size_t pass_len)
{
    char manual[33] = {0};
    ssid[0] = '\0';
    pass[0] = '\0';
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = '\0';
        url_decode(pair);
        url_decode(eq + 1);
        if (strcmp(pair, "ssid") == 0) strncpy(ssid, eq + 1, ssid_len - 1);
        if (strcmp(pair, "ssid2") == 0) strncpy(manual, eq + 1, sizeof(manual) - 1);
        if (strcmp(pair, "pass") == 0) strncpy(pass, eq + 1, pass_len - 1);
    }
    if (manual[0] != '\0') {
        size_t n = strnlen(manual, ssid_len - 1);
        memcpy(ssid, manual, n);
        ssid[n] = '\0';
    }
    return ssid[0] != '\0';
}

/* ---------------- portal page ---------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>简题配网</title>"
        "<style>body{font-family:-apple-system,sans-serif;margin:20px;color:#222}"
        "h2{font-size:22px}"
        "#list button{display:block;width:100%;padding:14px;margin:8px 0;font-size:17px;text-align:left;border:1px solid #ddd;border-radius:10px;background:#f8f8f8}"
        "button{padding:12px 20px;font-size:16px;border-radius:10px;border:1px solid #ddd;background:#fff}"
        "input[type=submit]{background:#07c160;color:#fff;border:none;padding:12px 28px;font-size:17px;border-radius:10px}"
        "input[name=ssid2],input[name=pass]{padding:10px;font-size:16px;width:85%;border:1px solid #ccc;border-radius:8px}</style></head><body>"
        "<h2>简题配网</h2>"
        "<p>点家里 WiFi，输密码，保存后设备自动重启连接。</p>"
        "<div id=\"list\">正在扫描 WiFi……</div>"
        "<button type=\"button\" onclick=\"load()\">重新扫描</button>"
        "<form method=\"POST\" action=\"/save\">"
        "<p>已选：<span id=\"picked\">（点上面列表选一个，或下面手动填）</span></p>"
        "<input type=\"hidden\" name=\"ssid\" id=\"ssid\">"
        "手动填写：<input name=\"ssid2\" id=\"ssid2\" maxlength=\"32\"><br><br>"
        "WiFi 密码：<input type=\"password\" name=\"pass\" maxlength=\"63\"><br><br>"
        "<input type=\"submit\" value=\"保存并连接\">"
        "</form>"
        "<script>"
        "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
        "function load(){fetch('/scan').then(function(r){return r.json();}).then(function(a){"
        "var h='';"
        "if(!a.length){h='没扫到，点重新扫描';}"
        "else{a.forEach(function(ap){"
        "h+='<button type=\"button\" onclick=\"pick(\\''+ap.ssid.replace(/'/g,'')+'\\')\">'+esc(ap.ssid)+(ap.open?'（开放）':'')+' '+ap.rssi+'dBm</button><br>';});}"
        "document.getElementById('list').innerHTML=h;"
        "}).catch(function(){document.getElementById('list').innerHTML='扫描失败，点重新扫描';});}"
        "function pick(s){document.getElementById('ssid').value=s;document.getElementById('picked').textContent=s;}"
        "load();"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, strlen(page));
}

/* ---------------- captive-portal probes ---------------- */

/* iOS / Android / Windows probe paths all land here: redirect to the portal
 * page, which is what makes the phone show the "sign in" popup. */
static esp_err_t probe_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------- WiFi scan ---------------- */

/* Static scan buffers: a blocking scan plus JSON building would otherwise
 * eat the httpd worker task's stack. One phone provisions at a time, so
 * sharing these across requests is fine. */
static wifi_ap_record_t s_scan_recs[20];
static char s_scan_json[2048];

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (o + 6 < dst_len) {
                o += (size_t)snprintf(dst + o, dst_len - o, "\\u%04x", c);
            }
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    uint16_t n = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t total = 0;
        if (esp_wifi_scan_get_ap_num(&total) == ESP_OK) {
            n = total > 20 ? 20 : total;
            if (n > 0 && esp_wifi_scan_get_ap_records(&n, s_scan_recs) != ESP_OK) n = 0;
        }
    }
    /* Dedupe by SSID (keep strongest), then sort by RSSI descending. */
    for (uint16_t i = 0; i < n; i++) {
        for (uint16_t j = i + 1; j < n;) {
            if (strcmp((const char *)s_scan_recs[i].ssid,
                       (const char *)s_scan_recs[j].ssid) == 0) {
                if (s_scan_recs[j].rssi > s_scan_recs[i].rssi) {
                    s_scan_recs[i] = s_scan_recs[j];
                }
                memmove(&s_scan_recs[j], &s_scan_recs[j + 1],
                        (n - j - 1) * sizeof(wifi_ap_record_t));
                n--;
            } else {
                j++;
            }
        }
    }
    for (uint16_t i = 0; i < n; i++) {
        for (uint16_t j = i + 1; j < n; j++) {
            if (s_scan_recs[j].rssi > s_scan_recs[i].rssi) {
                wifi_ap_record_t tmp = s_scan_recs[i];
                s_scan_recs[i] = s_scan_recs[j];
                s_scan_recs[j] = tmp;
            }
        }
    }
    size_t o = 0;
    o += (size_t)snprintf(s_scan_json + o, sizeof(s_scan_json) - o, "[");
    for (uint16_t i = 0; i < n; i++) {
        char ssid[33] = {0};
        memcpy(ssid, s_scan_recs[i].ssid, 32);
        char esc[160] = {0};
        json_escape(ssid, esc, sizeof(esc));
        o += (size_t)snprintf(s_scan_json + o, sizeof(s_scan_json) - o,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%d}",
                              i ? "," : "", esc, (int)s_scan_recs[i].rssi,
                              s_scan_recs[i].authmode == WIFI_AUTH_OPEN ? 1 : 0);
        if (o >= sizeof(s_scan_json) - 64) break;
    }
    snprintf(s_scan_json + o, sizeof(s_scan_json) - o, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s_scan_json, strlen(s_scan_json));
}

/* ---------------- save + status ---------------- */

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1023) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form size");
        return ESP_FAIL;
    }
    char body[1024];
    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)req->content_len - got);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    body[got] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!parse_form(body, ssid, sizeof(ssid), pass, sizeof(pass)) ||
        strlen(ssid) > 32 || strlen(pass) > 63) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(JT_NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, JT_NVS_KEY_SSID, ssid);
        if (e == ESP_OK) e = nvs_set_str(h, JT_NVS_KEY_PASS, pass);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(e));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WiFi saved (ssid=%s), restarting into STA mode", ssid);

    static const char ok_page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>简题配网</title></head><body>"
        "<h2>已保存，设备正在重启并连接该 WiFi……</h2>"
        "<p>稍等片刻，看设备屏幕是否回到预览画面。</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok_page, strlen(ok_page));

    /* Give the stack a moment to flush the reply, then restart cleanly. */
    static esp_timer_handle_t restart_timer = NULL;
    if (!restart_timer) {
        esp_timer_create_args_t args = {
            .callback = restart_cb,
            .name = "jt_prov_restart",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &restart_timer));
    }
    ESP_ERROR_CHECK(esp_timer_start_once(restart_timer, 1500000));
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "mode=setup\nap_ssid=%s\nsaved_ssid=%s\n",
             s_ap_ssid, s_has_saved ? s_sta_ssid : "(none)");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, buf, strlen(buf));
}

/* ---------------- setup start ---------------- */

esp_err_t jt_prov_start_setup(void)
{
    if (s_setup_started) return ESP_ERR_INVALID_STATE;
    s_setup_started = true;

    /* Product rule: the setup hotspot is OPEN (no password). It is transient
     * -- it disappears once provisioning finishes -- so a password on it is
     * pure friction. Stable hotspot name from the WiFi MAC. */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        mac[4] = (uint8_t)(esp_random() & 0xFF);
        mac[5] = (uint8_t)(esp_random() & 0xFF);
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "JianTi-%02X%02X", mac[4], mac[5]);
    s_ap_pass[0] = '\0'; /* open hotspot: no password by product rule */

    /* APSTA: AP serves the portal, STA stays idle so /scan can scan. */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!ap_netif || !sta_netif) {
        ESP_LOGE(TAG, "AP/STA netif create failed");
        return ESP_FAIL;
    }
    wifi_config_t apcfg = {0};
    size_t ap_ssid_n = strnlen(s_ap_ssid, sizeof(apcfg.ap.ssid) - 1);
    memcpy(apcfg.ap.ssid, s_ap_ssid, ap_ssid_n);
    apcfg.ap.ssid_len = ap_ssid_n;
    apcfg.ap.channel = 6;
    apcfg.ap.authmode = WIFI_AUTH_OPEN; /* open hotspot, see product rule above */
    apcfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up (open, APSTA, scan enabled): ssid=%s IP 192.168.4.1", s_ap_ssid);

    ESP_ERROR_CHECK(dns_server_start());

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 12;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ESP_FAIL;
    }
    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
    };
    static const httpd_uri_t scan_uri = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_handler,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t probe_204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = probe_handler,
    };
    static const httpd_uri_t probe_apple = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = probe_handler,
    };
    static const httpd_uri_t probe_ms = {
        .uri = "/connecttest.txt", .method = HTTP_GET, .handler = probe_handler,
    };
    static const httpd_uri_t probe_ncsi = {
        .uri = "/ncsi.txt", .method = HTTP_GET, .handler = probe_handler,
    };
    static const httpd_uri_t probe_redirect = {
        .uri = "/redirect", .method = HTTP_GET, .handler = probe_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &save_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &probe_204));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &probe_apple));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &probe_ms));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &probe_ncsi));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &probe_redirect));
    ESP_LOGI(TAG, "captive portal on http://192.168.4.1/ (popup + scan)");
    return ESP_OK;
}
