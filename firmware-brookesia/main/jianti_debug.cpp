/*
 * 简题 App 远程联调钩子（仅 CONFIG_JIANTI_DEBUG_AUTOTEST 构建启用）
 * SPDX-License-Identifier: Apache-2.0
 *
 * 板上没有键盘鼠标，联调靠串口：
 *   1) 启动后截主屏 -> 进简题 App -> 截简题页 -> 触发拍题 -> 截结果页
 *   2) 每张图走 LVGL snapshot -> JPEG -> base64，用 ===JT_SHOT_BEGIN/END=== 包起来
 * 串口控制台是 UART0，波特率 2,000,000。
 */
#include "sdkconfig.h"

#if CONFIG_JIANTI_DEBUG_AUTOTEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <new>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/jpeg_encode.h"
#include "lvgl.h"

#include "bsp_board_extra.h"
#include "esp_brookesia.hpp"
#include "esp_brookesia_app_jianti.hpp"

static const char *TAG = "JianTiDbg";

/* 本批字体对比测试页要用到的字体（.c 里是 C 链接，必须在匿名 namespace 之外声明） */
LV_FONT_DECLARE(font_jt_cmp_18_4);
LV_FONT_DECLARE(font_jt_cmp_20_4);
LV_FONT_DECLARE(font_jt_cmp_22_4);
LV_FONT_DECLARE(font_jt_cmp_24_4);
LV_FONT_DECLARE(font_jt_cmp_label_16_4);
/* 本批新增：28px / 32px 是按字号单独烘的字体文件，不是把 24px 放大。 */
LV_FONT_DECLARE(font_jt_cmp_28_4);
LV_FONT_DECLARE(font_jt_cmp_32_4);
LV_FONT_DECLARE(font_jt_cmp_14_4);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_22);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_18);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_20);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_22);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_24);

namespace
{

struct Ctx {
    esp_brookesia::systems::phone::Phone *phone;
    int app_id;
};

/* 截图缓冲由 LVGL 堆分配，创建/销毁都必须在显示锁内，否则会与渲染线程抢 layer 状态 */
static void freeShot(lv_draw_buf_t *shot)
{
    if (!shot) {
        return;
    }
    bsp_extra_display_lock(-1);
    lv_draw_buf_destroy(shot);
    bsp_extra_display_unlock();
}

/* 截当前活动屏并把 JPEG 以 base64 打到串口 */
bool dumpScreen(const char *tag)
{
    lv_draw_buf_t *shot = nullptr;

    /* lv_snapshot_take 会临时改写 display->layer_head，必须与渲染线程互斥 */
    bsp_extra_display_lock(-1);
    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        shot = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    }
    bsp_extra_display_unlock();

    if (!scr) {
        ESP_LOGE(TAG, "%s: no active screen", tag);
        return false;
    }
    if (!shot) {
        ESP_LOGE(TAG, "%s: snapshot failed", tag);
        return false;
    }

    const uint32_t w = shot->header.w;
    const uint32_t h = shot->header.h;
    const size_t src_len = static_cast<size_t>(shot->data_size);

    jpeg_encoder_handle_t enc = nullptr;
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    if (jpeg_new_encoder_engine(&eng_cfg, &enc) != ESP_OK) {
        ESP_LOGE(TAG, "%s: jpeg engine failed", tag);
        freeShot(shot);
        return false;
    }

    size_t out_cap = src_len;
    size_t alloc_sz = 0;
    uint8_t *out = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(out_cap, nullptr, &alloc_sz));
    if (!out) {
        ESP_LOGE(TAG, "%s: jpeg out alloc failed", tag);
        jpeg_del_encoder_engine(enc);
        freeShot(shot);
        return false;
    }

    jpeg_encode_cfg_t cfg = {};
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV444;
    cfg.width = w;
    cfg.height = h;
    cfg.image_quality = 85;

    uint32_t out_bytes = 0;
    esp_err_t err = jpeg_encoder_process(enc, &cfg, shot->data, src_len, out, out_cap, &out_bytes);
    jpeg_del_encoder_engine(enc);
    freeShot(shot);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: encode failed: %s", tag, esp_err_to_name(err));
        heap_caps_free(out);
        return false;
    }

    ESP_LOGI(TAG, "%s: %ux%u -> %u bytes jpeg, dumping", tag, (unsigned)w, (unsigned)h,
             (unsigned)out_bytes);

    /* base64 编码 + 打印 */
    static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    printf("===JT_SHOT_BEGIN %s===\n", tag);
    char line[97];
    size_t li = 0;
    for (size_t i = 0; i < out_bytes; i += 3) {
        uint32_t v = out[i] << 16;
        if (i + 1 < out_bytes) {
            v |= out[i + 1] << 8;
        }
        if (i + 2 < out_bytes) {
            v |= out[i + 2];
        }
        line[li++] = B64[(v >> 18) & 0x3F];
        line[li++] = B64[(v >> 12) & 0x3F];
        line[li++] = (i + 1 < out_bytes) ? B64[(v >> 6) & 0x3F] : '=';
        line[li++] = (i + 2 < out_bytes) ? B64[v & 0x3F] : '=';
        if (li >= 96) {
            line[li] = '\0';
            printf("%s\n", line);
            li = 0;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        printf("%s\n", line);
    }
    printf("===JT_SHOT_END %s===\n", tag);
    fflush(stdout);

    heap_caps_free(out);
    return true;
}

/* 触摸坐标日志：任何点按/滑动都会打 TOUCH 行 */
static void touchLogger(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_target(e) ? (lv_indev_t *)lv_event_get_target(e) : lv_indev_active();
    lv_point_t p = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    lv_event_code_t code = lv_event_get_code(e);
    const char *what = (code == LV_EVENT_PRESSED) ? "PRESS"
                       : (code == LV_EVENT_RELEASED ? "RELEASE" : "OTHER");
    ESP_LOGI("JianTiTouch", "TOUCH %s x=%d y=%d", what, (int)p.x, (int)p.y);
}

/* ================= 本批：字体对比测试页 =================
 * 只测字体：18/20/22/24 四档 x {普惠, Montserrat, Maison Neue} x {不加粗, 加粗}
 * 一行一个类型，共 24 行；页面 800x480 竖向滚动。
 *
 * 加粗 = 同字形右移 1px 重绘（重绘模拟）：
 *   板上没有任何粗体 ttf/c 文件；LVGL 9.5 的 text_outline_stroke 只在
 *   FreeType 矢量字体路径生效，而本工程 CONFIG_LV_USE_FREETYPE /
 *   CONFIG_LV_USE_VECTOR_GRAPHIC 均未开启，所以用"画两遍、第二遍右移 1px"模拟。
 *
 * 页面挂在简题 App 的当前屏上、盖满整屏、置顶，不改首页/结果页/服务器页布局。 */
#ifndef JIANTI_FONT_COMPARE
#define JIANTI_FONT_COMPARE 0   /* 上一批的字体对比页已收尾，本批走 fs_ 六页截图链路 */
#endif

constexpr int kFcW = 800;
constexpr int kFcH = 480;
constexpr int kFcHeaderH = 44;

/* 本批在页面顶部单加「标题对比」小节：28 / 32 / 24 加粗 + 14px 副标题参考。
 * 示例文案统一「简题」，只有 24px 那行同时充当正文参考档。
 * 行高统一 = 字号 + 22，与下面 24 行主表一致。 */
constexpr int kFcTitleHeadH = 30;
constexpr int kFcTitleRowH[4] = {28 + 22, 32 + 22, 24 + 22, 14 + 22};
constexpr int kFcTitleBlockH = kFcTitleHeadH + kFcTitleRowH[0] + kFcTitleRowH[1]
                               + kFcTitleRowH[2] + kFcTitleRowH[3];
constexpr int kFcMainStartY = kFcHeaderH + kFcTitleBlockH;   /* 主表首行顶边 */
/* 段 2 落在 18px 段第 6 行的顶边（18px 行高 40），整行不被切 */
constexpr int kFcSeg2Y = kFcMainStartY + 5 * 40;

/* 每行同一句：中文 + 数字 + 拉丁 + 全角标点 */
static const char *const kFcSample = "简题 轻按一下，思路便知 123 abc";
static const char *const kFcFamName[3] = {"普惠", "Montserrat", "Maison"};
static lv_obj_t *g_fc_root = nullptr;

static const lv_font_t *fcFont(int size, int fam)
{
    switch (fam) {
        case 0:
            return (size == 18) ? &font_jt_cmp_18_4
                   : (size == 20) ? &font_jt_cmp_20_4
                   : (size == 22) ? &font_jt_cmp_22_4
                   : &font_jt_cmp_24_4;
        case 1:
            return (size == 18) ? &lv_font_montserrat_18
                   : (size == 20) ? &lv_font_montserrat_20
                   : (size == 22) ? &lv_font_montserrat_22
                   : &lv_font_montserrat_24;
        default:
            return (size == 18) ? &esp_brookesia_font_maison_neue_book_18
                   : (size == 20) ? &esp_brookesia_font_maison_neue_book_20
                   : (size == 22) ? &esp_brookesia_font_maison_neue_book_22
                   : &esp_brookesia_font_maison_neue_book_24;
    }
}

/* 标题对比小节：28/32/24 加粗 + 14px 副标题参考 */
static const lv_font_t *fcTitleFont(int i)
{
    return (i == 0) ? &font_jt_cmp_28_4
           : (i == 1) ? &font_jt_cmp_32_4
           : (i == 2) ? &font_jt_cmp_24_4
           : &font_jt_cmp_14_4;
}

/* 必须在显示锁内调用 */
static void buildFontComparePage()
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        ESP_LOGE(TAG, "fc: no active screen");
        return;
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, kFcW, kFcH);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_move_foreground(root);
    g_fc_root = root;

    const int sizes[4] = {18, 20, 22, 24};
    int row_h[4] = {0, 0, 0, 0};
    int total = kFcMainStartY;            /* 表头 + 标题对比小节 */
    for (int i = 0; i < 4; i++) {
        row_h[i] = sizes[i] + 22;         /* 字号 + 上下留白，保证 24px 也不挤 */
        total += row_h[i] * 6;            /* 每档 6 行：3 字体 x 2 粗细 */
    }

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_size(content, kFcW, total);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *hd = lv_label_create(content);
        lv_obj_set_style_text_font(hd, &font_jt_cmp_label_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hd, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(hd, "字体对比 18/20/22/24px  加粗=同字形右移1px重绘");
        lv_obj_set_pos(hd, 12, 12);
    }

    /* ---- 标题对比小节（本批新增，放在最顶部，单独一段拍清） ----
     * 示例文案统一「简题」；前三行是 28 / 32 / 24px 加粗，24px 那行同时是正文参考档；
     * 第 T4 行是副标题 14px 不加粗参考（正式版副标题是烘焙图，此处用 14px 字体近似）。 */
    {
        lv_obj_t *sh = lv_obj_create(content);
        lv_obj_set_size(sh, kFcW, kFcTitleHeadH);
        lv_obj_set_pos(sh, 0, kFcHeaderH);
        lv_obj_set_style_bg_color(sh, lv_color_hex(0xE8EEF8), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(sh, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(sh, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sh, 0, LV_PART_MAIN);
        lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *st = lv_label_create(sh);
        lv_obj_set_style_text_font(st, &font_jt_cmp_label_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(st, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(st, "标题对比  示例：简题");
        lv_obj_align(st, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *sn = lv_label_create(sh);
        lv_obj_set_style_text_font(sn, &font_jt_cmp_label_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(sn, lv_color_hex(0x4E5969), LV_PART_MAIN);
        lv_label_set_text(sn, "24px 行 = 正文参考档");
        lv_obj_align(sn, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    static const char *const kTitleTag[4] = {
        "标题 28px 普惠 加粗",
        "标题 32px 普惠 加粗",
        "标题 24px 普惠 加粗",
        "副标题 14px 普惠 不加粗",
    };
    static const char *const kTitleText[4] = {
        "简题", "简题", "简题", "极简作业AI助手",
    };
    static const char *const kTitleNum[4] = {"T1", "T2", "T3", "T4"};
    int ty = kFcHeaderH + kFcTitleHeadH;
    for (int ti = 0; ti < 4; ti++) {
        lv_obj_t *trow = lv_obj_create(content);
        lv_obj_set_size(trow, kFcW, kFcTitleRowH[ti]);
        lv_obj_set_pos(trow, 0, ty);
        lv_obj_set_style_bg_color(trow, (ti & 1) ? lv_color_hex(0xF2F5FA)
                                     : lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(trow, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(trow, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(trow, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(trow, 0, LV_PART_MAIN);
        lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *tnum = lv_label_create(trow);
        lv_obj_set_style_text_font(tnum, &font_jt_cmp_label_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(tnum, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
        lv_label_set_text(tnum, kTitleNum[ti]);
        lv_obj_align(tnum, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *ttag = lv_label_create(trow);
        lv_obj_set_style_text_font(ttag, &font_jt_cmp_label_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(ttag, lv_color_hex(0x4E5969), LV_PART_MAIN);
        lv_label_set_long_mode(ttag, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(ttag, 236);
        lv_label_set_text(ttag, kTitleTag[ti]);
        lv_obj_align(ttag, LV_ALIGN_LEFT_MID, 42, 0);

        const lv_font_t *tf = fcTitleFont(ti);
        lv_obj_t *ts = lv_label_create(trow);
        lv_obj_set_style_text_font(ts, tf, LV_PART_MAIN);
        lv_obj_set_style_text_color(ts, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(ts, kTitleText[ti]);
        lv_obj_align(ts, LV_ALIGN_LEFT_MID, 292, 0);

        /* 前三行加粗：同字形右移 1px 重绘；T4 副标题参考行不加粗 */
        if (ti < 3) {
            lv_obj_t *ts2 = lv_label_create(trow);
            lv_obj_set_style_text_font(ts2, tf, LV_PART_MAIN);
            lv_obj_set_style_text_color(ts2, lv_color_hex(0x1F2329), LV_PART_MAIN);
            lv_label_set_text(ts2, kTitleText[ti]);
            lv_obj_align(ts2, LV_ALIGN_LEFT_MID, 293, 0);
        }

        ty += kFcTitleRowH[ti];
    }

    int y = kFcMainStartY;
    int idx = 0;
    char buf[64];
    for (int si = 0; si < 4; si++) {
        const int sz = sizes[si];
        for (int fam = 0; fam < 3; fam++) {
            for (int bold = 0; bold < 2; bold++) {
                idx++;

                lv_obj_t *row = lv_obj_create(content);
                lv_obj_set_size(row, kFcW, row_h[si]);
                lv_obj_set_pos(row, 0, y);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_bg_color(row, (idx & 1) ? lv_color_white()
                                             : lv_color_hex(0xF2F5FA), LV_PART_MAIN);
                lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
                lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *num = lv_label_create(row);
                lv_obj_set_style_text_font(num, &font_jt_cmp_label_16_4, LV_PART_MAIN);
                lv_obj_set_style_text_color(num, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
                snprintf(buf, sizeof(buf), "%02d", idx);
                lv_label_set_text(num, buf);
                lv_obj_align(num, LV_ALIGN_LEFT_MID, 10, 0);

                lv_obj_t *tag = lv_label_create(row);
                lv_obj_set_style_text_font(tag, &font_jt_cmp_label_16_4, LV_PART_MAIN);
                lv_obj_set_style_text_color(tag, lv_color_hex(0x4E5969), LV_PART_MAIN);
                lv_label_set_long_mode(tag, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(tag, 236);
                snprintf(buf, sizeof(buf), "%dpx %s %s", sz, kFcFamName[fam],
                         bold ? "加粗" : "不加粗");
                lv_label_set_text(tag, buf);
                lv_obj_align(tag, LV_ALIGN_LEFT_MID, 42, 0);

                const lv_font_t *f = fcFont(sz, fam);
                lv_obj_t *s1 = lv_label_create(row);
                lv_obj_set_style_text_font(s1, f, LV_PART_MAIN);
                lv_obj_set_style_text_color(s1, lv_color_hex(0x1F2329), LV_PART_MAIN);
                lv_obj_set_width(s1, 500);
                lv_label_set_long_mode(s1, LV_LABEL_LONG_CLIP);
                lv_label_set_text(s1, kFcSample);
                lv_obj_align(s1, LV_ALIGN_LEFT_MID, 292, 0);

                if (bold) {
                    lv_obj_t *s2 = lv_label_create(row);
                    lv_obj_set_style_text_font(s2, f, LV_PART_MAIN);
                    lv_obj_set_style_text_color(s2, lv_color_hex(0x1F2329), LV_PART_MAIN);
                    lv_obj_set_width(s2, 500);
                    lv_label_set_long_mode(s2, LV_LABEL_LONG_CLIP);
                    lv_label_set_text(s2, kFcSample);
                    lv_obj_align(s2, LV_ALIGN_LEFT_MID, 293, 0);
                }

                y += row_h[si];
            }
        }
    }

    ESP_LOGI(TAG, "fc: page built, %d title rows + %d rows, content %dx%d",
             4, idx, kFcW, total);
}

/* 必须在显示锁内调用 */
static void fcScrollTo(int y)
{
    if (!g_fc_root) {
        return;
    }
    lv_obj_scroll_to_y(g_fc_root, y, LV_ANIM_OFF);
    lv_obj_update_layout(g_fc_root);
}

/* ---- 扣解题卡全链路（8026 卡账本测试版） ----
 * 5 张起步 -> 扣到 0 -> 没卡双入口 -> 向家长申请 -> 家长批 5 张 -> 再扣 -> 预支
 * PC 侧配合窗口用串口 marker 对时：WAIT_A/B/C 之后 board 只等、不动，
 * 由 PC 调 /card/dev_reset、/card/approve，再继续。 */
constexpr int kStillWaitMs = 1500;
constexpr int kStillHoldMs = 12000;
constexpr int kResultSlackMs = 5000;
constexpr int kPageHoldMs = 4000;
constexpr int kWaitAms = 20000;   /* PC：dev_reset balance=0 + reset_unlocked */
constexpr int kWaitBms = 25000;   /* PC：approve 申请单 +5 */
constexpr int kWaitCms = 20000;   /* PC：dev_reset balance=0 + reset_unlocked */

__attribute__((unused)) static void waitWindow(const char *name, int ms)
{
    printf("===JT_AUTOTEST_WAIT %s %d===\n", name, ms);
    fflush(stdout);
    ESP_LOGW(TAG, "WAIT %s: %d ms for PC side (server reset/approve)", name, ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
    printf("===JT_AUTOTEST_RESUME %s===\n", name);
    fflush(stdout);
}

void autotestTask(void *arg)
{
    Ctx *ctx = static_cast<Ctx *>(arg);
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    /* 本批只测字体：等系统起来 -> 进简题 App（拿一块干净的整屏）-> 盖对比页
     * -> 三段滚动截图 + 每段留 12s 给手机翻拍。不触碰任何正式页。 */
#if JIANTI_FONT_COMPARE
    vTaskDelay(pdMS_TO_TICKS(8000));

    auto *fc_app = esp_brookesia::apps::JianTi::requestInstance(false, false);
    if (!fc_app) {
        ESP_LOGE(TAG, "JianTi instance missing");
        delete ctx;
        vTaskDelete(NULL);
        return;
    }
    (void)fc_app;
    /* 等 App 把屏建稳，避免之后 buildUi 的 lv_obj_clean 把我们盖的页清掉 */
    vTaskDelay(pdMS_TO_TICKS(4000));

    bsp_extra_display_lock(-1);
    buildFontComparePage();
    bsp_extra_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));

    int max_y = 0;
    bsp_extra_display_lock(-1);
    if (g_fc_root) {
        lv_obj_update_layout(g_fc_root);
        max_y = lv_obj_get_scroll_bottom(g_fc_root);
    }
    bsp_extra_display_unlock();
    ESP_LOGI(TAG, "fc: scrollable range max_y=%d", max_y);

    /* 分段点尽量落在行顶边，避免照片里把某行从中间切开：
     * 段 1 = 0，整段装下表头 + 标题对比 4 行（kFcTitleBlockH=216，全在 480 内）；
     * 段 2 = kFcSeg2Y，落在 18px 段第 6 行顶边；段 3 = max_y，页底。三段落有重叠、不断行。 */
    const int ys[3] = {0, kFcSeg2Y, max_y};
    const char *tags[3] = {"fc_1of3", "fc_2of3", "fc_3of3"};
    for (int i = 0; i < 3; i++) {
        bsp_extra_display_lock(-1);
        fcScrollTo(ys[i]);
        bsp_extra_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(800));
        ESP_LOGI(TAG, "fc seg %d/3: scroll_y=%d", i + 1, ys[i]);
        dumpScreen(tags[i]);
        ESP_LOGI(TAG, "fc hold: seg %d/3 stays %d ms for phone photo", i + 1, kStillHoldMs);
        vTaskDelay(pdMS_TO_TICKS(kStillHoldMs));
    }

    printf("===JT_AUTOTEST_DONE font_compare===\n");
    fflush(stdout);
    ESP_LOGI(TAG, "BOARD FONT COMPARE DONE: font compare page parked");
    delete ctx;
    vTaskDelete(NULL);
#else
    /* v10 素材批：同样的 7 个界面拍两遍，共 14 张。
     * A 遍（v10A_01..07）：真实拍摄 —— 真起相机、真上传，回包来自 8000 网关。
     * B 遍（v10B_01..07）：假拍 —— 相机/网关异常时的兜底素材；前 5 站同 A，
     *   第 6/7 站不点上传，而是由板端注入一道初一题库抽题（一元一次方程）
     *   的合成回包 → 结果页 / 过程与答案页，不经过网络、也不扣卡。 */
    static const char *kFakePaperJson =
        "{"
        "\"paper_id\":\"demo_grade7_equation\","
        "\"cards\":["
        "{\"title\":\"第1题·思路\","
        "\"text\":\"【题目】解方程 3x - 5 = 2x + 7，并检验。\\n"
        "【思路】把含 x 的项移到方程左边，常数项移到右边，移项要变号；"
        "合并同类项后把 x 的系数化为 1；最后把解代回原方程，检验左右两边是否相等。\"},"
        "{\"title\":\"第1题·过程与答案\","
        "\"text\":\"【过程】移项：3x - 2x = 7 + 5；合并同类项：x = 12。\\n"
        "【检验】左边 = 3×12 - 5 = 31，右边 = 2×12 + 7 = 31，左边 = 右边，"
        "所以 x = 12 是原方程的解。\\n"
        "【答案】x = 12\"}"
        "]"
        "}";

    vTaskDelay(pdMS_TO_TICKS(8000));

    auto *jianti = esp_brookesia::apps::JianTi::requestInstance(false, false);
    if (!jianti) {
        ESP_LOGE(TAG, "JianTi instance missing");
        delete ctx;
        vTaskDelete(NULL);
        return;
    }

    /* 正式版默认口径：裁切铺满 + 静态铺满。这里再显式对齐一次，防构建里残留旧默认 */
    jianti->debugSetFillMode(1);
    jianti->debugSetStillFill(true);
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* ================= A 遍：真实拍摄 ================= */
    printf("===JT_AUTOTEST_PASS A real===\n");
    fflush(stdout);

    /* A1/7 首页：开机直进的拍题主屏 */
    printf("===JT_AUTOTEST_STEP A1/7 home===\n");
    fflush(stdout);
    dumpScreen("v10A_01_home");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* A2/7 服务器页：内网 / 公网 / 测试连接 / 重新配网 */
    jianti->debugClick("server");
    vTaskDelay(pdMS_TO_TICKS(1200));
    dumpScreen("v10A_02_server");
    vTaskDelay(pdMS_TO_TICKS(1800));

    /* A3/7 服务器页「测试连接」：点一下看结果标签有没有回话 */
    jianti->debugClick("test");
    vTaskDelay(pdMS_TO_TICKS(8000));
    dumpScreen("v10A_03_server_test");
    vTaskDelay(pdMS_TO_TICKS(1800));
    jianti->debugClick("back");
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* A4/7 取景器：800x480 铺满 */
    jianti->debugClick("capture");
    vTaskDelay(pdMS_TO_TICKS(8000));   /* 起流 + AE 稳定 */
    dumpScreen("v10A_04_viewfinder");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* A5/7 静态确认：同样 800x480 铺满 */
    jianti->debugClick("shutter");
    vTaskDelay(pdMS_TO_TICKS(kStillWaitMs));
    dumpScreen("v10A_05_still");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* A6/7 上传 + 结果页思路（网关 8000，真模型慢，等 100s） */
    jianti->debugClick("still_ok");
    vTaskDelay(pdMS_TO_TICKS(100000));
    dumpScreen("v10A_06_result");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* A7/7 点解题看过程：真扣卡接口 + 公式清洗 + 顶部不截断 */
    jianti->debugClick("solve");
    vTaskDelay(pdMS_TO_TICKS(5000));
    dumpScreen("v10A_07_process");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* 回首页，准备 B 遍 */
    jianti->debugClick("recap");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ================= B 遍：假拍（初一题库抽题） ================= */
    printf("===JT_AUTOTEST_PASS B fake===\n");
    fflush(stdout);

    /* B1/7 首页 */
    printf("===JT_AUTOTEST_STEP B1/7 home===\n");
    fflush(stdout);
    dumpScreen("v10B_01_home");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* B2/7 服务器页 */
    jianti->debugClick("server");
    vTaskDelay(pdMS_TO_TICKS(1200));
    dumpScreen("v10B_02_server");
    vTaskDelay(pdMS_TO_TICKS(1800));

    /* B3/7 服务器页「测试连接」 */
    jianti->debugClick("test");
    vTaskDelay(pdMS_TO_TICKS(8000));
    dumpScreen("v10B_03_server_test");
    vTaskDelay(pdMS_TO_TICKS(1800));
    jianti->debugClick("back");
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* B4/7 取景器 */
    jianti->debugClick("capture");
    vTaskDelay(pdMS_TO_TICKS(8000));
    dumpScreen("v10B_04_viewfinder");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* B5/7 静态确认 */
    jianti->debugClick("shutter");
    vTaskDelay(pdMS_TO_TICKS(kStillWaitMs));
    dumpScreen("v10B_05_still");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* B6/7 结果页：注入合成回包（不点 still_ok、不真上传） */
    jianti->debugFakePaper(kFakePaperJson);
    vTaskDelay(pdMS_TO_TICKS(1500));
    dumpScreen("v10B_06_result");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* B7/7 过程与答案：跳过扣卡直接展开 */
    jianti->debugUnlockStep();
    vTaskDelay(pdMS_TO_TICKS(2500));
    dumpScreen("v10B_07_process");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* 回首页停住，不再出重复图 */
    jianti->debugClick("recap");
    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("===JT_AUTOTEST_DONE v10_material===\n");
    fflush(stdout);
    ESP_LOGI(TAG, "BOARD V10 MATERIAL DONE: 14 shots (7 real + 7 fake), parked at home");
    delete ctx;
    vTaskDelete(NULL);
#endif // JIANTI_FONT_COMPARE
}

} // namespace

void jianti_debug_start(esp_brookesia::systems::phone::Phone *phone, int app_id)
{
    auto *ctx = new(std::nothrow) Ctx();
    if (!ctx) {
        return;
    }
    ctx->phone = phone;
    ctx->app_id = app_id;

    /* 给触摸设备挂坐标日志：五点校验与手势复测都从这里读 */
    {
        bsp_extra_display_lock(-1);
        for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev;
             indev = lv_indev_get_next(indev)) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
                lv_indev_add_event_cb(indev, touchLogger, LV_EVENT_PRESSED, nullptr);
                lv_indev_add_event_cb(indev, touchLogger, LV_EVENT_RELEASED, nullptr);
            }
        }
        bsp_extra_display_unlock();
    }

    xTaskCreate(autotestTask, "jianti_autotest", 12 * 1024, ctx, 3, nullptr);
}

#endif // CONFIG_JIANTI_DEBUG_AUTOTEST
