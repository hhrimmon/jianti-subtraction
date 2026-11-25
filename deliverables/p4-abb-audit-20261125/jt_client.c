#include "jt_client.h"

#include "esp_http_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "esp_wifi.h"

#include "wifi_local_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "JT_CLIENT";

#ifndef JT_GATEWAY_BASE_URL
#define JT_GATEWAY_BASE_URL "http://192.168.0.108:8000"
#endif

#ifndef JT_TERMINAL_ID
#define JT_TERMINAL_ID "2040059832773436"
#endif

#ifndef JT_AUTH_TOKEN
#define JT_AUTH_TOKEN "880583b2025e8116dcf48266f899322d8a4f553ba0fe8c9d"
#endif

#define JT_POLL_INTERVAL_MS 1000
#define JT_POLL_MAX_COUNT   60
#define JT_MAX_RESPONSE     16384
#define JT_MAX_URL          320

static char s_base_url[192] = JT_GATEWAY_BASE_URL;
static char s_terminal_id[32] = JT_TERMINAL_ID;
static bool s_terminal_id_overridden = false;

const char *jt_client_get_terminal_id(void)
{
    return s_terminal_id;
}

const char *jt_client_terminal_id_source(void)
{
    return s_terminal_id_overridden ? "runtime-config" : "compile-time-default";
}

static char s_auth_token[256] = JT_AUTH_TOKEN;
static bool s_network_ready = false;
static bool s_wifi_started = false;
static bool s_wifi_initialized = false;

static void jt_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        if (evt) {
            char ip[16];
            esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof(ip));
            ESP_LOGI(TAG, "got IP: %s", ip);
        }
        s_network_ready = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "wifi disconnected, reason=%d", evt ? evt->reason : 0);
        s_network_ready = false;
    }
}

void jt_client_mark_network_ready(void)
{
    s_network_ready = true;
}

bool jt_client_network_ready(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        s_network_ready = true;
        return true;
    }
    return s_network_ready;
}

bool jt_client_internet_reachable(uint32_t timeout_ms)
{
    const char *probe_url = "http://www.baidu.com";
    esp_http_client_config_t cfg = {
        .url = probe_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "internet probe init failed: %s", probe_url);
        return false;
    }

    bool reachable = false;
    int status_code = 0;
    const char *resolved_url = probe_url;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        /* A captive portal may answer 4xx/5xx. Only a real upstream response
         * proves that the internet segment was reached. */
        reachable = content_length >= 0 && status_code >= 200 && status_code < 300;
    } else {
        ESP_LOGW(TAG, "internet probe open failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "internet probe %s status=%d reachable=%d",
             resolved_url, status_code, (int)reachable);
    return reachable;
}

bool jt_client_server_reachable(uint32_t timeout_ms)
{
    char url[JT_MAX_URL] = {0};
    snprintf(url, sizeof(url), "%s/lockscreen?device_id=%s",
             s_base_url, s_terminal_id);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "server probe init failed: %s", url);
        return false;
    }

    bool reachable = false;
    int status_code = 0;
    esp_err_t err = esp_http_client_open(client, 0);
    char host_value[128] = "-";
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        char *header = NULL;
        if (esp_http_client_get_header(client, "Host", &header) == ESP_OK &&
            header && *header) {
            snprintf(host_value, sizeof(host_value), "%s", header);
        }
        reachable = content_length >= 0 && status_code > 0 && status_code < 600;
    } else {
        ESP_LOGW(TAG, "server probe open failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "server probe %s host=%s status=%d reachable=%d",
             url, host_value, status_code, (int)reachable);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return reachable;
}

bool jt_client_network_init(uint32_t timeout_ms)
{
    if (s_network_ready) {
        return true;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return false;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    static esp_event_handler_instance_t wifi_handler;
    static esp_event_handler_instance_t ip_handler;
    static bool handlers_registered = false;
    if (!handlers_registered) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            jt_wifi_event_handler, NULL, &wifi_handler);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            jt_wifi_event_handler, NULL, &ip_handler);
        handlers_registered = true;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!s_wifi_initialized) {
        init_cfg.magic = WIFI_INIT_CONFIG_MAGIC;
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_wifi_init with explicit magic failed: %s, retrying default",
                     esp_err_to_name(err));
            err = esp_wifi_init(NULL);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "esp_wifi_init default failed: %s", esp_err_to_name(err));
                return false;
            }
        }
        s_wifi_initialized = true;
        ESP_LOGI(TAG, "wifi driver initialized");
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi set storage failed: %s", esp_err_to_name(err));
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi set mode failed: %s", esp_err_to_name(err));
        return false;
    }

    wifi_config_t wifi_cfg = {0};
    size_t ssid_len = strlen(JT_WIFI_SSID);
    size_t pass_len = strlen(JT_WIFI_PASSWORD);
    if (ssid_len < sizeof(wifi_cfg.sta.ssid) && pass_len < sizeof(wifi_cfg.sta.password)) {
        memcpy(wifi_cfg.sta.ssid, JT_WIFI_SSID, ssid_len);
        memcpy(wifi_cfg.sta.password, JT_WIFI_PASSWORD, pass_len);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi set config failed: %s", esp_err_to_name(err));
            return false;
        }
    } else {
        ESP_LOGE(TAG, "local WiFi config length invalid");
        return false;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        } else {
            ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "wifi connect failed: %s", esp_err_to_name(err));
    }

    const uint32_t poll_ms = 200;
    uint32_t waited_ms = 0;
    while (!s_network_ready && waited_ms < timeout_ms) {
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            s_network_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }

    if (!s_network_ready) {
        ESP_LOGW(TAG, "network not ready after %u ms", (unsigned)waited_ms);
    }
    return s_network_ready;
}

typedef struct {
    uint8_t *jpeg;
    size_t jpeg_size;
    jt_solve_callback_t callback;
    void *user_ctx;
} jt_solve_job_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int status_code;
    char http_date[128];
} jt_http_response_t;

static esp_err_t jt_http_event_handler(esp_http_client_event_t *evt)
{
    jt_http_response_t *resp = (jt_http_response_t *)evt->user_data;
    if (resp && evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Date") == 0) {
        snprintf(resp->http_date, sizeof(resp->http_date), "%s", evt->header_value);
        return ESP_OK;
    }
    if (!resp || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (resp->length >= JT_MAX_RESPONSE) {
        return ESP_OK;
    }
    size_t data_len = (size_t)evt->data_len;
    if (data_len > JT_MAX_RESPONSE - resp->length) {
        data_len = JT_MAX_RESPONSE - resp->length;
    }
    size_t needed = resp->length + data_len + 1;
    if (needed > resp->capacity) {
        size_t new_cap = resp->capacity ? resp->capacity : 1024;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *tmp = (char *)realloc(resp->data, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        resp->data = tmp;
        resp->capacity = new_cap;
    }
    memcpy(resp->data + resp->length, evt->data, data_len);
    resp->length += data_len;
    resp->data[resp->length] = '\0';
    return ESP_OK;
}

static void jt_response_free(jt_http_response_t *resp)
{
    if (resp->data) {
        free(resp->data);
    }
    memset(resp, 0, sizeof(*resp));
}

static bool jt_apply_http_date(const char *value)
{
    struct tm parsed = {0};
    char *end = NULL;
    char copy[160];

    if (!value || !value[0]) {
        return false;
    }
    snprintf(copy, sizeof(copy), "%s", value);
    end = strptime(copy, "%a, %d %b %Y %H:%M:%S GMT", &parsed);
    if (!end) {
        end = strptime(copy, "%A, %d-%b-%y %H:%M:%S GMT", &parsed);
    }
    if (!end) {
        end = strptime(copy, "%a %b %e %H:%M:%S %Y", &parsed);
    }
    if (!end) {
        return false;
    }
    parsed.tm_isdst = -1;
    /* newlib does not declare timegm(); mktime() interprets tm as local time,
       so add the GMT offset for the configured Asia/Shanghai zone (8h). */
    time_t remote = mktime(&parsed) + 8 * 60 * 60;
    if (remote <= 0) {
        return false;
    }
    struct timeval now = {.tv_sec = remote, .tv_usec = 0};
    if (settimeofday(&now, NULL) != 0) {
        return false;
    }
    ESP_LOGI(TAG, "lock screen time synced from HTTP Date: %s", value);
    return true;
}

static void jt_set_error(jt_solve_result_t *out, jt_result_t result,
                         int http_status, const char *code, const char *message)
{
    out->result = result;
    out->http_status = http_status;
    if (code) {
        snprintf(out->error_code, sizeof(out->error_code), "%s", code);
    }
    if (message) {
        snprintf(out->message, sizeof(out->message), "%s", message);
    }
}

static const char *jt_json_get_string(cJSON *root, const char *name)
{
    cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, name) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void jt_parse_error(const jt_http_response_t *resp,
                           jt_solve_result_t *out, jt_result_t fallback)
{
    const char *code = NULL;
    const char *message = NULL;
    cJSON *root = resp->data ? cJSON_Parse(resp->data) : NULL;
    if (root) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(error)) {
            code = jt_json_get_string(error, "code");
            message = jt_json_get_string(error, "message");
        }
        if (!code) {
            code = jt_json_get_string(root, "code");
        }
        if (!message) {
            message = jt_json_get_string(root, "message");
        }
        cJSON_Delete(root);
    }
    jt_set_error(out, fallback, resp->status_code, code, message);
}

static int jt_http_perform(const char *url, esp_http_client_method_t method,
                           const char *content_type, const uint8_t *body,
                           size_t body_size, const char *extra_header_name,
                           const char *extra_header_value,
                           jt_http_response_t *out)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = jt_http_event_handler,
        .user_data = out,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }

    esp_http_client_set_header(client, "X-Terminal-Id", s_terminal_id);
    if (s_auth_token[0]) {
        char auth[320];
        snprintf(auth, sizeof(auth), "Bearer %s", s_auth_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (content_type) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (extra_header_name && extra_header_value) {
        esp_http_client_set_header(client, extra_header_name, extra_header_value);
    }

    esp_err_t err = esp_http_client_open(client, body ? (int)body_size : 0);
    if (err == ESP_OK && body && body_size) {
        int written = esp_http_client_write(client, (const char *)body, (int)body_size);
        if (written < 0) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            err = ESP_FAIL;
        }
        out->status_code = esp_http_client_get_status_code(client);
        if (out->http_date[0] == 0) {
            char *date = NULL;
            if (esp_http_client_get_header(client, "Date", &date) == ESP_OK && date) {
                snprintf(out->http_date, sizeof(out->http_date), "%s", date);
                free(date);
            }
        }
        if (err == ESP_OK) {
            char read_buf[512];
            while (true) {
                int read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));
                if (read_len <= 0) {
                    break;
                }
                /* HTTP_EVENT_ON_DATA already stores the body. */
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http request failed: %s", esp_err_to_name(err));
        return -1;
    }
    return out->status_code;
}

static void jt_make_idempotency_key(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(buf, len, "jt-p4-%08x%08x",
             (unsigned)tv.tv_sec, (unsigned)tv.tv_usec);
}

static bool jt_parse_upload(const jt_http_response_t *resp,
                            jt_solve_result_t *out)
{
    cJSON *root = resp->data ? cJSON_Parse(resp->data) : NULL;
    if (!root) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     "invalid_json", "网关返回不是合法 JSON");
        return false;
    }
    const char *job_id = jt_json_get_string(root, "job_id");
    if (!job_id) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        job_id = jt_json_get_string(data, "job_id");
    }
    if (!job_id) {
        jt_parse_error(resp, out, JT_RESULT_SERVER_ERROR);
        cJSON_Delete(root);
        return false;
    }
    snprintf(out->job_id, sizeof(out->job_id), "%s", job_id);
    cJSON_Delete(root);
    return true;
}

static bool jt_parse_poll(const jt_http_response_t *resp,
                          jt_solve_result_t *out, bool *finished)
{
    *finished = false;
    cJSON *root = resp->data ? cJSON_Parse(resp->data) : NULL;
    if (!root) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     "invalid_json", "网关轮询返回不是合法 JSON");
        return false;
    }

    const char *status = jt_json_get_string(root, "status");
    const char *failure_reason = jt_json_get_string(root, "failure_reason");
    if (!failure_reason) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        failure_reason = jt_json_get_string(error, "code");
    }

    if (failure_reason && strcmp(failure_reason, "no_question") == 0) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     "no_question", "这张里没有题目");
        *finished = true;
        cJSON_Delete(root);
        return false;
    }
    if (failure_reason && strcmp(failure_reason, "recognition_failed") == 0) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     "recognition_failed", "没看清题目，请重拍");
        *finished = true;
        cJSON_Delete(root);
        return false;
    }
    if (failure_reason && strcmp(failure_reason, "model_unavailable") == 0) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     "model_unavailable", "判题服务暂时接不通");
        *finished = true;
        cJSON_Delete(root);
        return false;
    }
    if (status && strcmp(status, "failed") == 0) {
        jt_set_error(out, JT_RESULT_SERVER_ERROR, resp->status_code,
                     failure_reason ? failure_reason : "failed",
                     "判题失败，请重拍");
        *finished = true;
        cJSON_Delete(root);
        return false;
    }

    cJSON *questions = cJSON_GetObjectItemCaseSensitive(root, "questions");
    if (status && strcmp(status, "done") == 0 && cJSON_IsArray(questions)) {
        int count = cJSON_GetArraySize(questions);
        if (count > JT_MAX_QUESTION_COUNT) {
            count = JT_MAX_QUESTION_COUNT;
        }
        out->question_count = count;
        for (int i = 0; i < count; ++i) {
            cJSON *q = cJSON_GetArrayItem(questions, i);
            jt_question_t *dst = &out->questions[i];
            const char *qid = jt_json_get_string(q, "question_id");
            const char *thinking = jt_json_get_string(q, "thinking");
            const char *answer = jt_json_get_string(q, "answer");
            cJSON *correct = q ? cJSON_GetObjectItemCaseSensitive(q, "correct") : NULL;
            cJSON *unlocked = q ? cJSON_GetObjectItemCaseSensitive(q, "unlocked") : NULL;
            cJSON *idx = q ? cJSON_GetObjectItemCaseSensitive(q, "question_index") : NULL;
            if (qid) {
                snprintf(dst->question_id, sizeof(dst->question_id), "%s", qid);
            }
            if (thinking) {
                snprintf(dst->thinking, sizeof(dst->thinking), "%s", thinking);
            }
            if (answer) {
                snprintf(dst->answer, sizeof(dst->answer), "%s", answer);
            }
            dst->correct = cJSON_IsBool(correct) ? cJSON_IsTrue(correct) : false;
            dst->unlocked = cJSON_IsBool(unlocked) ? cJSON_IsTrue(unlocked) : false;
            dst->question_index = cJSON_IsNumber(idx) ? idx->valueint : (i + 1);
        }
        out->result = JT_RESULT_OK;
        *finished = true;
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return true;
}

static bool jt_solve_worker(const uint8_t *jpeg, size_t jpeg_size,
                            jt_solve_result_t *out)
{
    if (!jt_client_network_init(10000)) {
        jt_set_error(out, JT_RESULT_NETWORK_ERROR, 0,
                     "network_not_ready", "网络尚未就绪");
        return false;
    }

    char upload_url[256];
    snprintf(upload_url, sizeof(upload_url), "%s/solve", s_base_url);

    char idempotency[64];
    jt_make_idempotency_key(idempotency, sizeof(idempotency));

    jt_http_response_t resp = {0};
    int status = jt_http_perform(upload_url, HTTP_METHOD_POST, "image/jpeg",
                                 jpeg, jpeg_size,
                                 "Idempotency-Key", idempotency, &resp);
    if (status < 0) {
        jt_set_error(out, JT_RESULT_NETWORK_ERROR, 0, "network_error", "网络不可用");
        jt_response_free(&resp);
        return false;
    }
    if (status != 200 && status != 202) {
        ESP_LOGE(TAG, "solve upload HTTP error: status=%d body=%s",
                 status, resp.data ? resp.data : "");
        jt_parse_error(&resp, out, JT_RESULT_HTTP_ERROR);
        jt_response_free(&resp);
        return false;
    }
    if (!jt_parse_upload(&resp, out)) {
        jt_response_free(&resp);
        return false;
    }
    jt_response_free(&resp);

    char poll_url[JT_MAX_URL];
    snprintf(poll_url, sizeof(poll_url), "%s/solve/%s", s_base_url, out->job_id);

    for (int i = 0; i < JT_POLL_MAX_COUNT; ++i) {
        vTaskDelay(pdMS_TO_TICKS(JT_POLL_INTERVAL_MS));
        jt_http_response_t poll_resp = {0};
        int poll_status = jt_http_perform(poll_url, HTTP_METHOD_GET, NULL,
                                          NULL, 0, NULL, NULL, &poll_resp);
        if (poll_status < 0) {
            jt_set_error(out, JT_RESULT_NETWORK_ERROR, 0,
                         "network_error", "轮询时网络断开");
            jt_response_free(&poll_resp);
            return false;
        }
        if (poll_status != 200) {
            ESP_LOGE(TAG, "solve poll HTTP error: status=%d body=%s",
                     poll_status, poll_resp.data ? poll_resp.data : "");
            jt_parse_error(&poll_resp, out, JT_RESULT_HTTP_ERROR);
            jt_response_free(&poll_resp);
            return false;
        }
        bool finished = false;
        bool ok = jt_parse_poll(&poll_resp, out, &finished);
        jt_response_free(&poll_resp);
        if (!ok || finished) {
            return ok;
        }
    }

    jt_set_error(out, JT_RESULT_TIMEOUT, 200, "50401", "处理超时，请重拍或稍后再试");
    return false;
}

static void jt_solve_task(void *arg)
{
    jt_solve_job_t *job = (jt_solve_job_t *)arg;
    jt_solve_result_t result = {0};

    jt_solve_worker(job->jpeg, job->jpeg_size, &result);
    if (job->callback) {
        job->callback(&result, job->user_ctx);
    }

    free(job->jpeg);
    free(job);
    vTaskDelete(NULL);
}

void jt_client_set_config(const char *base_url,
                          const char *terminal_id,
                          const char *auth_token)
{
    if (base_url && base_url[0]) {
        snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
    }
    if (terminal_id && terminal_id[0]) {
        snprintf(s_terminal_id, sizeof(s_terminal_id), "%s", terminal_id);
        s_terminal_id_overridden = true;
    }
    if (auth_token) {
        snprintf(s_auth_token, sizeof(s_auth_token), "%s", auth_token);
    }
}

bool jt_client_solve_async(const uint8_t *jpeg, size_t jpeg_size,
                           jt_solve_callback_t cb, void *user_ctx)
{
    if (!jpeg || jpeg_size == 0 || jpeg_size > 8 * 1024 * 1024) {
        return false;
    }

    jt_solve_job_t *job = (jt_solve_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        return false;
    }
    job->jpeg = (uint8_t *)malloc(jpeg_size);
    if (!job->jpeg) {
        free(job);
        return false;
    }
    memcpy(job->jpeg, jpeg, jpeg_size);
    job->jpeg_size = jpeg_size;
    job->callback = cb;
    job->user_ctx = user_ctx;

    BaseType_t ok = xTaskCreate(jt_solve_task, "jt_solve", 32768, job, 5, NULL);
    if (ok != pdPASS) {
        free(job->jpeg);
        free(job);
        return false;
    }
    return true;
}

static void jt_api_set_error(jt_api_result_t *out, jt_result_t result,
                             int http_status, const char *code,
                             const char *message)
{
    out->result = result;
    out->http_status = http_status;
    if (code) {
        snprintf(out->error_code, sizeof(out->error_code), "%s", code);
    }
    if (message) {
        snprintf(out->message, sizeof(out->message), "%s", message);
    }
}

static int jt_api_int(cJSON *root, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool jt_api_bool(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : false;
}

static void jt_api_copy_string(char *dst, size_t dst_len, cJSON *root,
                               const char *name)
{
    const char *value = jt_json_get_string(root, name);
    if (value) {
        snprintf(dst, dst_len, "%s", value);
    }
}

static jt_api_result_t jt_api_request_data(const char *path,
                                           esp_http_client_method_t method,
                                           const char *body,
                                           bool idempotent,
                                           cJSON **data_out)
{
    jt_api_result_t out = {0};
    if (data_out) {
        *data_out = NULL;
    }
    if (!jt_client_network_init(10000)) {
        jt_api_set_error(&out, JT_RESULT_NETWORK_ERROR, 0,
                         "network_not_ready", "网络尚未就绪");
        return out;
    }

    char url[JT_MAX_URL + 192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    char idempotency[64] = {0};
    if (idempotent) {
        jt_make_idempotency_key(idempotency, sizeof(idempotency));
    }

    jt_http_response_t resp = {0};
    int status = jt_http_perform(url, method, "application/json",
                                 (const uint8_t *)body, body ? strlen(body) : 0,
                                 idempotent ? "Idempotency-Key" : NULL,
                                 idempotent ? idempotency : NULL, &resp);
    if (status < 0) {
        jt_api_set_error(&out, JT_RESULT_NETWORK_ERROR, 0,
                         "network_error", "网络不可用");
        jt_response_free(&resp);
        return out;
    }
    out.http_status = status;
    if (status < 200 || status >= 300) {
        jt_result_t fallback = status >= 500 ? JT_RESULT_SERVER_ERROR
                                             : JT_RESULT_HTTP_ERROR;
        cJSON *err_root = resp.data ? cJSON_Parse(resp.data) : NULL;
        const char *code = NULL;
        const char *msg = NULL;
        if (err_root) {
            cJSON *error = cJSON_GetObjectItemCaseSensitive(err_root, "error");
            if (cJSON_IsObject(error)) {
                code = jt_json_get_string(error, "code");
                msg = jt_json_get_string(error, "message");
            }
            cJSON_Delete(err_root);
        }
        jt_api_set_error(&out, fallback, status, code, msg);
        jt_response_free(&resp);
        return out;
    }

    cJSON *root = resp.data ? cJSON_Parse(resp.data) : NULL;
    if (!root) {
        jt_api_set_error(&out, JT_RESULT_SERVER_ERROR, status,
                         "invalid_json", "网关返回不是合法 JSON");
        jt_response_free(&resp);
        return out;
    }

    /* v9 gateway uses a bare JSON object on success; accept the documented
       {code,message,data} envelope too for older/future deployments. */
    cJSON *payload = root;
    cJSON *envelope = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(envelope)) {
        payload = envelope;
    }
    if (data_out) {
        *data_out = cJSON_Duplicate(payload, true);
        cJSON_Delete(root);
        jt_response_free(&resp);
        out.result = JT_RESULT_OK;
        return out;
    }

    out.balance = jt_api_int(payload, "balance", 0);
    out.daily_total = jt_api_int(payload, "daily_total", 0);
    out.advanced = jt_api_int(payload, "advanced", 0);
    out.advance_limit = jt_api_int(payload, "advance_limit", 0);
    out.remaining_advance = jt_api_int(payload, "remaining_advance", 0);
    out.unlocked = jt_api_bool(payload, "unlocked");
    out.can_deduct = jt_api_bool(payload, "can_deduct");
    jt_api_copy_string(out.question_id, sizeof(out.question_id), payload, "question_id");
    jt_api_copy_string(out.answer, sizeof(out.answer), payload, "answer");
    out.result = JT_RESULT_OK;

    cJSON_Delete(root);
    jt_response_free(&resp);
    return out;
}

static jt_api_result_t jt_api_request(const char *path,
                                      esp_http_client_method_t method,
                                      const char *body,
                                      bool idempotent)
{
    return jt_api_request_data(path, method, body, idempotent, NULL);
}

jt_api_result_t jt_cards_check(const char *question_id, const char *batch_id)
{
    char body[320];
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        jt_api_result_t out = {0};
        jt_api_set_error(&out, JT_RESULT_SERVER_ERROR, 0,
                         "no_memory", "构造请求失败");
        return out;
    }
    cJSON_AddStringToObject(root, "question_id", question_id ? question_id : "");
    cJSON_AddStringToObject(root, "batch_id", batch_id ? batch_id : "");
    char *encoded = cJSON_PrintUnformatted(root);
    snprintf(body, sizeof(body), "%s", encoded ? encoded : "{}");
    if (encoded) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    return jt_api_request("/cards/check", HTTP_METHOD_POST, body, false);
}

jt_api_result_t jt_cards_deduct(const char *question_id,
                                const char *batch_id,
                                const char *job_id)
{
    char body[384];
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        jt_api_result_t out = {0};
        jt_api_set_error(&out, JT_RESULT_SERVER_ERROR, 0,
                         "no_memory", "构造请求失败");
        return out;
    }
    cJSON_AddStringToObject(root, "question_id", question_id ? question_id : "");
    cJSON_AddStringToObject(root, "batch_id", batch_id ? batch_id : "");
    cJSON_AddStringToObject(root, "job_id", job_id ? job_id : "");
    char *encoded = cJSON_PrintUnformatted(root);
    snprintf(body, sizeof(body), "%s", encoded ? encoded : "{}");
    if (encoded) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    return jt_api_request("/cards/deduct", HTTP_METHOD_POST, body, true);
}

jt_api_result_t jt_cards_advance(void)
{
    return jt_api_request("/cards/advance", HTTP_METHOD_POST, "{}", true);
}

static char *jt_json_string(const cJSON *root, const char *name)
{
    const char *value = jt_json_get_string((cJSON *)root, name);
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, value, len);
    }
    return copy;
}

static jt_action_result_t jt_action_request_data(const char *path,
                                                  esp_http_client_method_t method,
                                                  const char *body,
                                                  bool idempotent,
                                                  char **application_id_out,
                                                  char **status_out,
                                                  char **created_at_out,
                                                  bool *skipped_out,
                                                  int *question_index_out)
{
    jt_action_result_t out = {0};
    cJSON *data = NULL;
    jt_api_result_t api = jt_api_request_data(path, method, body, idempotent, &data);
    out.result = api.result;
    out.http_status = api.http_status;
    snprintf(out.error_code, sizeof(out.error_code), "%s", api.error_code);
    snprintf(out.message, sizeof(out.message), "%s", api.message);
    if (api.result != JT_RESULT_OK || !data) {
        if (data) {
            cJSON_Delete(data);
        }
        return out;
    }

    if (application_id_out) {
        *application_id_out = jt_json_string(data, "application_id");
        if (!*application_id_out) {
            *application_id_out = jt_json_string(data, "id");
        }
    }
    if (status_out) {
        *status_out = jt_json_string(data, "status");
    }
    if (created_at_out) {
        *created_at_out = jt_json_string(data, "created_at");
    }
    if (skipped_out) {
        cJSON *skipped = cJSON_GetObjectItemCaseSensitive(data, "skipped");
        *skipped_out = cJSON_IsBool(skipped) && cJSON_IsTrue(skipped);
    }
    if (question_index_out) {
        cJSON *index = cJSON_GetObjectItemCaseSensitive(data, "question_index");
        *question_index_out = cJSON_IsNumber(index) ? index->valueint : 0;
    }
    cJSON_Delete(data);
    return out;
}

jt_action_result_t jt_application_create(const char *question_id,
                                         const char *note)
{
    char body[512];
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        jt_action_result_t out = {0};
        return out;
    }
    cJSON_AddStringToObject(root, "terminal_id", s_terminal_id);
    cJSON_AddStringToObject(root, "question_id", question_id ? question_id : "");
    cJSON_AddStringToObject(root, "note", note ? note : "");
    char *encoded = cJSON_PrintUnformatted(root);
    snprintf(body, sizeof(body), "%s", encoded ? encoded : "{}");
    if (encoded) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);

    char *application_id = NULL;
    char *status = NULL;
    char *created_at = NULL;
    jt_action_result_t out = jt_action_request_data(
        "/applications", HTTP_METHOD_POST, body, true,
        &application_id, &status, &created_at, NULL, NULL);
    if (application_id) {
        snprintf(out.application_id, sizeof(out.application_id), "%s", application_id);
        free(application_id);
    }
    if (status) {
        snprintf(out.status, sizeof(out.status), "%s", status);
        free(status);
    }
    if (created_at) {
        snprintf(out.created_at, sizeof(out.created_at), "%s", created_at);
        free(created_at);
    }
    return out;
}

jt_action_result_t jt_application_withdraw(const char *application_id)
{
    char path[256];
    snprintf(path, sizeof(path), "/applications/%s/withdraw",
             application_id ? application_id : "");
    char *status = NULL;
    jt_action_result_t out = jt_action_request_data(path, HTTP_METHOD_POST, "{}", false,
                                                    NULL, &status, NULL, NULL, NULL);
    if (status) {
        snprintf(out.status, sizeof(out.status), "%s", status);
        free(status);
    }
    return out;
}

jt_action_result_t jt_solve_cancel(const char *job_id)
{
    char path[256];
    snprintf(path, sizeof(path), "/solve/%s/cancel", job_id ? job_id : "");
    char *status = NULL;
    jt_action_result_t out = jt_action_request_data(path, HTTP_METHOD_POST, "{}", false,
                                                    NULL, &status, NULL, NULL, NULL);
    if (status) {
        snprintf(out.status, sizeof(out.status), "%s", status);
        free(status);
    }
    return out;
}

jt_action_result_t jt_solve_skip(const char *job_id, int question_index)
{
    char path[256];
    char body[96];
    snprintf(path, sizeof(path), "/solve/%s/skip", job_id ? job_id : "");
    snprintf(body, sizeof(body), "{\"question_index\":%d}", question_index);
    bool skipped = false;
    int returned_index = 0;
    jt_action_result_t out = jt_action_request_data(path, HTTP_METHOD_POST, body, false,
                                                    NULL, NULL, NULL,
                                                    &skipped, &returned_index);
    out.skipped = skipped;
    out.question_index = returned_index;
    return out;
}

static jt_config_result_t jt_config_request(const char *path)
{
    jt_config_result_t out = {0};
    cJSON *data = NULL;
    jt_api_result_t api = jt_api_request_data(path, HTTP_METHOD_GET, NULL, false, &data);
    out.result = api.result;
    out.http_status = api.http_status;
    snprintf(out.error_code, sizeof(out.error_code), "%s", api.error_code);
    snprintf(out.message, sizeof(out.message), "%s", api.message);
    if (data) {
        jt_api_copy_string(out.server_mode, sizeof(out.server_mode), data, "server_mode");
        if (!out.server_mode[0]) {
            jt_api_copy_string(out.server_mode, sizeof(out.server_mode), data, "mode");
        }
        jt_api_copy_string(out.custom_ip, sizeof(out.custom_ip), data, "custom_ip");
        if (!out.custom_ip[0]) {
            jt_api_copy_string(out.custom_ip, sizeof(out.custom_ip), data, "custom_address");
        }
        jt_api_copy_string(out.updated_at, sizeof(out.updated_at), data, "updated_at");
        jt_api_copy_string(out.temperature, sizeof(out.temperature), data, "temperature");
        jt_api_copy_string(out.city, sizeof(out.city), data, "city");
        jt_api_copy_string(out.memo, sizeof(out.memo), data, "memo");
        cJSON_Delete(data);
    }
    return out;
}

jt_config_result_t jt_config_server_get(void)
{
    return jt_config_request("/config/server");
}

jt_config_result_t jt_config_environment_get(void)
{
    return jt_config_request("/config/environment");
}

jt_lock_screen_result_t jt_lock_screen_get(void)
{
    jt_lock_screen_result_t out = {0};
    out.result = JT_RESULT_NETWORK_ERROR;
    if (!jt_client_network_ready()) {
        return out;
    }

    char path[160];
    snprintf(path, sizeof(path), "/lockscreen?device_id=%s", s_terminal_id);
    char url[JT_MAX_URL + 192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    jt_http_response_t resp = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = jt_http_event_handler,
        .user_data = &resp,
        .timeout_ms = 3000,
        .disable_auto_redirect = true,
    };
    ESP_LOGI(TAG, "lock screen GET %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        jt_response_free(&resp);
        return out;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        resp.status_code = esp_http_client_get_status_code(client);
        char read_buf[512];
        while (true) {
            int read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));
            if (read_len <= 0) {
                break;
            }
        }
    }
    if (resp.http_date[0] == 0) {
        char *date = NULL;
        if (esp_http_client_get_header(client, "Date", &date) == ESP_OK && date) {
            snprintf(resp.http_date, sizeof(resp.http_date), "%s", date);
            free(date);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out.http_status = resp.status_code;
    snprintf(out.http_date, sizeof(out.http_date), "%s", resp.http_date);
    ESP_LOGI(TAG, "lock screen response status=%d bytes=%d",
             out.http_status, (int)resp.length);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lock screen request failed: %s", esp_err_to_name(err));
        jt_response_free(&resp);
        return out;
    }
    if (out.http_status < 200 || out.http_status >= 300) {
        ESP_LOGW(TAG, "lock screen http error status=%d body=%s",
                 out.http_status, resp.data ? resp.data : "");
        out.result = JT_RESULT_HTTP_ERROR;
        jt_response_free(&resp);
        return out;
    }

    cJSON *root = resp.data ? cJSON_Parse(resp.data) : NULL;
    if (!root) {
        ESP_LOGW(TAG, "lock screen invalid json body=%s", resp.data ? resp.data : "");
        out.result = JT_RESULT_SERVER_ERROR;
        jt_response_free(&resp);
        return out;
    }
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    cJSON *memo = cJSON_GetObjectItemCaseSensitive(root, "memo");
    cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (cJSON_IsNumber(temperature)) {
        snprintf(out.temperature, sizeof(out.temperature), "%d°C", temperature->valueint);
    }
    if (cJSON_IsString(temperature) && temperature->valuestring &&
        temperature->valuestring[0]) {
        snprintf(out.temperature, sizeof(out.temperature), "%s", temperature->valuestring);
    }
    if (cJSON_IsString(memo) && memo->valuestring) {
        snprintf(out.memo, sizeof(out.memo), "%s", memo->valuestring);
    }
    if (cJSON_IsString(city) && city->valuestring) {
        snprintf(out.city, sizeof(out.city), "%s", city->valuestring);
    }
    cJSON_Delete(root);
    jt_response_free(&resp);
    out.time_synced = jt_apply_http_date(out.http_date);
    out.result = JT_RESULT_OK;
    ESP_LOGI(TAG, "lock screen data city=%s temp=%s memo_len=%d synced=%d date=%s",
             out.city, out.temperature, (int)strlen(out.memo),
             out.time_synced ? 1 : 0, out.http_date);
    return out;
}

const char *jt_result_to_string(jt_result_t result)
{
    switch (result) {
        case JT_RESULT_OK: return "ok";
        case JT_RESULT_NETWORK_ERROR: return "network_error";
        case JT_RESULT_HTTP_ERROR: return "http_error";
        case JT_RESULT_SERVER_ERROR: return "server_error";
        case JT_RESULT_TIMEOUT: return "timeout";
        default: return "unknown";
    }
}
