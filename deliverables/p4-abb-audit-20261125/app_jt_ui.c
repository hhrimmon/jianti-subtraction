#include "app_jt_ui.h"

#include "esp_rtc.h"

#include "app_jt_ui_spec_fonts.h"
#include "jt_v113_icons.h"
#include "jt_v113_screens.h"

#include "app_main_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mipi_cam.h"
#include "app_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_jt_cn_20_1.h"
#include "font_jt_b_24_2.h"
#include "font_jt_display_72_2.h"
#include "font_jt_lock_brand_104_2.h"
#include "font_jt_lock_time_132_2.h"
#include "font_jt_title_42_2.h"
#include "font_jt_text_28_2.h"
#include "font_jt_small_22_2.h"
#include "font_jt_memo_48_2.h"
#include "jt_business.h"

#ifndef CONFIG_JT_FINDER_SELFTEST
#define CONFIG_JT_FINDER_SELFTEST 0
#endif

#ifndef CONFIG_JT_UI_SNAPSHOT_TEST
#define CONFIG_JT_UI_SNAPSHOT_TEST 0
#endif

#ifndef CONFIG_JT_UI_CAPTURE_TOUR
#define CONFIG_JT_UI_CAPTURE_TOUR 0
#endif

#define JT_SCREEN_COUNT 8

#define JT_UI_W 1024
#define JT_UI_H 600

#define JT_SWIPE_MIN_DX 120
#define JT_SWIPE_MAX_DY 90
#define JT_SWIPE_MAX_MS 1600

#define JT_BTN_CAMERA LV_SYMBOL_IMAGE
#define JT_BTN_SHUTTER LV_SYMBOL_IMAGE

#define JT_GEAR_W 55
#define JT_GEAR_H 55
#define JT_GEAR_LOCK_X 934
#define JT_GEAR_LOCK_Y 14
#define JT_GEAR_HOME_X 946
#define JT_GEAR_HOME_Y 10

#define JT_GEAR_ICON_PAD 6

#define JT_S2_CARD_Y       122
#define JT_S2_CARD_H       128
#define JT_S2_SUB_Y        258
#define JT_S2_TITLE_Y      338
#define JT_S2_TITLE_H      110
#define JT_S2_CAM_X        380
#define JT_S2_CAM_Y        452
#define JT_S2_CAM_W        138
#define JT_S2_CAM_H        138

#define JT_S3_BTN_W        138
#define JT_S3_BTN_H        138
#define JT_S3_PREVIEW_X    16
#define JT_S3_PREVIEW_Y    14
#define JT_S3_PREVIEW_W    992
#define JT_S3_PREVIEW_H    440

#define JT_S4_CLOCK_Y      66
#define JT_S4_CLOCK_H      138
#define JT_S4_TITLE_Y      231
#define JT_S4_TITLE_H      100
#define JT_S4_BAR_X        35
#define JT_S4_BAR_Y        374
#define JT_S4_BAR_W        952
#define JT_S4_BAR_H        26
#define JT_S4_STAGE_Y      427
#define JT_S4_STAGE_W      183
#define JT_S4_STAGE_H      150

#define JT_S5_HEAD_X       37
#define JT_S5_HEAD_W       138
#define JT_S5_HEAD_H       138
#define JT_S5_TITLE_X      206
#define JT_S5_TITLE_W      552
#define JT_S5_CARD_X       755
#define JT_S5_CARD_W       232
#define JT_S5_CARD_H       84
#define JT_S5_BOX_X        35
#define JT_S5_BOX_Y        166
#define JT_S5_BOX_W        952
#define JT_S5_BOX_H        330

#define JT_S7_NET_X        35
#define JT_S7_NET_W        471
#define JT_S7_NET_Y        161
#define JT_S7_NET_H        210
#define JT_S7_SRV_X        518
#define JT_S7_SRV_W        471
#define JT_S7_CHAIN_X      35
#define JT_S7_CHAIN_Y      383
#define JT_S7_CHAIN_W      956
#define JT_S7_CHAIN_H      170

enum {
    JT_SCREEN_LOCK = 0,
    JT_SCREEN_HOME,
    JT_SCREEN_FINDER,
    JT_SCREEN_PROCESS,
    JT_SCREEN_IDEA,
    JT_SCREEN_RESULT,
    JT_SCREEN_SETTINGS,
    JT_SCREEN_CLAIM,
};

static const char *JT_UI_TAG = "JT_UI";
static bool s_jt_process_frozen;

typedef void (*jt_lock_screen_enter_cb_t)(void *ctx);
static jt_lock_screen_enter_cb_t s_lock_screen_enter_cb;
static void *s_lock_screen_enter_ctx;

typedef enum {
    JT_FINDER_LIVE = 0,
    JT_FINDER_FROZEN,
} jt_finder_state_t;

static void jt_apply_camera_frame(void *buffer, uint32_t width,
                                  uint32_t height, void *ctx);
static void jt_go_finder(lv_event_t *e);

typedef struct {
    lv_obj_t *root;
    lv_obj_t *pages[JT_SCREEN_COUNT];

    lv_obj_t *finder_preview;
    lv_img_dsc_t finder_preview_dsc;
    lv_obj_t *finder_status;
    lv_obj_t *finder_back_btn;
    lv_obj_t *finder_confirm_btn;
    lv_obj_t *finder_confirm_icon;
    lv_obj_t *finder_shutter_btn;
    lv_obj_t *frozen_caption;

    lv_obj_t *lock_time;
    lv_obj_t *lock_date;
    lv_obj_t *lock_week;
    lv_obj_t *lock_temp_label;
    lv_obj_t *lock_temp;
    lv_obj_t *lock_memo;

    lv_obj_t *home_card;

    lv_obj_t *process_clock;
    lv_obj_t *process_bar;
    lv_obj_t *process_bar_fill;
    lv_obj_t *process_stage[5];
    lv_obj_t *process_stage_num[5];
    lv_obj_t *process_stage_text[5];

    lv_obj_t *idea_title;
    lv_obj_t *idea_card;
    lv_obj_t *idea_verdict;
    lv_obj_t *idea_steps;

    lv_obj_t *result_title;
    lv_obj_t *result_chain;
    lv_obj_t *result_answer;

    lv_obj_t *settings_ssid;
    lv_obj_t *settings_signal;
    lv_obj_t *settings_chain;
    lv_obj_t *settings_device_id;
    lv_obj_t *settings_server_btn[2];
    lv_obj_t *settings_server_radio[2];

    lv_obj_t *claim_balance;
    lv_obj_t *claim_advanced;
    lv_obj_t *claim_feedback;
    lv_obj_t *claim_send_btn;
    lv_obj_t *claim_withdraw_btn;

    lv_obj_t *softap_page;
    lv_obj_t *qr_page;

    uint8_t screen;
    uint8_t question_index;
    uint8_t question_count;
    bool answer_opened[32];
    bool server_official;
    bool softap_on;
    bool qr_shown;
    bool advanced_sent;
    char application_id[JT_MAX_ID_LEN];
    char device_id[32];
    jt_finder_state_t finder_state;

    lv_point_t touch_start;
    uint32_t touch_start_ms;
    bool touch_tracking;
} jt_ui_t;

static jt_ui_t s_jt;

/* v113 lock-screen ratios, measured from the 1024x600 reference image.
 * Digits: 338x104 px ink inside a 578x224 box.  Brand: 简 x=74..168,
 * 题 x=253..347 on the same 1024x600 canvas.  Keep UI geometry derived from
 * these ratios instead of trial-and-error font point increments.
 */
#define JT_LOCK_BRAND_BOX_X   35
#define JT_LOCK_BRAND_BOX_Y   80
#define JT_LOCK_BRAND_BOX_W   353
#define JT_LOCK_BRAND_BOX_H   219
#define JT_LOCK_TIME_BOX_X    412
#define JT_LOCK_TIME_BOX_Y    80
#define JT_LOCK_TIME_BOX_W    577
#define JT_LOCK_TIME_BOX_H    219
#define JT_LOCK_DATE_BOX_X    35
#define JT_LOCK_DATE_BOX_Y    322
#define JT_LOCK_DATE_BOX_W    353
#define JT_LOCK_DATE_BOX_H    242
#define JT_LOCK_MEMO_BOX_X    412
#define JT_LOCK_MEMO_BOX_Y    322
#define JT_LOCK_MEMO_BOX_W    577
#define JT_LOCK_MEMO_BOX_H    242

static const char *const s_stage_labels[5] = {
    "上传", "网关", "大模型", "NLP", "下发",
};

static const char *const s_idea_steps[3] = {
    "→ 5 × (−2) 先算出来，是 −10",
    "→ −(−4) 变成 +4，式子成了 (−3) + (−10) + 4",
    "→ (−3) 加 (−10) 是 −13，剩下的 −13 + 4 接着算",
};

static const char *const s_result_chain[4] = {
    "(−3) + 5 × (−2) − (−4)",
    "= (−3) + (−10) + 4",
    "= −13 + 4",
    "= −9",
};

static const jt_question_t *jt_current_question(void)
{
    const jt_business_solve_t *solve = jt_business_solve_cached();
    if (!solve || s_jt.question_index >= (uint8_t)solve->question_count) {
        return NULL;
    }
    return &solve->questions[s_jt.question_index];
}

static lv_color_t jt_ink(void)
{
    return lv_color_hex(0x111211);
}

static void jt_pointer_cb(lv_event_t *e);
static void jt_set_finder_state(jt_finder_state_t state);
static void jt_go_settings(lv_event_t *e);
static void jt_go_idea(lv_event_t *e);

static lv_color_t jt_paper(void)
{
    return lv_color_hex(0xE9EBE7);
}

static lv_color_t jt_panel(void)
{
    return lv_color_hex(0xF4F3EE);
}

static lv_color_t jt_gray(void)
{
    return lv_color_hex(0x8A8A84);
}

static lv_color_t jt_subtitle(void)
{
    return lv_color_hex(0x4D4F4C);
}

static lv_color_t jt_blue(void)
{
    return lv_color_hex(0x274C8C);
}

static const lv_font_t *jt_font_body(void)
{
    return &font_jt_text_28_2;
}

static const lv_font_t *jt_font_bold(void)
{
    return &font_jt_title_42_2;
}

static const lv_font_t *jt_font_display(void)
{
    return &font_jt_display_72_2;
}

static const lv_font_t *jt_font_lock_brand(void)
{
    return &font_jt_lock_brand_104_2;
}

static const lv_font_t *jt_font_lock_time(void)
{
    return &font_jt_lock_time_132_2;
}

static const lv_font_t *jt_font_small(void)
{
    return &font_jt_small_22_2;
}

static const lv_font_t *jt_font_memo(void)
{
    return &font_jt_memo_48_2;
}

static const lv_font_t *jt_spec_brand(void) { return &font_jt_spec_brand_102; }
static const lv_font_t *jt_spec_time(void) { return &font_jt_spec_time_134; }
static const lv_font_t *jt_spec_date(void) { return &font_jt_spec_date_53; }
static const lv_font_t *jt_spec_week(void) { return &font_jt_spec_week_34; }
static const lv_font_t *jt_spec_temp(void) { return &font_jt_spec_temp_45; }
static const lv_font_t *jt_spec_label(void) { return &font_jt_spec_label_33; }
static const lv_font_t *jt_spec_memo(void) { return &font_jt_spec_memo_46; }
static const lv_font_t *jt_spec_sub(void) { return &font_jt_spec_sub_27; }

static void jt_set_no_scroll(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

static void jt_set_label_multiline(lv_obj_t *label, const char *const *lines, int count)
{
    char buffer[512];
    int used = 0;

    if (!label) {
        return;
    }
    for (int i = 0; i < count; i++) {
        const char *prefix = i == 0 ? "" : "\n";
        size_t prefix_len = strlen(prefix);
        size_t line_len = strlen(lines[i]);

        if (used + prefix_len + line_len + 1 > sizeof(buffer)) {
            break;
        }
        if (!lines[i]) {
            continue;
        }
        memcpy(buffer + used, prefix, prefix_len);
        used += prefix_len;
        memcpy(buffer + used, lines[i], line_len);
        used += line_len;
    }
    buffer[used] = '\0';
    lv_label_set_text(label, buffer);
}

static void jt_style_box(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, jt_panel(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, jt_ink(), 0);
    lv_obj_set_style_border_width(obj, 3, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    jt_set_no_scroll(obj);
}

static void jt_set_label_wrap(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_obj_get_width(label));
}

static lv_obj_t *jt_create_rule(lv_obj_t *parent, int32_t x, int32_t y,
                                int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_size(rule, w, h);
    lv_obj_set_style_bg_color(rule, color, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    jt_set_no_scroll(rule);
    return rule;
}

static lv_obj_t *jt_create_page(uint8_t index, lv_color_t bg)
{
    lv_obj_t *page = lv_obj_create(s_jt.root);
    s_jt.pages[index] = page;
    lv_obj_set_size(page, JT_UI_W, JT_UI_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    jt_set_no_scroll(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (index == JT_SCREEN_LOCK || index == JT_SCREEN_HOME) {
        lv_obj_add_event_cb(page, jt_pointer_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(page, jt_pointer_cb, LV_EVENT_RELEASED, NULL);
    }
    return page;
}

static lv_obj_t *jt_create_label(lv_obj_t *parent, const char *text,
                                 int32_t x, int32_t y, int32_t w, int32_t h,
                                 const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_pad_all(label, 0, 0);
    jt_set_no_scroll(label);
    return label;
}

static lv_obj_t *jt_create_button(lv_obj_t *parent, const char *text,
                                  int32_t x, int32_t y, int32_t w, int32_t h,
                                  lv_event_cb_t cb, void *user_data, bool filled)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_border_color(btn, jt_ink(), 0);
    lv_obj_set_style_bg_color(btn, filled ? jt_ink() : jt_panel(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, jt_font_body(), 0);
    lv_obj_set_style_text_color(label, filled ? jt_panel() : jt_ink(), 0);
    lv_obj_center(label);
    lv_obj_set_style_pad_all(label, 0, 0);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t *jt_create_symbol_button(lv_obj_t *parent, const char *symbol,
                                        int32_t x, int32_t y, int32_t size,
                                        lv_color_t fg, lv_color_t bg,
                                        lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_border_color(btn, jt_ink(), 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, jt_font_body(), 0);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_center(label);
    lv_obj_set_style_pad_all(label, 0, 0);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static void jt_set_label_textf(lv_obj_t *label, const char *fmt, const char *value)
{
    char text[JT_MAX_ANSWER_TEXT_LEN];
    if (!label || !fmt || !value) {
        return;
    }
    snprintf(text, sizeof(text), fmt, value);
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *jt_create_icon_button(lv_obj_t *parent, const lv_img_dsc_t *icon,
                                       int32_t x, int32_t y, int32_t size,
                                       lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_set_size(img, size, size);
    lv_obj_center(img);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t *jt_create_gear_button(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    /* Use the prototype button bitmap directly. The icon-only approach
     * loses shape at this physical size, so the whole button is an image. */
    lv_obj_set_size(btn, 64, 64);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, &jt_icon_gear_button);
    lv_obj_set_size(img, 64, 64);
    lv_obj_center(img);
    lv_obj_add_event_cb(btn, jt_go_settings, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *jt_create_head_icon_button(lv_obj_t *parent, const lv_img_dsc_t *icon,
                                            int32_t x, int32_t y, int32_t size,
                                            lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, jt_panel(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_set_size(img, size, size);
    lv_obj_center(img);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *jt_create_corner_image_button(lv_obj_t *parent,
                                               const lv_img_dsc_t *icon,
                                               int32_t x, int32_t y,
                                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    jt_set_no_scroll(btn);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_set_size(img, 64, 64);
    lv_obj_center(img);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/* The prototype bitmaps already draw every static icon. In prototype mode
 * the original controls are hidden, so these invisible objects restore the
 * touch targets without changing the rendered image. */
static lv_obj_t *jt_create_hotspot(lv_obj_t *parent, int32_t x, int32_t y,
                                   int32_t w, int32_t h, lv_event_cb_t cb,
                                   void *user_data)
{
    lv_obj_t *hot = lv_obj_create(parent);
    lv_obj_set_pos(hot, x, y);
    lv_obj_set_size(hot, w, h);
    lv_obj_set_style_bg_opa(hot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hot, 0, 0);
    lv_obj_set_style_pad_all(hot, 0, 0);
    lv_obj_set_style_radius(hot, 0, 0);
    jt_set_no_scroll(hot);
    lv_obj_add_flag(hot, LV_OBJ_FLAG_CLICKABLE);
    if (cb) {
        lv_obj_add_event_cb(hot, cb, LV_EVENT_CLICKED, user_data);
    }
    return hot;
}

static uint8_t jt_card_balance(void)
{
    const jt_business_card_t *card = jt_business_cards_cached();
    if (!card || card->balance < 0) {
        return 5;
    }
    return (uint8_t)card->balance;
}

static uint8_t jt_advanced_count(void)
{
    const jt_business_card_t *card = jt_business_cards_cached();
    if (!card || card->advanced < 0) {
        return 0;
    }
    return (uint8_t)card->advanced;
}

static void jt_update_clock_labels(void)
{
    char time_text[16];
    char date_text[32];
    char week_text[16];
    static const char *const week_cn[7] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };

    rtc_get_time();
    snprintf(time_text, sizeof(time_text), "%02u:%02u",
             (unsigned)calendar.hour, (unsigned)calendar.min);
    snprintf(date_text, sizeof(date_text), "%u月%u日",
             (unsigned)calendar.month, (unsigned)calendar.date);
    snprintf(week_text, sizeof(week_text), "%s",
             week_cn[calendar.week % 7]);
    if (s_jt.lock_time) {
        lv_label_set_text(s_jt.lock_time, time_text);
    }
    if (s_jt.process_clock) {
        lv_label_set_text(s_jt.process_clock, time_text);
    }
    if (s_jt.lock_date) {
        lv_label_set_text(s_jt.lock_date, date_text);
    }
    if (s_jt.lock_week) {
        lv_label_set_text(s_jt.lock_week, week_text);
    }
}

static void jt_update_environment_labels(void)
{
    const jt_business_environment_t *env = jt_business_environment_cached();
    if (!env) {
        return;
    }
    jt_set_label_textf(s_jt.lock_temp, "%s",
                       env->temperature[0] ? env->temperature : "---°C");
    jt_set_label_textf(s_jt.lock_memo, "%s",
                       env->memo[0] ? env->memo : "");
}

void app_jt_ui_set_environment(const char *temperature, const char *memo)
{
    char temp_copy[JT_MAX_TEXT_LEN] = {0};
    char memo_copy[JT_MAX_ANSWER_TEXT_LEN] = {0};
    if (temperature) {
        snprintf(temp_copy, sizeof(temp_copy), "%s", temperature);
    }
    if (memo) {
        snprintf(memo_copy, sizeof(memo_copy), "%s", memo);
    }
    jt_business_environment_set(temp_copy[0] ? temp_copy : "---°C", memo_copy);
    jt_update_environment_labels();
}

void app_jt_ui_set_clock(const char *hhmm, const char *date_md, const char *week)
{
    if (hhmm && s_jt.lock_time) {
        lv_label_set_text(s_jt.lock_time, hhmm);
    }
    if (date_md && s_jt.lock_date) {
        lv_label_set_text(s_jt.lock_date, date_md);
    }
    if (week && s_jt.lock_week) {
        lv_label_set_text(s_jt.lock_week, week);
    }
}

static void jt_update_cards(void)
{
    char text[96];
    uint8_t balance = jt_card_balance();

    if (s_jt.home_card) {
        snprintf(text, sizeof(text), "%u / 5 张", (unsigned)balance);
        lv_label_set_text(s_jt.home_card, text);
    }
    if (s_jt.idea_card) {
        snprintf(text, sizeof(text), "解题卡 %u张", (unsigned)balance);
        lv_label_set_text(s_jt.idea_card, text);
    }
    if (s_jt.claim_balance) {
        snprintf(text, sizeof(text), "今日剩余 %u 张", (unsigned)balance);
        lv_label_set_text(s_jt.claim_balance, text);
    }
    /* claim_advanced doubles as the action feedback label; do not overwrite it. */
}

static void jt_update_question(void)
{
    char text[96];
    char body[JT_MAX_THINKING_LEN + JT_MAX_ANSWER_LEN + 16];
    const jt_business_solve_t *solve = jt_business_solve_cached();
    const jt_question_t *question;
    uint8_t number = (uint8_t)(s_jt.question_index + 1);

    if (solve && solve->question_count > 0) {
        s_jt.question_count = (uint8_t)solve->question_count;
    }
    if (s_jt.idea_title) {
        snprintf(text, sizeof(text), "共%u题第%u题 · 思路",
                 (unsigned)s_jt.question_count, (unsigned)number);
        lv_label_set_text(s_jt.idea_title, text);
    }
    if (s_jt.result_title) {
        snprintf(text, sizeof(text), "共%u题第%u题 · 结果",
                 (unsigned)s_jt.question_count, (unsigned)number);
        lv_label_set_text(s_jt.result_title, text);
    }

    question = jt_current_question();
    if (s_jt.idea_verdict) {
        if (!question) {
            lv_label_set_text(s_jt.idea_verdict, "暂无解题思路");
        } else if (question->correct) {
            lv_label_set_text(s_jt.idea_verdict, "这个题做对了");
        } else {
            lv_label_set_text(s_jt.idea_verdict, "这个题做错了");
        }
    }
    if (s_jt.idea_steps) {
        if (question && question->thinking[0]) {
            snprintf(body, sizeof(body), "%s", question->thinking);
        } else {
            snprintf(body, sizeof(body), "请重新拍题，或等待判题完成后查看思路。");
        }
        lv_label_set_text(s_jt.idea_steps, body);
        jt_set_label_wrap(s_jt.idea_steps);
    }
    if (s_jt.result_chain) {
        if (question && question->answer[0]) {
            snprintf(body, sizeof(body), "%s", question->answer);
        } else {
            snprintf(body, sizeof(body), "暂无可显示答案，请重新拍题。");
        }
        lv_label_set_text(s_jt.result_chain, body);
        jt_set_label_wrap(s_jt.result_chain);
    }
    if (s_jt.idea_card) {
        bool opened = s_jt.question_index < 32 && s_jt.answer_opened[s_jt.question_index];
        if (opened) {
            lv_label_set_text(s_jt.idea_card, "已解锁");
        } else {
            jt_update_cards();
        }
    }
}

static void jt_clear_finder_preview(void)
{
    if (!s_jt.finder_preview) {
        return;
    }
    /* The preview descriptor lives in static storage and is reused by every
     * camera frame.  Clear the image cache before blanking the widget so a
     * pending draw cannot decode a zeroed descriptor as an indexed image. */
    lv_img_cache_invalidate_src(&s_jt.finder_preview_dsc);
    lv_img_set_src(s_jt.finder_preview, NULL);
    memset(&s_jt.finder_preview_dsc, 0, sizeof(s_jt.finder_preview_dsc));
    s_jt.finder_preview_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    lv_obj_invalidate(s_jt.finder_preview);
}

static void jt_show_screen_raw(uint8_t index)
{
    if (index >= JT_SCREEN_COUNT || !s_jt.root) {
        return;
    }
    s_jt.screen = index;
    for (int i = 0; i < JT_SCREEN_COUNT; i++) {
        if (!s_jt.pages[i]) {
            continue;
        }
        if (i == index) {
            lv_obj_clear_flag(s_jt.pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_jt.pages[i]);
        } else {
            lv_obj_add_flag(s_jt.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (index == JT_SCREEN_IDEA || index == JT_SCREEN_RESULT) {
        jt_update_question();
    }
    if (index == JT_SCREEN_FINDER) {
        jt_set_finder_state(JT_FINDER_LIVE);
        mipi_cam_set_frame_callback(jt_apply_camera_frame, NULL);
        if (s_jt.finder_status) {
            lv_obj_clear_flag(s_jt.finder_status, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        mipi_cam_set_frame_callback(NULL, NULL);
        jt_clear_finder_preview();
        if (s_jt.finder_status) {
            lv_obj_add_flag(s_jt.finder_status, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (index == JT_SCREEN_HOME || index == JT_SCREEN_IDEA) {
        jt_update_cards();
    }
    if (index == JT_SCREEN_SETTINGS) {
        /* Config refresh is a blocking HTTP call; do it only when the user
         * explicitly taps 连通测试 instead of freezing the settings page. */
    }
    if (s_jt.softap_page) {
        lv_obj_add_flag(s_jt.softap_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_jt.qr_page) {
        lv_obj_add_flag(s_jt.qr_page, LV_OBJ_FLAG_HIDDEN);
    }
    jt_update_clock_labels();
    lv_obj_invalidate(s_jt.root);
    lv_refr_now(NULL);
}

static void jt_show_screen(uint8_t index)
{
    jt_show_screen_raw(index);
    if (index == JT_SCREEN_LOCK && s_lock_screen_enter_cb) {
        s_lock_screen_enter_cb(s_lock_screen_enter_ctx);
    }
}

void app_jt_ui_set_lock_screen_enter_callback(jt_lock_screen_enter_cb_t cb,
                                              void *ctx)
{
    s_lock_screen_enter_cb = cb;
    s_lock_screen_enter_ctx = ctx;
}

void app_jt_ui_on_lock_screen_entered(void)
{
    if (s_lock_screen_enter_cb) {
        s_lock_screen_enter_cb(s_lock_screen_enter_ctx);
    }
}

static void jt_apply_camera_frame(void *buffer, uint32_t width,
                                  uint32_t height, void *ctx)
{
    (void)ctx;
    if (s_jt.screen != JT_SCREEN_FINDER || !s_jt.finder_preview ||
        !buffer || !width || !height) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_img_cache_invalidate_src(&s_jt.finder_preview_dsc);
    s_jt.finder_preview_dsc.data = buffer;
    s_jt.finder_preview_dsc.header.w = width;
    s_jt.finder_preview_dsc.header.h = height;
    s_jt.finder_preview_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_jt.finder_preview_dsc.data_size = (size_t)width * height * 2U;
    lv_img_set_src(s_jt.finder_preview, &s_jt.finder_preview_dsc);
    lv_obj_set_size(s_jt.finder_preview, width, height);
    lv_obj_invalidate(s_jt.finder_preview);
    lv_refr_now(NULL);
    if (s_jt.finder_status && lv_obj_has_flag(s_jt.finder_status, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_jt.finder_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_jt.finder_status, "摄像头预览中");
    }
    lvgl_port_unlock();
}

static bool jt_is_swipe_screen(uint8_t index)
{
    return index == JT_SCREEN_LOCK || index == JT_SCREEN_HOME;
}

static void jt_pointer_cb(lv_event_t *e)
{
    lv_indev_t *indev;
    lv_point_t point;
    int32_t dx;
    int32_t dy;
    uint32_t now;
    uint8_t next;

    if (!jt_is_swipe_screen(s_jt.screen)) {
        return;
    }
    indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_indev_get_point(indev, &point);
    now = lv_tick_get();

    if (e->code == LV_EVENT_PRESSED) {
        s_jt.touch_start = point;
        s_jt.touch_start_ms = now;
        s_jt.touch_tracking = true;
        return;
    }
    if (e->code != LV_EVENT_RELEASED || !s_jt.touch_tracking) {
        return;
    }
    s_jt.touch_tracking = false;
    dx = point.x - s_jt.touch_start.x;
    dy = point.y - s_jt.touch_start.y;
    if (abs(dx) < JT_SWIPE_MIN_DX || abs(dy) > JT_SWIPE_MAX_DY ||
        (now - s_jt.touch_start_ms) > JT_SWIPE_MAX_MS) {
        return;
    }

    if (s_jt.screen == JT_SCREEN_LOCK) {
        next = JT_SCREEN_HOME;
    } else {
        next = JT_SCREEN_LOCK;
    }
    ESP_LOGI(JT_UI_TAG, "swipe dx=%ld dy=%ld screen %u -> %u",
             (long)dx, (long)dy, s_jt.screen, next);
    jt_show_screen(next);
}

static void jt_go_home(lv_event_t *e)
{
    (void)e;
    jt_show_screen(JT_SCREEN_HOME);
}

static void jt_go_settings(lv_event_t *e)
{
    (void)e;
    jt_show_screen(JT_SCREEN_SETTINGS);
}

#if CONFIG_JT_UI_CAPTURE_TOUR
static void jt_capture_tour_timer_cb(lv_timer_t *timer)
{
    static uint8_t step;
    static const uint8_t order[] = {
        JT_SCREEN_LOCK, JT_SCREEN_HOME, JT_SCREEN_FINDER,
        JT_SCREEN_PROCESS, JT_SCREEN_IDEA, JT_SCREEN_RESULT,
        JT_SCREEN_SETTINGS, JT_SCREEN_CLAIM,
    };
    (void)timer;
    if (step >= sizeof(order)) {
        return;
    }
    ESP_LOGI(JT_UI_TAG, "capture tour %u -> %u", (unsigned)step,
             (unsigned)order[step]);
    jt_show_screen_raw(order[step]);
    step++;
}

#endif

void app_jt_ui_debug_show_page(int page)
{
    if (page < 0 || page >= JT_SCREEN_COUNT || !s_jt.root) {
        return;
    }
    s_jt_process_frozen = (page == JT_SCREEN_PROCESS);
    jt_show_screen_raw((uint8_t)page);
    if (page == JT_SCREEN_HOME) {
        jt_go_finder(NULL);
    }
    if (page == JT_SCREEN_IDEA) {
        jt_go_idea(NULL);
    }
    if (page == JT_SCREEN_SETTINGS) {
        ESP_LOGI(JT_UI_TAG, "debug settings: network_ready=%d",
                 (int)jt_client_network_ready());
    }
}

void app_jt_ui_debug_freeze_process(bool freeze)
{
    s_jt_process_frozen = freeze;
}

void app_jt_ui_debug_print_status(void)
{
    const jt_business_solve_t *solve = jt_business_solve_cached();
    printf("screen=%u frozen=%d stream=%d frame=%d solve_job=%s solve_result=%d solve_questions=%d\n",
           (unsigned)s_jt.screen,
           (int)s_jt_process_frozen,
           (int)camera_stream_ready,
           (int)preview_frame_ready,
           (solve && solve->job_id[0]) ? solve->job_id : "-",
           solve ? (int)solve->result : -1,
           solve ? solve->question_count : -1);
}

static void jt_go_finder(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(JT_UI_TAG, "finder start: camera_ready=%d", (int)camera_stream_ready);
    esp_err_t cam_err = mipi_cam_init();
    ESP_LOGI(JT_UI_TAG, "finder camera init err=0x%x ready=%d",
             (unsigned)cam_err, (int)camera_stream_ready);
    jt_set_finder_state(JT_FINDER_LIVE);
    jt_show_screen(JT_SCREEN_FINDER);
}

static void jt_reset_process_visual(void)
{
    for (int i = 0; i < 5; i++) {
        if (s_jt.process_stage[i]) {
            lv_obj_set_style_bg_color(s_jt.process_stage[i], jt_panel(), 0);
            lv_obj_set_style_text_color(s_jt.process_stage_text[i], jt_ink(), 0);
            lv_obj_set_style_text_color(s_jt.process_stage_num[i], jt_ink(), 0);
        }
    }
    if (s_jt.process_bar_fill) {
        lv_obj_set_width(s_jt.process_bar_fill, 0);
    }
}

static void jt_finder_shutter(lv_event_t *e)
{
    (void)e;
    if (s_jt.screen != JT_SCREEN_FINDER) {
        return;
    }
    if (!camera_stream_ready || !preview_frame_ready) {
        ESP_LOGW(JT_UI_TAG,
                 "finder shutter ignored: stream_ready=%d frame_ready=%d",
                 (int)camera_stream_ready, (int)preview_frame_ready);
        if (s_jt.finder_status) {
            lv_label_set_text(s_jt.finder_status, "摄像头启动中，请稍候");
        }
        return;
    }
    ESP_LOGI(JT_UI_TAG, "finder shutter -> process");
    jt_reset_process_visual();
    mipi_cam_request_photo();
    mipi_cam_set_frame_callback(NULL, NULL);
    jt_clear_finder_preview();
    if (s_jt.finder_status) {
        lv_label_set_text(s_jt.finder_status, "拍照中...");
    }
    jt_set_finder_state(JT_FINDER_LIVE);
    jt_show_screen(JT_SCREEN_PROCESS);
}

static void jt_finder_back(lv_event_t *e)
{
    (void)e;
    if (s_jt.screen != JT_SCREEN_FINDER) {
        return;
    }
    ESP_LOGI(JT_UI_TAG, "finder back: return to home");
    jt_show_screen(JT_SCREEN_HOME);
}

static void jt_go_process(lv_event_t *e)
{
    (void)e;
    jt_show_screen(JT_SCREEN_PROCESS);
}

static void jt_finder_send(lv_event_t *e)
{
    (void)e;
    if (s_jt.screen != JT_SCREEN_FINDER) {
        return;
    }
    if (!camera_stream_ready || !preview_frame_ready) {
        ESP_LOGW(JT_UI_TAG,
                 "finder confirm ignored: stream_ready=%d frame_ready=%d",
                 (int)camera_stream_ready, (int)preview_frame_ready);
        if (s_jt.finder_status) {
            lv_label_set_text(s_jt.finder_status, "摄像头启动中，请稍候");
        }
        return;
    }
    ESP_LOGI(JT_UI_TAG, "finder confirm -> process");
    jt_reset_process_visual();
    mipi_cam_request_photo();
    mipi_cam_set_frame_callback(NULL, NULL);
    jt_clear_finder_preview();
    if (s_jt.finder_status) {
        lv_label_set_text(s_jt.finder_status, "拍照中...");
    }
    jt_set_finder_state(JT_FINDER_LIVE);
    jt_show_screen(JT_SCREEN_PROCESS);
}

void app_jt_ui_debug_finder_shutter(void)
{
    jt_finder_shutter(NULL);
}

void app_jt_ui_debug_finder_confirm(void)
{
    jt_finder_send(NULL);
}

static void jt_set_finder_state(jt_finder_state_t state)
{
    bool frozen = state == JT_FINDER_FROZEN;

    s_jt.finder_state = state;
    if (s_jt.frozen_caption) {
        if (frozen) {
            lv_label_set_text(s_jt.frozen_caption, "拍摄结果");
            lv_obj_clear_flag(s_jt.frozen_caption, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_jt.frozen_caption);
        } else {
            lv_obj_add_flag(s_jt.frozen_caption, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_jt.finder_shutter_btn) {
        if (frozen) {
            lv_obj_add_state(s_jt.finder_shutter_btn, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_jt.finder_shutter_btn, jt_gray(), 0);
            lv_obj_set_style_bg_opa(s_jt.finder_shutter_btn, LV_OPA_60, 0);
        } else {
            lv_obj_clear_state(s_jt.finder_shutter_btn, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_jt.finder_shutter_btn, jt_panel(), 0);
            lv_obj_set_style_bg_opa(s_jt.finder_shutter_btn, LV_OPA_COVER, 0);
        }
    }
    if (s_jt.finder_confirm_btn && s_jt.finder_confirm_icon) {
        lv_img_set_src(s_jt.finder_confirm_icon,
                       frozen ? (const void *)&jt_icon_send
                              : (const void *)&jt_icon_confirm);
        if (frozen) {
            lv_obj_set_style_bg_color(s_jt.finder_confirm_btn, jt_ink(), 0);
            lv_obj_set_style_border_color(s_jt.finder_confirm_btn, jt_ink(), 0);
        } else {
            lv_obj_set_style_bg_color(s_jt.finder_confirm_btn, lv_color_hex(0x191919), 0);
            lv_obj_set_style_border_color(s_jt.finder_confirm_btn, lv_color_hex(0x191919), 0);
        }
    }
}

static void jt_go_idea(lv_event_t *e)
{
    (void)e;
    const jt_question_t *question = jt_current_question();
    const jt_business_solve_t *solve = jt_business_solve_cached();
    if (question) {
        jt_business_card_t card = jt_business_cards_refresh(
            question->question_id,
            solve && solve->job_id[0] ? solve->job_id : question->job_id);
        ESP_LOGI(JT_UI_TAG, "idea cards refresh: result=%d balance=%d unlocked=%d",
                 (int)card.result, card.balance, (int)card.unlocked);
        if (card.result == JT_RESULT_OK) {
            s_jt.answer_opened[s_jt.question_index] = card.unlocked;
            jt_update_cards();
        }
    }
    jt_show_screen(JT_SCREEN_IDEA);
}

static void jt_go_result(lv_event_t *e)
{
    (void)e;
    if (s_jt.question_index < 32) {
        s_jt.answer_opened[s_jt.question_index] = true;
    }
    jt_show_screen(JT_SCREEN_RESULT);
}

static void jt_use_card(lv_event_t *e)
{
    (void)e;
    const jt_question_t *question = jt_current_question();
    const jt_business_solve_t *solve = jt_business_solve_cached();
    const char *job_id;
    jt_business_card_t refreshed;
    jt_business_card_t card;
    if (!question) {
        return;
    }
    job_id = solve && solve->job_id[0] ? solve->job_id : question->job_id;
    refreshed = jt_business_cards_refresh(
        question->question_id,
        job_id);
    ESP_LOGI(JT_UI_TAG, "use card refresh: result=%d balance=%d unlocked=%d",
             (int)refreshed.result, refreshed.balance, (int)refreshed.unlocked);
    if (refreshed.result == JT_RESULT_OK && refreshed.unlocked) {
        s_jt.answer_opened[s_jt.question_index] = true;
        jt_update_cards();
        jt_go_result(e);
        return;
    }
    if (refreshed.result == JT_RESULT_OK && refreshed.balance <= 0 &&
        !refreshed.can_deduct) {
        jt_show_screen(JT_SCREEN_CLAIM);
        return;
    }
    /* A card balance means the answer must be unlocked now. A stale or failed
     * refresh must never turn the primary action into a claim-page shortcut. */
    card = jt_business_answer_unlock(question->question_id, "", job_id);
    ESP_LOGI(JT_UI_TAG, "unlock card: result=%d http=%d balance=%d can_deduct=%d",
             (int)card.result, card.http_status, card.balance, (int)card.can_deduct);
    if (card.result != JT_RESULT_OK) {
        ESP_LOGW(JT_UI_TAG, "unlock card failed: %s",
                 card.message[0] ? card.message : "unknown");
        if (card.http_status == 402 || (card.balance <= 0 && !card.can_deduct)) {
            jt_show_screen(JT_SCREEN_CLAIM);
        }
        jt_update_cards();
        return;
    }
    s_jt.answer_opened[s_jt.question_index] = true;
    jt_update_cards();
    jt_go_result(e);
}

static void jt_prev_question(lv_event_t *e)
{
    (void)e;
    if (s_jt.question_index == 0) {
        s_jt.question_index = (uint8_t)(s_jt.question_count - 1);
    } else {
        s_jt.question_index--;
    }
    jt_update_question();
}

static void jt_next_question(lv_event_t *e)
{
    (void)e;
    s_jt.question_index++;
    if (s_jt.question_index >= s_jt.question_count) {
        s_jt.question_index = 0;
    }
    jt_update_question();
}

static void jt_show_softap(bool show)
{
    if (!s_jt.softap_page) {
        return;
    }
    s_jt.softap_on = show;
    if (show) {
        wifi_config_t cfg = {0};
        char softap_ssid[32];
        size_t softap_len;
        snprintf(softap_ssid, sizeof(softap_ssid), "JianTi-%.24s", s_jt.device_id);
        softap_len = strnlen(softap_ssid, sizeof(cfg.ap.ssid));
        memcpy(cfg.ap.ssid, softap_ssid, softap_len);
        cfg.ap.ssid_len = softap_len;
        cfg.ap.channel = 1;
        cfg.ap.max_connection = 4;
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        wifi_module_init();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &cfg);
        esp_wifi_start();
        ESP_LOGI(JT_UI_TAG, "softap started: %s", softap_ssid);
        lv_obj_clear_flag(s_jt.softap_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_jt.softap_page);
    } else {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        wifi_mode_t mode = WIFI_MODE_MAX;
        esp_err_t stop_err = ESP_ERR_INVALID_STATE;
        esp_err_t mode_err = ESP_ERR_INVALID_STATE;
        wifi_mode_t target_mode = WIFI_MODE_STA;
        bool sta_should_restart = jt_client_network_ready();
        if (ap_netif) {
            stop_err = esp_netif_dhcps_stop(ap_netif);
            ESP_LOGI(JT_UI_TAG, "softap dhcps stop err=0x%x", (unsigned)stop_err);
        } else {
            ESP_LOGW(JT_UI_TAG, "softap ap netif missing on stop");
        }
        /* Keep the STA interface running while disabling only the AP. */
        mode_err = esp_wifi_set_mode(target_mode);
        esp_wifi_get_mode(&mode);
        ESP_LOGI(JT_UI_TAG,
                 "softap stopped: dhcps=0x%x mode_set=0x%x mode_now=%d(STA=%d) sta_restart=%d",
                 (unsigned)stop_err, (unsigned)mode_err, (int)mode,
                 (int)WIFI_MODE_STA, (int)sta_should_restart);
        if (mode_err != ESP_OK) {
            /* Legacy fallback: a few remote Wi-Fi builds only apply an AP stop
             * through a full stop/start cycle. Reconnect STA afterwards. */
            ESP_LOGW(JT_UI_TAG, "softap direct mode switch failed, restart wifi");
            esp_wifi_stop();
            mode_err = esp_wifi_set_mode(target_mode);
            esp_wifi_get_mode(&mode);
            esp_wifi_start();
            ESP_LOGI(JT_UI_TAG,
                     "softap stopped fallback: mode_set=0x%x mode_now=%d(STA=%d)",
                     (unsigned)mode_err, (int)mode, (int)WIFI_MODE_STA);
            if (sta_should_restart) {
                esp_wifi_connect();
            }
        }
        lv_obj_add_flag(s_jt.softap_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void jt_show_qr(bool show)
{
    if (!s_jt.qr_page) {
        return;
    }
    s_jt.qr_shown = show;
    if (show) {
        lv_obj_clear_flag(s_jt.qr_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_jt.qr_page);
    } else {
        lv_obj_add_flag(s_jt.qr_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void jt_open_softap(lv_event_t *e)
{
    (void)e;
    jt_show_softap(true);
}

static void jt_close_softap(lv_event_t *e)
{
    (void)e;
    jt_show_softap(false);
}

static void jt_open_qr(lv_event_t *e)
{
    (void)e;
    jt_show_qr(true);
}

static void jt_close_qr(lv_event_t *e)
{
    (void)e;
    jt_show_qr(false);
}

static void jt_advance_card(lv_event_t *e)
{
    (void)e;
    jt_business_card_t card = jt_business_cards_advance();
    if (s_jt.claim_advanced) {
        char text[JT_MAX_ERROR_LEN + 32];
        if (card.result == JT_RESULT_OK) {
            snprintf(text, sizeof(text), "预支成功，今日剩余 %d 张", card.balance);
        } else {
            snprintf(text, sizeof(text), "预支失败：%s",
                     card.message[0] ? card.message : "请稍后重试");
        }
        lv_label_set_text(s_jt.claim_advanced, text);
    }
    jt_update_cards();
}

static void jt_ask_card(lv_event_t *e)
{
    (void)e;
    const jt_question_t *question = jt_current_question();
    jt_business_action_t action = jt_business_application_create(
        question && question->question_id[0] ? question->question_id : NULL,
        "申请解题卡");
    s_jt.advanced_sent = action.result == JT_RESULT_OK;
    if (s_jt.advanced_sent) {
        snprintf(s_jt.application_id, sizeof(s_jt.application_id), "%s",
                 action.application_id);
    }
    if (s_jt.claim_feedback) {
        if (s_jt.advanced_sent) {
            lv_label_set_text(s_jt.claim_feedback, "申请已发送，可撤回");
        } else {
            lv_label_set_text(s_jt.claim_feedback, "申请失败，请稍后重试");
        }
    }
    if (s_jt.claim_withdraw_btn) {
        if (s_jt.advanced_sent) {
            lv_obj_clear_flag(s_jt.claim_withdraw_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_jt.claim_withdraw_btn, 545, 478);
            lv_obj_set_size(s_jt.claim_withdraw_btn, 416, 55);
        } else {
            lv_obj_add_flag(s_jt.claim_withdraw_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_jt.claim_send_btn) {
        lv_obj_set_pos(s_jt.claim_send_btn, 27, 111);
    }
}

static void jt_withdraw_card_request(lv_event_t *e)
{
    (void)e;
    if (!s_jt.advanced_sent || !s_jt.application_id[0]) {
        return;
    }
    jt_business_action_t action =
        jt_business_application_withdraw(s_jt.application_id);
    if (action.result == JT_RESULT_OK) {
        s_jt.advanced_sent = false;
        s_jt.application_id[0] = '\0';
        if (s_jt.claim_feedback) {
            lv_label_set_text(s_jt.claim_feedback, "申请已撤回");
        }
        if (s_jt.claim_withdraw_btn) {
            lv_obj_add_flag(s_jt.claim_withdraw_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_jt.claim_feedback) {
        lv_label_set_text(s_jt.claim_feedback, "撤回失败，请稍后重试");
    }
}

static void jt_run_chain_test(lv_event_t *e)
{
    (void)e;
    char chain_label[128];
    if (s_jt.settings_chain) {
        lv_obj_clear_flag(s_jt.settings_chain, LV_OBJ_FLAG_HIDDEN);
        bool wifi_ok = jt_client_network_ready();
        bool internet_ok = jt_client_internet_reachable(3000);
        bool server_ok = jt_client_server_reachable(3000);
        wifi_mode_t mode = WIFI_MODE_MAX;
        esp_wifi_get_mode(&mode);
        ESP_LOGI(JT_UI_TAG, "chain test: wifi=%d internet=%d server=%d",
                 (int)wifi_ok, (int)internet_ok, (int)server_ok);
        ESP_LOGI(JT_UI_TAG,
                 "chain state: wifi_mode=%d(APSTA=%d) softap_page=%d wifi=%d "
                 "internet=%d server=%d",
                 (int)mode, (int)WIFI_MODE_APSTA, (int)s_jt.softap_on,
                 (int)wifi_ok, (int)internet_ok, (int)server_ok);
        snprintf(chain_label, sizeof(chain_label),
                 "① WiFi %s → ② 互联网 %s → ③ 服务器 %s",
                 wifi_ok ? "OK" : "FAIL",
                 internet_ok ? "OK" : "FAIL",
                 server_ok ? "OK" : "FAIL");
        lv_label_set_text(s_jt.settings_chain, chain_label);
    }
}

void app_jt_ui_debug_softap(bool start)
{
    if (start == s_jt.softap_on) {
        ESP_LOGI(JT_UI_TAG, "debug softap state already start=%d", (int)start);
        return;
    }
    jt_show_softap(start);
}

void app_jt_ui_debug_chain_test(void)
{
    jt_run_chain_test(NULL);
}

void app_jt_ui_debug_ap_state(void)
{
    wifi_mode_t mode = WIFI_MODE_MAX;
    esp_err_t err = esp_wifi_get_mode(&mode);
    ESP_LOGI(JT_UI_TAG, "ap state: err=0x%x mode=%d ap_active=%d softap_page=%d",
             (unsigned)err, (int)mode,
             (int)((mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
                   s_jt.softap_on),
             (int)s_jt.softap_on);
}

static void jt_select_server(lv_event_t *e)
{
    uintptr_t official = (uintptr_t)e->user_data;
    int selected = official != 0 ? 1 : 0;
    s_jt.server_official = official != 0;
    for (int i = 0; i < 2; i++) {
        if (s_jt.settings_server_btn[i]) {
            lv_obj_set_style_bg_color(s_jt.settings_server_btn[i],
                                      i == selected ? jt_ink() : jt_panel(), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_jt.settings_server_btn[i], 0),
                                        i == selected ? jt_panel() : jt_ink(), 0);
        }
        if (s_jt.settings_server_radio[i]) {
            lv_obj_set_style_bg_color(s_jt.settings_server_radio[i],
                                      i == selected ? jt_ink() : jt_panel(), 0);
            lv_obj_set_style_border_color(s_jt.settings_server_radio[i], jt_ink(), 0);
            lv_obj_set_style_border_width(s_jt.settings_server_radio[i], 3, 0);
        }
    }
}

static void jt_process_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_jt_process_frozen) {
        return;
    }
    if (s_jt.screen != JT_SCREEN_PROCESS) {
        return;
    }
    /* PRD flow: stay on the processing screen until the real solve callback
     * finishes. The five stages remain the prototype visual; they must not
     * fake completion or force a jump while the request is still pending. */
    const jt_business_solve_t *solve = jt_business_solve_cached();
    if (!solve || solve->job_id[0] == 0) {
        return;
    }
    if (solve->result == JT_RESULT_OK && solve->question_count > 0) {
        jt_show_screen(JT_SCREEN_IDEA);
        return;
    }
    if (solve->result != JT_RESULT_OK) {
        s_jt.finder_status = NULL;
        if (s_jt.process_stage_text[0]) {
            lv_label_set_text(s_jt.process_stage_text[0], jt_result_to_string(solve->result));
        }
    }
}

static void jt_hide_legacy_ui(void)
{
    if (lv_app_ui.main_cont && lv_obj_is_valid(lv_app_ui.main_cont)) {
        lv_obj_add_flag(lv_app_ui.main_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_app_ui.wallpaper_img && lv_obj_is_valid(lv_app_ui.wallpaper_img)) {
        lv_obj_add_flag(lv_app_ui.wallpaper_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_app_ui.app_main_ui.main_time_label && lv_obj_is_valid(lv_app_ui.app_main_ui.main_time_label)) {
        lv_obj_add_flag(lv_app_ui.app_main_ui.main_time_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_app_ui.app_small_ui.small_cont && lv_obj_is_valid(lv_app_ui.app_small_ui.small_cont)) {
        lv_obj_add_flag(lv_app_ui.app_small_ui.small_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_app_ui.app_small_ui.pull_down_panel && lv_obj_is_valid(lv_app_ui.app_small_ui.pull_down_panel)) {
        lv_obj_add_flag(lv_app_ui.app_small_ui.pull_down_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void jt_add_prototype_backgrounds(void)
{
    static const lv_img_dsc_t *const screen_images[JT_SCREEN_COUNT] = {
        &jt_v113_0, &jt_v113_1, &jt_v113_2, &jt_v113_3,
        &jt_v113_4, &jt_v113_5, &jt_v113_6, &jt_v113_7,
    };
    for (uint8_t i = 0; i < JT_SCREEN_COUNT; i++) {
        lv_obj_t *page = s_jt.pages[i];
        lv_obj_t *bg_img;
        if (!page) {
            continue;
        }
        /* 第3屏必须显示真实摄像头帧，不能铺原型静图；否则盖镜头/挪纸时
         * 看到的仍是固定照片。其他页面保留原型图作为底板。 */
        if (i == JT_SCREEN_FINDER) {
            bg_img = NULL;
        } else {
            bg_img = lv_img_create(page);
            lv_img_set_src(bg_img, screen_images[i]);
            lv_obj_set_pos(bg_img, 0, 0);
            lv_obj_set_size(bg_img, JT_UI_W, JT_UI_H);
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_move_background(bg_img);
        }

        /* Prototype bitmaps already contain all static text and artwork.
         * Keep only the live value labels and input hot spots visible. */
        for (int child_idx = 0; child_idx < (int)lv_obj_get_child_cnt(page); child_idx++) {
            lv_obj_t *child = lv_obj_get_child(page, child_idx);
            bool keep = child == bg_img ||
                        child == s_jt.home_card ||
                        child == s_jt.finder_preview ||
                        child == s_jt.finder_status ||
                        child == s_jt.lock_time || child == s_jt.lock_date ||
                        child == s_jt.lock_week || child == s_jt.lock_temp ||
                        child == s_jt.lock_memo ||
                        child == s_jt.process_bar_fill ||
                        child == s_jt.idea_verdict ||
                        child == s_jt.claim_balance || child == s_jt.claim_advanced ||
                        child == s_jt.claim_feedback;
            if (!keep) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    jt_create_hotspot(s_jt.pages[JT_SCREEN_LOCK], JT_GEAR_LOCK_X,
                      JT_GEAR_LOCK_Y, 64, 64, jt_go_settings, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_HOME], JT_GEAR_HOME_X,
                      JT_GEAR_HOME_Y, 64, 64, jt_go_settings, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_HOME], JT_S2_CAM_X, JT_S2_CAM_Y,
                      JT_S2_CAM_W, JT_S2_CAM_H, jt_go_finder, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_FINDER], 36, 452,
                      JT_S3_BTN_W, JT_S3_BTN_H, jt_finder_back, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_FINDER], 443, 452,
                      JT_S3_BTN_W, JT_S3_BTN_H, jt_finder_shutter, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_FINDER], 850, 452,
                      JT_S3_BTN_W, JT_S3_BTN_H, jt_finder_send, NULL);
    if (s_jt.finder_shutter_btn) {
        lv_obj_move_foreground(s_jt.finder_shutter_btn);
    }
    if (s_jt.finder_confirm_btn) {
        lv_obj_move_foreground(s_jt.finder_confirm_btn);
    }
    if (s_jt.finder_back_btn) {
        lv_obj_move_foreground(s_jt.finder_back_btn);
    }
    jt_create_hotspot(s_jt.pages[JT_SCREEN_IDEA], JT_S5_HEAD_X, 20,
                      JT_S5_HEAD_W, JT_S5_HEAD_H, jt_go_home, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_IDEA], JT_S5_CARD_X, 44,
                      JT_S5_CARD_W, JT_S5_CARD_H, jt_use_card, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_IDEA], 35, 512, 166, 84,
                      jt_prev_question, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_IDEA], 823, 512, 166, 84,
                      jt_next_question, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_RESULT], 42, 20, 64, 64,
                      jt_go_home, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_RESULT], 878, 20, 64, 64,
                      jt_go_idea, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS], 42, 20, 64, 64,
                      jt_go_home, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS],
                      JT_S7_NET_X + 27, JT_S7_NET_Y + 171, 416, 25,
                      jt_open_softap, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS],
                      JT_S7_SRV_X + 27, JT_S7_NET_Y + 68, 416, 68,
                      jt_select_server, (void *)1);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS],
                      JT_S7_SRV_X + 27, JT_S7_NET_Y + 144, 416, 68,
                      jt_select_server, (void *)0);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS],
                      JT_S7_CHAIN_X + 743, JT_S7_CHAIN_Y + 18, 184, 57,
                      jt_run_chain_test, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_SETTINGS],
                      JT_S7_CHAIN_X + 701, JT_S7_CHAIN_Y + 94, 226, 45,
                      jt_open_qr, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_CLAIM], 42, 20, 64, 64,
                      jt_go_home, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_CLAIM], 878, 20, 64, 64,
                      jt_go_idea, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_CLAIM], 27, 207, 416, 55,
                      jt_advance_card, NULL);
    jt_create_hotspot(s_jt.pages[JT_SCREEN_CLAIM], 518 + 27, 277 + 207, 416, 55,
                      jt_ask_card, NULL);
}

static void jt_build_lock_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_LOCK, jt_paper());
    lv_obj_t *box;

    jt_create_gear_button(page, JT_GEAR_LOCK_X, JT_GEAR_LOCK_Y);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_LOCK_BRAND_BOX_X, JT_LOCK_BRAND_BOX_Y);
    lv_obj_set_size(box, JT_LOCK_BRAND_BOX_W, JT_LOCK_BRAND_BOX_H);
    lv_obj_t *brand_ji = jt_create_label(box, "简", 33, 14, 100, 110,
                                         jt_spec_brand(), jt_ink());
    lv_obj_t *brand_ti = jt_create_label(box, "题", 212, 14, 100, 110,
                                         jt_spec_brand(), jt_ink());
    lv_obj_set_style_text_align(brand_ji, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(brand_ti, LV_TEXT_ALIGN_CENTER, 0);
    jt_create_rule(box, 33, 132, 277, 2, jt_gray());
    {
        static const char *const subtitle_glyphs[] = {
            "极", "简", "作", "业", "A", "I", "助", "手"
        };
        const int32_t subtitle_x = 33;
        const int32_t subtitle_w = 278;
        const int32_t cell_w = 34;
        for (int i = 0; i < 8; i++) {
            lv_obj_t *glyph = jt_create_label(box, subtitle_glyphs[i],
                                              subtitle_x + i * (subtitle_w - cell_w) / 7,
                                              140, cell_w, 44,
                                              jt_spec_sub(), jt_subtitle());
            lv_obj_set_style_text_align(glyph, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_LOCK_TIME_BOX_X, JT_LOCK_TIME_BOX_Y);
    lv_obj_set_size(box, JT_LOCK_TIME_BOX_W, JT_LOCK_TIME_BOX_H);
    s_jt.lock_time = jt_create_label(box, "21:51",
                                     11, 38, 555, 136,
                                     jt_spec_time(), jt_ink());
    lv_obj_set_style_text_align(s_jt.lock_time, LV_TEXT_ALIGN_CENTER, 0);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_LOCK_DATE_BOX_X, JT_LOCK_DATE_BOX_Y);
    lv_obj_set_size(box, JT_LOCK_DATE_BOX_W, JT_LOCK_DATE_BOX_H);
    s_jt.lock_date = jt_create_label(box, "11月24日", 33, 32, 278, 58,
                                     jt_spec_date(), jt_ink());
    s_jt.lock_week = jt_create_label(box, "星期二", 33, 90, 278, 44,
                                     jt_spec_week(), jt_ink());
    jt_create_rule(box, 33, 139, 278, 2, jt_gray());
    s_jt.lock_temp_label = jt_create_label(box, "温度", 33, 150, 82, 56,
                                           jt_spec_label(), jt_ink());
    s_jt.lock_temp = jt_create_label(box, "---°C", 171, 150, 140, 56,
                                     jt_spec_temp(), jt_ink());

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_LOCK_MEMO_BOX_X, JT_LOCK_MEMO_BOX_Y);
    lv_obj_set_size(box, JT_LOCK_MEMO_BOX_W, JT_LOCK_MEMO_BOX_H);
    lv_obj_t *memo = jt_create_label(box, "MEMO", 33, 28, 501, 50,
                                     jt_spec_label(), jt_gray());
    lv_obj_set_style_text_align(memo, LV_TEXT_ALIGN_LEFT, 0);
    jt_create_rule(box, 33, 84, 501, 2, jt_gray());
    s_jt.lock_memo = jt_create_label(box, "", 33, 90, 501, 130,
                                     jt_spec_memo(), jt_ink());
}

static void jt_build_home_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_HOME, jt_paper());
    lv_obj_t *box;
    lv_obj_t *pill;

    pill = lv_obj_create(page);
    lv_obj_set_pos(pill, 632, 8);
    lv_obj_set_size(pill, 286, 62);
    lv_obj_set_style_radius(pill, 28, 0);
    lv_obj_set_style_bg_color(pill, jt_panel(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pill, jt_ink(), 0);
    lv_obj_set_style_border_width(pill, 3, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    jt_set_no_scroll(pill);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
    jt_create_gear_button(page, JT_GEAR_HOME_X, JT_GEAR_HOME_Y);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, 35, 122);
    lv_obj_set_size(box, 952, JT_S2_CARD_H);
    jt_create_label(box, "解题卡 · 今日剩余", 34, 30, 430, 64,
                    jt_font_bold(), jt_ink());
    s_jt.home_card = jt_create_label(box, "5 / 5 张", 610, 26, 306, 76,
                                     jt_font_bold(), jt_ink());
    lv_obj_set_style_text_align(s_jt.home_card, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *sub = lv_label_create(page);
    lv_label_set_text(sub, "拍 到 几 道 是 几 道");
    lv_obj_set_style_text_font(sub, jt_font_bold(), 0);
    lv_obj_set_style_text_color(sub, jt_ink(), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, JT_S2_SUB_Y);

    lv_obj_t *title = jt_create_label(page, "简单    拍题", 0, JT_S2_TITLE_Y,
                                      JT_UI_W, JT_S2_TITLE_H,
                                      jt_font_display(), jt_ink());
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *cam_btn = lv_btn_create(page);
    lv_obj_set_pos(cam_btn, JT_S2_CAM_X, JT_S2_CAM_Y);
    lv_obj_set_size(cam_btn, JT_S2_CAM_W, JT_S2_CAM_H);
    lv_obj_set_style_radius(cam_btn, JT_S2_CAM_W / 2, 0);
    lv_obj_set_style_pad_all(cam_btn, 0, 0);
    lv_obj_set_style_border_width(cam_btn, 0, 0);
    lv_obj_set_style_bg_opa(cam_btn, LV_OPA_TRANSP, 0);
    jt_set_no_scroll(cam_btn);
    lv_obj_t *cam_img = lv_img_create(cam_btn);
    lv_img_set_src(cam_img, &jt_icon_shutter_button);
    lv_obj_set_size(cam_img, JT_S2_CAM_W, JT_S2_CAM_H);
    lv_obj_center(cam_img);
    lv_obj_add_event_cb(cam_btn, jt_go_finder, LV_EVENT_CLICKED, NULL);
}

static void jt_build_finder_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_FINDER, lv_color_hex(0x191919));
    lv_obj_t *img;
    lv_obj_t *btn;

    memset(&s_jt.finder_preview_dsc, 0, sizeof(s_jt.finder_preview_dsc));
    s_jt.finder_preview_dsc.header.w = JT_S3_PREVIEW_W;
    s_jt.finder_preview_dsc.header.h = JT_S3_PREVIEW_H;
    s_jt.finder_preview_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_jt.finder_preview_dsc.data_size = JT_S3_PREVIEW_W * JT_S3_PREVIEW_H * 2;

    s_jt.finder_preview = lv_img_create(page);
    lv_obj_set_pos(s_jt.finder_preview, JT_S3_PREVIEW_X, JT_S3_PREVIEW_Y);
    lv_obj_set_size(s_jt.finder_preview, JT_S3_PREVIEW_W, JT_S3_PREVIEW_H);
    jt_set_no_scroll(s_jt.finder_preview);

    s_jt.frozen_caption = jt_create_label(page, "", 32, 350, 900, 64,
                                          jt_font_bold(), jt_blue());
    lv_obj_set_style_text_align(s_jt.frozen_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_jt.frozen_caption, LV_OBJ_FLAG_HIDDEN);

    s_jt.finder_status = jt_create_label(page, "摄像头启动中", 32, 20, 600, 48,
                                         jt_font_body(), lv_color_white());
    lv_obj_add_flag(s_jt.finder_status, LV_OBJ_FLAG_HIDDEN);

    s_jt.finder_back_btn = jt_create_icon_button(page, &jt_icon_back, 36, 452,
                                                 JT_S3_BTN_W,
                                                 lv_color_hex(0x191919),
                                                 jt_finder_back, NULL);
    lv_obj_set_size(s_jt.finder_back_btn, JT_S3_BTN_W, JT_S3_BTN_H);
    s_jt.finder_confirm_btn = jt_create_icon_button(page, &jt_icon_confirm,
                                                    850, 452, JT_S3_BTN_W,
                                                    lv_color_hex(0x191919),
                                                    jt_finder_send, NULL);
    lv_obj_set_size(s_jt.finder_confirm_btn, JT_S3_BTN_W, JT_S3_BTN_H);
    btn = lv_obj_get_child(s_jt.finder_confirm_btn, 0);
    s_jt.finder_confirm_icon = btn;
    s_jt.finder_shutter_btn = lv_btn_create(page);
    lv_obj_set_pos(s_jt.finder_shutter_btn, 443, 452);
    lv_obj_set_size(s_jt.finder_shutter_btn, JT_S3_BTN_W, JT_S3_BTN_H);
    lv_obj_set_style_radius(s_jt.finder_shutter_btn, JT_S3_BTN_W / 2, 0);
    lv_obj_set_style_pad_all(s_jt.finder_shutter_btn, 0, 0);
    lv_obj_set_style_border_width(s_jt.finder_shutter_btn, 0, 0);
    lv_obj_set_style_bg_opa(s_jt.finder_shutter_btn, LV_OPA_TRANSP, 0);
    jt_set_no_scroll(s_jt.finder_shutter_btn);
    img = lv_img_create(s_jt.finder_shutter_btn);
    lv_img_set_src(img, &jt_icon_shutter_button);
    lv_obj_set_size(img, JT_S3_BTN_W, JT_S3_BTN_H);
    lv_obj_center(img);
    lv_obj_add_event_cb(s_jt.finder_shutter_btn, jt_finder_shutter,
                        LV_EVENT_CLICKED, NULL);
    jt_set_finder_state(JT_FINDER_LIVE);
}

static void jt_build_process_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_PROCESS, jt_paper());
    lv_obj_t *bar;

    s_jt.process_clock = jt_create_label(page, "20:06", 0, JT_S4_CLOCK_Y,
                                         JT_UI_W, JT_S4_CLOCK_H,
                                         jt_font_display(), jt_ink());
    lv_obj_set_style_text_align(s_jt.process_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *title = jt_create_label(page, "简题 处理中…", 0, JT_S4_TITLE_Y,
                                      JT_UI_W, JT_S4_TITLE_H,
                                      jt_font_display(), jt_ink());
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    bar = lv_obj_create(page);
    lv_obj_set_pos(bar, JT_S4_BAR_X, JT_S4_BAR_Y);
    lv_obj_set_size(bar, JT_S4_BAR_W, JT_S4_BAR_H);
    lv_obj_set_style_bg_color(bar, jt_panel(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, jt_ink(), 0);
    lv_obj_set_style_border_width(bar, 3, 0);
    lv_obj_set_style_radius(bar, 13, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    jt_set_no_scroll(bar);
    s_jt.process_bar_fill = lv_obj_create(bar);
    lv_obj_set_pos(s_jt.process_bar_fill, 4, 7);
    lv_obj_set_size(s_jt.process_bar_fill, 0, 10);
    lv_obj_set_style_bg_color(s_jt.process_bar_fill, jt_ink(), 0);
    lv_obj_set_style_bg_opa(s_jt.process_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_jt.process_bar_fill, 0, 0);
    lv_obj_set_style_radius(s_jt.process_bar_fill, 6, 0);
    jt_set_no_scroll(s_jt.process_bar_fill);
    s_jt.process_bar = NULL;

    for (int i = 0; i < 5; i++) {
        lv_obj_t *circle;
        char num[4];
        s_jt.process_stage[i] = jt_create_button(page, s_stage_labels[i],
                                                  30 + i * 196, JT_S4_STAGE_Y,
                                                  JT_S4_STAGE_W, JT_S4_STAGE_H,
                                                  NULL, NULL, false);
        lv_obj_set_style_radius(s_jt.process_stage[i], 24, 0);
        lv_obj_set_style_border_width(s_jt.process_stage[i], 0, 0);
        lv_obj_set_style_bg_color(s_jt.process_stage[i], jt_paper(), 0);
        s_jt.process_stage_text[i] = lv_obj_get_child(s_jt.process_stage[i], 0);
        lv_label_set_long_mode(s_jt.process_stage_text[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_jt.process_stage_text[i], JT_S4_STAGE_W - 10);
        lv_obj_clear_flag(s_jt.process_stage[i], LV_OBJ_FLAG_CLICKABLE);
        circle = lv_obj_create(s_jt.process_stage[i]);
        lv_obj_set_align(circle, LV_ALIGN_TOP_MID);
        lv_obj_set_pos(circle, 0, 4);
        lv_obj_set_size(circle, 66, 66);
        lv_obj_set_style_radius(circle, 33, 0);
        lv_obj_set_style_border_color(circle, jt_gray(), 0);
        lv_obj_set_style_border_width(circle, 3, 0);
        lv_obj_set_style_bg_color(circle, jt_paper(), 0);
        lv_obj_set_style_pad_all(circle, 0, 0);
        jt_set_no_scroll(circle);
        lv_obj_move_background(circle);
        snprintf(num, sizeof(num), "%d", i + 1);
        s_jt.process_stage_num[i] = jt_create_label(circle, num, 0, 12, 66, 38,
                                                    jt_font_body(), jt_ink());
        lv_obj_set_style_text_align(s_jt.process_stage_num[i], LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void jt_build_idea_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_IDEA, jt_paper());
    lv_obj_t *box;

    jt_create_corner_image_button(page, &jt_icon_home_button_idea,
                                  JT_S5_HEAD_X, 20, jt_go_home);

    s_jt.idea_title = jt_create_label(page, "共5题第1题 · 思路",
                                      JT_S5_TITLE_X, 58, JT_S5_TITLE_W, 55,
                                      jt_spec_label(), jt_ink());
    s_jt.idea_card = jt_create_button(page, "解题卡 5张", JT_S5_CARD_X, 44,
                                      JT_S5_CARD_W, JT_S5_CARD_H,
                                      jt_use_card, NULL, true);
    lv_obj_set_style_radius(s_jt.idea_card, 18, 0);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_S5_BOX_X, JT_S5_BOX_Y);
    lv_obj_set_size(box, JT_S5_BOX_W, JT_S5_BOX_H);
    s_jt.idea_verdict = jt_create_label(box, "这个题做错了", 35, 18,
                                         JT_S5_BOX_W - 70, 64,
                                         jt_spec_temp(), jt_ink());
    s_jt.idea_steps = lv_label_create(box);
    jt_set_label_multiline(s_jt.idea_steps, s_idea_steps, 3);
    lv_obj_set_style_text_font(s_jt.idea_steps, jt_font_bold(), 0);
    lv_obj_set_style_text_color(s_jt.idea_steps, jt_ink(), 0);
    lv_obj_set_pos(s_jt.idea_steps, 35, 92);
    lv_obj_set_size(s_jt.idea_steps, JT_S5_BOX_W - 70, JT_S5_BOX_H - 110);
    jt_set_no_scroll(s_jt.idea_steps);

    lv_obj_t *prev = jt_create_button(page, "上一题", 35, 512, 166, 84,
                                      jt_prev_question, NULL, false);
    lv_obj_t *next = jt_create_button(page, "下一题", 823, 512, 166, 84,
                                      jt_next_question, NULL, true);
    lv_obj_set_style_radius(prev, 30, 0);
    lv_obj_set_style_radius(next, 30, 0);
}

static void jt_build_result_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_RESULT, jt_paper());
    lv_obj_t *box;

    jt_create_corner_image_button(page, &jt_icon_home_button,
                                  42, 20, jt_go_home);
    s_jt.result_title = jt_create_label(page, "共5题第1题 · 结果",
                                        206, 58, 552, 55,
                                        jt_spec_label(), jt_ink());
    jt_create_corner_image_button(page, &jt_icon_back_button,
                                  878, 20, jt_go_idea);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, 35, 166);
    lv_obj_set_size(box, 954, 434);
    lv_obj_set_style_bg_color(box, jt_panel(), 0);
    lv_obj_set_style_border_color(box, jt_ink(), 0);

    s_jt.result_chain = lv_label_create(box);
    jt_set_label_multiline(s_jt.result_chain, s_result_chain, 4);
    lv_obj_set_style_text_font(s_jt.result_chain, jt_spec_temp(), 0);
    lv_obj_set_style_text_color(s_jt.result_chain, jt_ink(), 0);
    lv_obj_set_pos(s_jt.result_chain, 35, 24);
    lv_obj_set_size(s_jt.result_chain, 883, 186);
    jt_set_no_scroll(s_jt.result_chain);
}

static void jt_build_settings_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_SETTINGS, jt_paper());
    lv_obj_t *box;
    lv_obj_t *radio;
    char hidden_ssid[16];

    jt_create_corner_image_button(page, &jt_icon_home_button,
                                  42, 20, jt_go_home);
    jt_create_label(page, "设置", 206, 58, 200, 55, jt_spec_label(), jt_ink());

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_S7_NET_X, JT_S7_NET_Y);
    lv_obj_set_size(box, JT_S7_NET_W, JT_S7_NET_H);
    jt_create_label(box, "网络连接", 27, 15, 416, 46, jt_spec_label(), jt_ink());
    jt_create_label(box, "SSID", 27, 68, 180, 44, jt_font_body(), jt_gray());
    snprintf(hidden_ssid, sizeof(hidden_ssid), "%s", "Home-702");
    s_jt.settings_ssid = jt_create_label(box, hidden_ssid, 211, 68, 220, 44,
                                         jt_font_body(), jt_ink());
    jt_create_label(box, "信号强弱", 27, 119, 120, 44, jt_font_body(), jt_gray());
    s_jt.settings_signal = jt_create_label(box, "强", 157, 119, 40, 44,
                                           jt_font_body(), jt_ink());
    lv_obj_t *wifi_btn = jt_create_button(box, "配置 Wi-Fi", 27, 171, 416, 25,
                     jt_open_softap, NULL, false);
    lv_obj_set_style_radius(wifi_btn, 20, 0);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_S7_SRV_X, JT_S7_NET_Y);
    lv_obj_set_size(box, JT_S7_SRV_W, JT_S7_NET_H);
    jt_create_label(box, "服务器连接", 27, 15, 416, 46, jt_spec_label(), jt_ink());
    s_jt.settings_server_btn[1] = jt_create_button(box, "官方服务器", 27, 68, 416, 68,
                                                  jt_select_server, (void *)1, true);
    s_jt.settings_server_btn[0] = jt_create_button(box, "自有服务器", 27, 144, 416, 68,
                                                  jt_select_server, (void *)0, false);
    radio = lv_obj_create(box);
    lv_obj_set_pos(radio, 39, 92);
    lv_obj_set_size(radio, 18, 18);
    lv_obj_set_style_radius(radio, 9, 0);
    lv_obj_set_style_bg_color(radio, jt_panel(), 0);
    lv_obj_set_style_border_width(radio, 0, 0);
    lv_obj_set_style_pad_all(radio, 0, 0);
    jt_set_no_scroll(radio);
    s_jt.settings_server_radio[1] = radio;
    radio = lv_obj_create(box);
    lv_obj_set_pos(radio, 39, 168);
    lv_obj_set_size(radio, 18, 18);
    lv_obj_set_style_radius(radio, 9, 0);
    lv_obj_set_style_bg_color(radio, jt_panel(), 0);
    lv_obj_set_style_border_color(radio, jt_ink(), 0);
    lv_obj_set_style_border_width(radio, 3, 0);
    lv_obj_set_style_pad_all(radio, 0, 0);
    jt_set_no_scroll(radio);
    s_jt.settings_server_radio[0] = radio;

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, JT_S7_CHAIN_X, JT_S7_CHAIN_Y);
    lv_obj_set_size(box, JT_S7_CHAIN_W, JT_S7_CHAIN_H);
    s_jt.settings_chain = jt_create_label(box, "",
                                           415, 47, 308, 39,
                                          jt_font_body(), jt_ink());
    lv_obj_add_flag(s_jt.settings_chain, LV_OBJ_FLAG_HIDDEN);
    jt_create_button(box, "连通测试", 743, 18, 184, 57,
                     jt_run_chain_test, NULL, true);
    jt_create_label(box, "1", 42, 11, 34, 34, jt_font_bold(), jt_ink());
    jt_create_label(box, "WiFi", 28, 51, 122, 32, jt_font_body(), jt_gray());
    jt_create_label(box, "→", 99, 11, 49, 34, jt_font_bold(), jt_ink());
    jt_create_label(box, "2", 183, 11, 34, 34, jt_font_bold(), jt_ink());
    jt_create_label(box, "互联网", 150, 51, 138, 32, jt_font_body(), jt_gray());
    jt_create_label(box, "→", 252, 11, 49, 34, jt_font_bold(), jt_ink());
    jt_create_label(box, "3", 336, 11, 34, 34, jt_font_bold(), jt_ink());
    jt_create_label(box, "服务器", 293, 51, 138, 32, jt_font_body(), jt_gray());
    jt_create_rule(box, 27, 86, 900, 2, jt_gray());
    char device_text[64];
    snprintf(device_text, sizeof(device_text), "设备ID:%s", s_jt.device_id);
    jt_create_label(box, device_text, 27, 86, 620, 53,
                    jt_font_body(), jt_ink());
    jt_create_button(box, "显示设备二维码", 701, 94, 226, 45,
                     jt_open_qr, NULL, false);
    jt_create_label(box, "版本号：", 27, 143, 160, 41,
                    jt_font_body(), jt_gray());
    jt_create_label(box, "V1.0", 175, 143, 180, 41,
                    jt_font_bold(), jt_ink());
    s_jt.settings_device_id = jt_create_label(box, "", 27, 190, 880, 38,
                                              jt_font_body(), jt_ink());

    s_jt.softap_page = lv_obj_create(s_jt.root);
    lv_obj_set_size(s_jt.softap_page, JT_UI_W, JT_UI_H);
    lv_obj_set_style_bg_color(s_jt.softap_page, jt_paper(), 0);
    lv_obj_set_style_bg_opa(s_jt.softap_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_jt.softap_page, 0, 0);
    jt_set_no_scroll(s_jt.softap_page);
    lv_obj_t *softap_title = jt_create_label(s_jt.softap_page, "连接热点",
                                             0, 100, JT_UI_W, 80,
                                             jt_font_bold(), jt_ink());
    lv_obj_set_style_text_align(softap_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *softap_name = jt_create_label(s_jt.softap_page, "",
                                            0, 210, JT_UI_W, 80,
                                            jt_font_body(), jt_ink());
    char softap_ssid[64];
    snprintf(softap_ssid, sizeof(softap_ssid), "JianTi-%s", s_jt.device_id);
    lv_label_set_text(softap_name, softap_ssid);
    lv_obj_set_style_text_align(softap_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *softap_note = jt_create_label(s_jt.softap_page,
                                            "用手机连上这个热点，连上后在手机上完成配网",
                                            0, 320, JT_UI_W, 70,
                                            jt_font_body(), jt_ink());
    lv_obj_set_style_text_align(softap_note, LV_TEXT_ALIGN_CENTER, 0);
    jt_create_button(s_jt.softap_page, "返回设置", 392, 430, 240, 70,
                     jt_close_softap, NULL, true);
    lv_obj_add_flag(s_jt.softap_page, LV_OBJ_FLAG_HIDDEN);

    s_jt.qr_page = lv_obj_create(s_jt.root);
    lv_obj_set_size(s_jt.qr_page, JT_UI_W, JT_UI_H);
    lv_obj_set_style_bg_color(s_jt.qr_page, jt_paper(), 0);
    lv_obj_set_style_bg_opa(s_jt.qr_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_jt.qr_page, 0, 0);
    jt_set_no_scroll(s_jt.qr_page);
    box = lv_obj_create(s_jt.qr_page);
    jt_style_box(box);
    lv_obj_set_pos(box, 352, 55);
    lv_obj_set_size(box, 320, 400);
    lv_obj_set_style_bg_color(box, jt_panel(), 0);
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 10; x++) {
            bool dark = ((x * 7 + y * 3 + ((x / 3) ^ (y / 3))) % 5) < 2;
            if ((x < 3 && y < 3) || (x > 6 && y < 3) || (x < 3 && y > 6)) {
                dark = (x == 0 || y == 0 || x == 2 || y == 2 || x == 7 || y == 7);
            }
            if (dark) {
                lv_obj_t *cell = lv_obj_create(box);
                lv_obj_set_pos(cell, 18 + x * 28, 18 + y * 28);
                lv_obj_set_size(cell, 24, 24);
                lv_obj_set_style_bg_color(cell, jt_ink(), 0);
                lv_obj_set_style_border_width(cell, 0, 0);
                lv_obj_set_style_pad_all(cell, 0, 0);
            }
        }
    }
    jt_create_button(s_jt.qr_page, "关闭", 392, 490, 240, 70,
                     jt_close_qr, NULL, true);
    lv_obj_add_flag(s_jt.qr_page, LV_OBJ_FLAG_HIDDEN);
}

static void jt_build_claim_screen(void)
{
    lv_obj_t *page = jt_create_page(JT_SCREEN_CLAIM, jt_paper());
    lv_obj_t *box;

    jt_create_corner_image_button(page, &jt_icon_home_button,
                                  42, 20, jt_go_home);
    jt_create_label(page, "领取 · 解题卡", 206, 58, 254, 55,
                    jt_spec_label(), jt_ink());
    jt_create_corner_image_button(page, &jt_icon_back_button,
                                  878, 20, jt_go_idea);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, 35, 161);
    lv_obj_set_size(box, 954, 104);
    s_jt.claim_balance = jt_create_label(box, "今日剩余 5 张", 31, 15, 300, 73,
                                         jt_spec_label(), jt_ink());
    s_jt.claim_advanced = jt_create_label(box, "", 761, 30, 160, 43,
                                           jt_spec_week(), jt_ink());
    lv_obj_set_style_text_align(s_jt.claim_advanced, LV_TEXT_ALIGN_RIGHT, 0);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, 35, 277);
    lv_obj_set_size(box, 471, 274);
    jt_create_label(box, "自己来预支", 27, 17, 416, 59,
                    jt_spec_temp(), jt_ink());
    jt_create_button(box, "领取 1 张", 27, 207, 416, 55,
                     jt_advance_card, NULL, true);

    box = lv_obj_create(page);
    jt_style_box(box);
    lv_obj_set_pos(box, 518, 277);
    lv_obj_set_size(box, 471, 274);
    jt_create_label(box, "向家长申请", 27, 17, 416, 59,
                    jt_spec_temp(), jt_ink());
    s_jt.claim_send_btn = jt_create_button(box, "发送申请", 27, 207, 416, 55,
                                           jt_ask_card, NULL, false);
    s_jt.claim_feedback = jt_create_label(page, "", 35, 563, 954, 37,
                                          jt_spec_sub(), jt_gray());
    s_jt.claim_withdraw_btn = jt_create_button(page, "撤回申请", 545, 478, 416, 55,
                                               jt_withdraw_card_request, NULL, false);
    lv_obj_add_flag(s_jt.claim_withdraw_btn, LV_OBJ_FLAG_HIDDEN);
}

#if CONFIG_JT_FINDER_SELFTEST
static void jt_run_finder_selftest(void)
{
    ESP_LOGI(JT_UI_TAG, "finder selftest begin");
    jt_show_screen(JT_SCREEN_FINDER);
    jt_set_finder_state(JT_FINDER_LIVE);
    jt_finder_send(NULL);
    ESP_LOGI(JT_UI_TAG, "selftest live_send_screen=%u", s_jt.screen);
    jt_finder_shutter(NULL);
    ESP_LOGI(JT_UI_TAG, "selftest frozen_state=%d", (int)s_jt.finder_state);
    jt_finder_shutter(NULL);
    jt_finder_back(NULL);
    ESP_LOGI(JT_UI_TAG, "selftest retake_state=%d screen=%u",
             (int)s_jt.finder_state, s_jt.screen);
    jt_finder_shutter(NULL);
    jt_finder_send(NULL);
    ESP_LOGI(JT_UI_TAG, "selftest sent_screen=%u", s_jt.screen);
    jt_show_screen(JT_SCREEN_LOCK);
    ESP_LOGI(JT_UI_TAG, "finder selftest end");
}
#endif

static void jt_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    jt_update_clock_labels();
    jt_update_environment_labels();
}

#if CONFIG_JT_UI_SNAPSHOT_TEST
static void jt_dump_snapshot(const char *label, lv_obj_t *obj)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint32_t needed = lv_snapshot_buf_size_needed(obj, LV_IMG_CF_TRUE_COLOR);
    lv_img_dsc_t *dsc = NULL;

    if (!needed) {
        ESP_LOGE(JT_UI_TAG, "snapshot %s alloc failed need=%u", label, (unsigned)needed);
        return;
    }
    dsc = lv_snapshot_take(obj, LV_IMG_CF_TRUE_COLOR);
    if (!dsc) {
        ESP_LOGE(JT_UI_TAG, "snapshot %s take failed need=%u", label, (unsigned)needed);
        return;
    }
    printf("JT_SNAPSHOT_BEGIN %s %u %u %u\n", label, (unsigned)dsc->header.w,
           (unsigned)dsc->header.h, (unsigned)dsc->data_size);
    for (uint32_t i = 0; i < dsc->data_size; i++) {
        const unsigned char byte = ((const unsigned char *)dsc->data)[i];
        putchar(hex[byte >> 4]);
        putchar(hex[byte & 0x0f]);
        if ((i & 0x3fff) == 0x3fff) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    printf("\nJT_SNAPSHOT_END %s\n", label);
    lv_snapshot_free(dsc);
}
#endif

void app_jt_ui_del(void)
{
    memset(&s_jt, 0, sizeof(s_jt));
}

void app_jt_ui_init(void)
{
    ESP_LOGI(JT_UI_TAG, "Initializing JianTi v114 native-widget UI");
    jt_business_init();
    bsp_display_brightness_set(45);
    memset(&s_jt, 0, sizeof(s_jt));
    s_jt.question_count = 5;
    s_jt.server_official = true;
    snprintf(s_jt.device_id, sizeof(s_jt.device_id), "%s",
             jt_client_get_terminal_id());
    ESP_LOGI(JT_UI_TAG, "device id=%s source=%s", s_jt.device_id,
             jt_client_terminal_id_source());
    jt_hide_legacy_ui();

    s_jt.root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_jt.root, JT_UI_W, JT_UI_H);
    lv_obj_set_pos(s_jt.root, 0, 0);
    lv_obj_set_style_bg_color(s_jt.root, jt_paper(), 0);
    lv_obj_set_style_bg_opa(s_jt.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_jt.root, 0, 0);
    lv_obj_set_style_pad_all(s_jt.root, 0, 0);
    jt_set_no_scroll(s_jt.root);

    jt_build_lock_screen();
    jt_build_home_screen();
    jt_build_finder_screen();
    jt_build_process_screen();
    jt_build_idea_screen();
    jt_build_result_screen();
    jt_build_settings_screen();
    jt_build_claim_screen();
    jt_add_prototype_backgrounds();

    lv_timer_create(jt_timer_cb, 1000, NULL);
    lv_timer_create(jt_process_timer_cb, 500, NULL);
#if CONFIG_JT_UI_CAPTURE_TOUR
    lv_timer_create(jt_capture_tour_timer_cb, 4000, NULL);
#endif

    jt_show_screen_raw(JT_SCREEN_LOCK);
    jt_update_environment_labels();
    lv_general.current_parent = s_jt.root;
    lv_general.del_function = app_jt_ui_del;
    lv_obj_move_foreground(s_jt.root);
    lv_refr_now(NULL);
#if CONFIG_JT_FINDER_SELFTEST
    jt_run_finder_selftest();
#endif
#if CONFIG_JT_UI_SNAPSHOT_TEST
    {
        static const char *const labels[JT_SCREEN_COUNT] = {
            "lock", "home", "finder", "process",
            "idea", "result", "settings", "claim",
        };
        app_jt_ui_set_environment("---°C", "更细心地去倾听，你就会知道");
        for (uint8_t i = 0; i < JT_SCREEN_COUNT; i++) {
            jt_show_screen_raw(i);
            lv_refr_now(NULL);
            taskYIELD();
            vTaskDelay(pdMS_TO_TICKS(20));
            jt_dump_snapshot(labels[i], s_jt.pages[i]);
        }
        jt_show_screen_raw(JT_SCREEN_LOCK);
    }
#endif
    ESP_LOGI(JT_UI_TAG, "JianTi v114 native-widget UI ready");
}
