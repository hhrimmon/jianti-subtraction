#ifndef __JT_BUSINESS_H__
#define __JT_BUSINESS_H__

#include "jt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} jt_business_card_t;

typedef struct {
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    char temperature[JT_MAX_TEXT_LEN];
    char city[JT_MAX_TEXT_LEN];
    char memo[JT_MAX_ANSWER_TEXT_LEN];
    char updated_at[JT_MAX_TEXT_LEN];
} jt_business_environment_t;

/** Set cached lock-screen environment values without issuing a network request. */
void jt_business_environment_set(const char *temperature, const char *memo);

typedef struct {
    jt_result_t result;
    int http_status;
    char error_code[JT_MAX_ERROR_LEN];
    char message[JT_MAX_ERROR_LEN];
    char job_id[64];
    int question_count;
    jt_question_t questions[JT_MAX_QUESTION_COUNT];
} jt_business_solve_t;

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
} jt_business_action_t;

void jt_business_init(void);

/** Store a solve callback result for later UI/business consumption. */
void jt_business_on_solve_result(const jt_solve_result_t *result);

/** A3. Refreshes the cached card view. */
jt_business_card_t jt_business_cards_refresh(const char *question_id,
                                             const char *batch_id);

/** Cached card view; empty when nothing has been loaded yet. */
const jt_business_card_t *jt_business_cards_cached(void);

/** A4. Unlock the answer with one card. */
jt_business_card_t jt_business_answer_unlock(const char *question_id,
                                             const char *batch_id,
                                             const char *job_id);

/** A5. Advance one card. */
jt_business_card_t jt_business_cards_advance(void);

/** A6. Create an application. */
jt_business_action_t jt_business_application_create(const char *question_id,
                                                    const char *note);

/** A7. Withdraw an application. */
jt_business_action_t jt_business_application_withdraw(const char *application_id);

/** A8. Fetch and cache server config. */
jt_business_environment_t jt_business_server_config_refresh(void);

/** A10. Fetch and cache lock-screen environment. */
jt_business_environment_t jt_business_environment_refresh(void);

/** Lock screen v5.7: fetch /lockscreen, sync HTTP Date, cache text. */
jt_business_environment_t jt_business_lock_screen_refresh(void);

/** Cached environment; empty when nothing has been loaded yet. */
const jt_business_environment_t *jt_business_environment_cached(void);

/** A12. Cancel a pending solve job. */
jt_business_action_t jt_business_solve_cancel(const char *job_id);

/** A13. Skip one question in a solve job. */
jt_business_action_t jt_business_solve_skip(const char *job_id,
                                            int question_index);

/** Cached last solve result. */
const jt_business_solve_t *jt_business_solve_cached(void);

#ifdef __cplusplus
}
#endif

#endif
