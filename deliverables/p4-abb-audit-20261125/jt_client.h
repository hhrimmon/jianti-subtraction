#ifndef __JT_CLIENT_H__
#define __JT_CLIENT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define JT_MAX_QUESTION_COUNT 8
#define JT_MAX_THINKING_LEN   1024
#define JT_MAX_ANSWER_LEN     1024
#define JT_MAX_ERROR_LEN      128

typedef enum {
    JT_RESULT_OK = 0,
    JT_RESULT_NETWORK_ERROR,
    JT_RESULT_HTTP_ERROR,
    JT_RESULT_SERVER_ERROR,
    JT_RESULT_TIMEOUT,
} jt_result_t;

typedef struct {
    char job_id[64];
    char question_id[96];
    int question_index;
    bool correct;
    char thinking[JT_MAX_THINKING_LEN];
    char answer[JT_MAX_ANSWER_LEN];
    bool unlocked;
} jt_question_t;

typedef struct {
    char job_id[64];
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    int question_count;
    jt_question_t questions[JT_MAX_QUESTION_COUNT];
} jt_solve_result_t;

typedef void (*jt_solve_callback_t)(const jt_solve_result_t *result, void *user_ctx);

#define JT_MAX_ANSWER_TEXT_LEN 1024
#define JT_MAX_ID_LEN         96
#define JT_MAX_TEXT_LEN       128

typedef struct {
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    int balance;
    int daily_total;
    int advanced;
    int advance_limit;
    int remaining_advance;
    bool unlocked;
    bool can_deduct;
    char question_id[JT_MAX_ID_LEN];
    char answer[JT_MAX_ANSWER_TEXT_LEN];
} jt_api_result_t;

typedef struct {
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    char server_mode[JT_MAX_TEXT_LEN];
    char custom_ip[JT_MAX_TEXT_LEN];
    char updated_at[JT_MAX_TEXT_LEN];
    char temperature[JT_MAX_TEXT_LEN];
    char city[JT_MAX_TEXT_LEN];
    char memo[JT_MAX_ANSWER_TEXT_LEN];
} jt_config_result_t;

/** Lock-screen data from GET /lockscreen. */
typedef struct {
    jt_result_t result;
    int http_status;
    bool time_synced;
    char http_date[JT_MAX_TEXT_LEN];
    char city[JT_MAX_TEXT_LEN];
    char temperature[JT_MAX_TEXT_LEN];
    char memo[JT_MAX_ANSWER_TEXT_LEN];
} jt_lock_screen_result_t;

typedef struct {
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    char application_id[JT_MAX_ID_LEN];
    char status[JT_MAX_TEXT_LEN];
    char created_at[JT_MAX_TEXT_LEN];
    bool skipped;
    int question_index;
} jt_action_result_t;

void jt_client_set_config(const char *base_url,
                          const char *terminal_id,
                          const char *auth_token);

/** Current terminal/device id used in server requests. */
const char *jt_client_get_terminal_id(void);

/**
 * @brief Initialize lwIP and connect the ESP32-C6 Wi-Fi uplink.
 * @note Safe to call multiple times. Uploads wait for a valid IPv4 address.
 */
bool jt_client_network_init(uint32_t timeout_ms);

/**
 * @brief Start the project's proven STA flow and mark the network as ready.
 * @note Use this when app_wifi.c has already initialized/connected Wi-Fi.
 */
void jt_client_mark_network_ready(void);

/** True after ESP32-C6 station has a valid IPv4 address. */
bool jt_client_network_ready(void);

/**
 * @brief Upload one raw JPEG and poll A2 until done/failed/timeout.
 * @note The callback is invoked from the worker task. Do not touch LVGL
 *       objects directly in the callback; hand it to the GUI task.
 */
bool jt_client_solve_async(const uint8_t *jpeg, size_t jpeg_size,
                           jt_solve_callback_t cb, void *user_ctx);

/**
 * @brief A3 POST /cards/check. Pass an empty question_id to query balance only.
 * @note Blocking API; call from a worker task, not from the LVGL task.
 */
jt_api_result_t jt_cards_check(const char *question_id, const char *batch_id);

/** A4 POST /cards/deduct. */
jt_api_result_t jt_cards_deduct(const char *question_id,
                                const char *batch_id,
                                const char *job_id);

/** A5 POST /cards/advance. */
jt_api_result_t jt_cards_advance(void);

/** A6 POST /applications. */
jt_action_result_t jt_application_create(const char *question_id,
                                         const char *note);

/** A7 POST /applications/{id}/withdraw. */
jt_action_result_t jt_application_withdraw(const char *application_id);

/** A8 GET /config/server. */
jt_config_result_t jt_config_server_get(void);

/** A10 GET /config/environment (public endpoint). */
jt_config_result_t jt_config_environment_get(void);

/** Lock screen GET /lockscreen?device_id=..., 3s timeout, public endpoint. */
jt_lock_screen_result_t jt_lock_screen_get(void);

/** A12 POST /solve/{job_id}/cancel. */
jt_action_result_t jt_solve_cancel(const char *job_id);

/** A13 POST /solve/{job_id}/skip. */
jt_action_result_t jt_solve_skip(const char *job_id, int question_index);

const char *jt_result_to_string(jt_result_t result);

#ifdef __cplusplus
}
#endif

#endif
