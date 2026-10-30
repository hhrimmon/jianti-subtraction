/* JianTi v0.2 uplink: WiFi station (P4 + C6 via esp_hosted) + HTTP POST gray JPEG
 * to the PC gateway, then show the result on screen (via main.c hooks).
 *
 * v0.3 provisioning (prov.c): the STA creds now come from NVS (saved by the
 * SoftAP web form), with the Kconfig JT_WIFI_SSID/PASS kept only as a dev
 * fallback when NVS is empty. The gateway URL is still Kconfig.
 * WiFi pattern follows Waveshare example 04_wifistation (esp_wifi_remote
 * 0.14.* + esp_hosted 1.4.* for IDF < 6.0); no explicit esp_hosted init call.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "uplink.h"
#include "prov.h"

static const char *TAG = "jt_uplink";

#define RESP_CAP 8192

static volatile bool s_wifi_connected = false;
static SemaphoreHandle_t s_upload_sem = NULL;
static SemaphoreHandle_t s_jpeg_mutex = NULL;
static uint8_t *s_jpeg = NULL;
static size_t s_jpeg_len = 0;
static size_t s_jpeg_cap = 0;
static char s_resp[RESP_CAP];
static size_t s_resp_len = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "wifi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        (void)data;
        s_wifi_connected = true;
        ESP_LOGI(TAG, "wifi got ip");
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t room = RESP_CAP - 1 - s_resp_len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(s_resp + s_resp_len, evt->data, n);
        s_resp_len += n;
        s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

/* Extract the "text" field from {"ok":true,"text":"..."} with \" \\ \n unescaping.
 * The gateway sends raw UTF-8 (ensure_ascii=False), so no \u decoding is needed. */
static bool parse_text_field(const char *json, char *out, size_t out_sz)
{
    const char *p = strstr(json, "\"text\"");
    if (!p) return false;
    p = strchr(p + 6, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

static bool parse_int_field(const char *json, const char *key, int *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    return true;
}

static void upload_task(void *arg)
{
    (void)arg;
    char textbuf[2048];
    while (true) {
        if (xSemaphoreTake(s_upload_sem, portMAX_DELAY) != pdTRUE) continue;
        if (s_jpeg_len == 0) {
            jt_show_result(false, "no jpeg yet: type cap first", 0, 0, 0);
            continue;
        }
        int waited = 0;
        while (!s_wifi_connected && waited < 30000) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
        }
        if (!s_wifi_connected) {
            ESP_LOGE(TAG, "upload: wifi not connected");
            jt_show_result(false, "wifi not connected", 0, 0, 0);
            continue;
        }
        uint8_t *jpeg_copy = NULL;
        size_t jpeg_len = 0;
        if (xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            jpeg_len = s_jpeg_len;
            jpeg_copy = heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
            if (jpeg_copy) memcpy(jpeg_copy, s_jpeg, jpeg_len);
            xSemaphoreGive(s_jpeg_mutex);
        }
        if (!jpeg_copy) {
            jt_show_result(false, "jpeg buffer busy", 0, 0, 0);
            continue;
        }
        s_resp_len = 0;
        s_resp[0] = '\0';
        esp_http_client_config_t cfg = {
            .url = CONFIG_JT_GATEWAY_URL,
            .method = HTTP_METHOD_POST,
            /* 180s: local Ollama inference is slow; 20s would fail before the
             * first answer comes back. Cloud calls usually return in seconds. */
            .timeout_ms = 180000,
            .event_handler = http_event_handler,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            heap_caps_free(jpeg_copy);
            jt_show_result(false, "http client init failed", 0, 0, 0);
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_http_client_set_post_field(client, (const char *)jpeg_copy, (int)jpeg_len);
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(client);
        int latency_ms = (int)((esp_timer_get_time() - t0) / 1000);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        heap_caps_free(jpeg_copy);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "upload perform failed: %s", esp_err_to_name(err));
            jt_show_result(false, "upload failed", latency_ms, 0, 0);
            continue;
        }
        if (status != 200) {
            ESP_LOGE(TAG, "upload http status %d", status);
            jt_show_result(false, "http status != 200", latency_ms, 0, 0);
            continue;
        }
        if (!parse_text_field(s_resp, textbuf, sizeof(textbuf))) {
            ESP_LOGE(TAG, "upload: bad json: %.200s", s_resp);
            jt_show_result(false, "bad gateway reply", latency_ms, 0, 0);
            continue;
        }
        ESP_LOGI(TAG, "upload ok: %d ms, answer %u bytes", latency_ms, (unsigned)strlen(textbuf));
        int up_ms = 0, an_ms = 0;
        parse_int_field(s_resp, "upload_ms", &up_ms);
        parse_int_field(s_resp, "answer_ms", &an_ms);
        jt_show_result(true, textbuf, latency_ms, up_ms, an_ms);
    }
}

void jt_uplink_offer_jpeg(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !s_jpeg_mutex) return;
    if (xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (len > s_jpeg_cap) {
        heap_caps_free(s_jpeg);
        s_jpeg = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        s_jpeg_cap = s_jpeg ? len : 0;
    }
    if (s_jpeg) {
        memcpy(s_jpeg, data, len);
        s_jpeg_len = len;
    }
    xSemaphoreGive(s_jpeg_mutex);
}

void jt_uplink_kick(void)
{
    if (s_upload_sem) xSemaphoreGive(s_upload_sem);
}

void jt_uplink_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_upload_sem = xSemaphoreCreateBinary();
    s_jpeg_mutex = xSemaphoreCreateMutex();
    xTaskCreate(upload_task, "jt_upload", 8192, NULL, 5, NULL);

    /* Netif + driver + event loop first: prov's SoftAP needs them too. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, &inst_ip));
    /* v0.3 provisioning: SETUP mode skips STA entirely; main.c starts the
     * SoftAP + web form after this returns. */
    if (jt_prov_decide() == JT_PROV_MODE_SETUP) {
        ESP_LOGW(TAG, "setup mode: STA skipped, provisioning SoftAP comes next");
        return;
    }
    esp_netif_create_default_wifi_sta();
    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};
    if (!jt_prov_get_sta_creds(sta_ssid, sizeof(sta_ssid), sta_pass, sizeof(sta_pass))) {
        ESP_LOGE(TAG, "STA mode but no creds from prov, WiFi not started");
        return;
    }
    wifi_config_t wificfg = { 0 };
    memcpy(wificfg.sta.ssid, sta_ssid, strnlen(sta_ssid, sizeof(wificfg.sta.ssid)));
    memcpy(wificfg.sta.password, sta_pass, strnlen(sta_pass, sizeof(wificfg.sta.password)));
    wificfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wificfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi station started, connecting (ssid from prov)");
}
