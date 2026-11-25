#include "jt_business.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "JT_BIZ";
static jt_business_card_t s_cards;
static jt_business_environment_t s_environment;
static jt_business_solve_t s_solve;

static void copy_common_error(jt_result_t result, int http_status,
                              const char *error_code, const char *message,
                              char *out_code, size_t out_code_len,
                              char *out_message, size_t out_message_len)
{
    if (error_code && error_code[0]) {
        snprintf(out_code, out_code_len, "%s", error_code);
    }
    if (message && message[0]) {
        snprintf(out_message, out_message_len, "%s", message);
    } else {
        switch (result) {
            case JT_RESULT_NETWORK_ERROR:
                snprintf(out_message, out_message_len, "网络尚未就绪");
                break;
            case JT_RESULT_HTTP_ERROR:
                snprintf(out_message, out_message_len, "网关请求失败(%d)", http_status);
                break;
            case JT_RESULT_SERVER_ERROR:
                snprintf(out_message, out_message_len, "服务返回错误");
                break;
            case JT_RESULT_TIMEOUT:
                snprintf(out_message, out_message_len, "处理超时，请重试");
                break;
            default:
                break;
        }
    }
}

static jt_business_card_t card_from_api(const jt_api_result_t *api)
{
    jt_business_card_t out = {0};
    out.result = api->result;
    out.http_status = api->http_status;
    copy_common_error(api->result, api->http_status,
                      api->error_code, api->message,
                      out.error_code, sizeof(out.error_code),
                      out.message, sizeof(out.message));
    out.balance = api->balance;
    out.daily_total = api->daily_total;
    out.advanced = api->advanced;
    out.advance_limit = api->advance_limit;
    out.remaining_advance = api->remaining_advance;
    out.unlocked = api->unlocked;
    out.can_deduct = api->can_deduct;
    snprintf(out.question_id, sizeof(out.question_id), "%s", api->question_id);
    snprintf(out.answer, sizeof(out.answer), "%s", api->answer);
    return out;
}

static jt_business_action_t action_from_api(const jt_action_result_t *api)
{
    jt_business_action_t out = {0};
    out.result = api->result;
    out.http_status = api->http_status;
    copy_common_error(api->result, api->http_status,
                      api->error_code, api->message,
                      out.error_code, sizeof(out.error_code),
                      out.message, sizeof(out.message));
    snprintf(out.application_id, sizeof(out.application_id), "%s", api->application_id);
    snprintf(out.status, sizeof(out.status), "%s", api->status);
    snprintf(out.created_at, sizeof(out.created_at), "%s", api->created_at);
    out.skipped = api->skipped;
    out.question_index = api->question_index;
    return out;
}

void jt_business_init(void)
{
    memset(&s_cards, 0, sizeof(s_cards));
    memset(&s_environment, 0, sizeof(s_environment));
    memset(&s_solve, 0, sizeof(s_solve));
    s_environment.result = JT_RESULT_NETWORK_ERROR;
    snprintf(s_environment.temperature, sizeof(s_environment.temperature), "---°C");
}

void jt_business_on_solve_result(const jt_solve_result_t *result)
{
    if (!result) {
        return;
    }
    memset(&s_solve, 0, sizeof(s_solve));
    s_solve.result = result->result;
    s_solve.http_status = result->http_status;
    snprintf(s_solve.error_code, sizeof(s_solve.error_code), "%s", result->error_code);
    snprintf(s_solve.message, sizeof(s_solve.message), "%s", result->message);
    snprintf(s_solve.job_id, sizeof(s_solve.job_id), "%s", result->job_id);
    s_solve.question_count = result->question_count;
    if (s_solve.question_count > JT_MAX_QUESTION_COUNT) {
        s_solve.question_count = JT_MAX_QUESTION_COUNT;
    }
    for (int i = 0; i < s_solve.question_count; ++i) {
        s_solve.questions[i] = result->questions[i];
    }

    if (result->result != JT_RESULT_OK) {
        snprintf(s_solve.message, sizeof(s_solve.message), "%s",
                 result->message[0] ? result->message : "判题失败");
    }
}

jt_business_card_t jt_business_cards_refresh(const char *question_id,
                                             const char *batch_id)
{
    jt_api_result_t api = jt_cards_check(question_id, batch_id);
    s_cards = card_from_api(&api);
    return s_cards;
}

const jt_business_card_t *jt_business_cards_cached(void)
{
    return &s_cards;
}

jt_business_card_t jt_business_answer_unlock(const char *question_id,
                                             const char *batch_id,
                                             const char *job_id)
{
    jt_api_result_t api = jt_cards_deduct(question_id, batch_id, job_id);
    jt_business_card_t out = card_from_api(&api);
    if (api.result == JT_RESULT_OK) {
        s_cards = out;
    }
    return out;
}

jt_business_card_t jt_business_cards_advance(void)
{
    jt_api_result_t api = jt_cards_advance();
    jt_business_card_t out = card_from_api(&api);
    if (api.result == JT_RESULT_OK) {
        s_cards = out;
    }
    return out;
}

jt_business_action_t jt_business_application_create(const char *question_id,
                                                    const char *note)
{
    jt_action_result_t api = jt_application_create(question_id, note);
    return action_from_api(&api);
}

jt_business_action_t jt_business_application_withdraw(const char *application_id)
{
    jt_action_result_t api = jt_application_withdraw(application_id);
    return action_from_api(&api);
}

jt_business_environment_t jt_business_server_config_refresh(void)
{
    jt_config_result_t api = jt_config_server_get();
    jt_business_environment_t out = {0};
    out.result = api.result;
    out.http_status = api.http_status;
    copy_common_error(api.result, api.http_status,
                      api.error_code, api.message,
                      out.error_code, sizeof(out.error_code),
                      out.message, sizeof(out.message));
    snprintf(out.updated_at, sizeof(out.updated_at), "%s", api.updated_at);
    if (out.result == JT_RESULT_OK) {
        snprintf(s_environment.updated_at, sizeof(s_environment.updated_at),
                 "%s", out.updated_at);
    }
    return out;
}

jt_business_environment_t jt_business_environment_refresh(void)
{
    jt_config_result_t api = jt_config_environment_get();
    jt_business_environment_t out = {0};
    out.result = api.result;
    out.http_status = api.http_status;
    copy_common_error(api.result, api.http_status,
                      api.error_code, api.message,
                      out.error_code, sizeof(out.error_code),
                      out.message, sizeof(out.message));
    snprintf(out.temperature, sizeof(out.temperature), "%s",
             api.temperature[0] ? api.temperature : "---°C");
    snprintf(out.city, sizeof(out.city), "%s", api.city);
    snprintf(out.memo, sizeof(out.memo), "%s", api.memo);
    snprintf(out.updated_at, sizeof(out.updated_at), "%s", api.updated_at);
    if (out.result == JT_RESULT_OK) {
        s_environment = out;
    }
    return out;
}

const jt_business_environment_t *jt_business_environment_cached(void)
{
    return &s_environment;
}

jt_business_environment_t jt_business_lock_screen_refresh(void)
{
    jt_lock_screen_result_t api = jt_lock_screen_get();
    jt_business_environment_t out = {0};
    out.result = api.result;
    out.http_status = api.http_status;
    if (api.time_synced) {
        snprintf(out.updated_at, sizeof(out.updated_at), "%s", api.http_date);
    }
    snprintf(out.city, sizeof(out.city), "%s", api.city);
    if (api.result == JT_RESULT_OK) {
        snprintf(out.temperature, sizeof(out.temperature), "%s",
                 api.temperature[0] ? api.temperature : "---°C");
        snprintf(out.memo, sizeof(out.memo), "%s", api.memo);
        if (api.city[0]) {
            snprintf(s_environment.city, sizeof(s_environment.city), "%s", api.city);
        }
        snprintf(s_environment.temperature, sizeof(s_environment.temperature), "%s",
                 api.temperature[0] ? api.temperature : "---°C");
        if (api.memo[0]) {
            snprintf(s_environment.memo, sizeof(s_environment.memo), "%s", api.memo);
        }
        if (api.time_synced) {
            snprintf(s_environment.updated_at, sizeof(s_environment.updated_at), "%s",
                     api.http_date);
        }
        s_environment.result = JT_RESULT_OK;
        s_environment.http_status = api.http_status;
    }
    ESP_LOGI(TAG,
             "lock screen refresh result=%s http=%d temp=%s memo_len=%d date=%s",
             jt_result_to_string(api.result), api.http_status,
             api.temperature[0] ? api.temperature : "---",
             (int)strlen(api.memo), api.http_date);
    return out;
}

void jt_business_environment_set(const char *temperature, const char *memo)
{
    if (temperature && temperature[0]) {
        snprintf(s_environment.temperature, sizeof(s_environment.temperature),
                 "%s", temperature);
    }
    if (memo && memo[0]) {
        snprintf(s_environment.memo, sizeof(s_environment.memo), "%s", memo);
    }
}

jt_business_action_t jt_business_solve_cancel(const char *job_id)
{
    jt_action_result_t api = jt_solve_cancel(job_id);
    return action_from_api(&api);
}

jt_business_action_t jt_business_solve_skip(const char *job_id,
                                            int question_index)
{
    jt_action_result_t api = jt_solve_skip(job_id, question_index);
    return action_from_api(&api);
}

const jt_business_solve_t *jt_business_solve_cached(void)
{
    return &s_solve;
}
