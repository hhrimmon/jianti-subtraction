/* JianTi Phase 1 firmware v0.1 - camera baseline
 * Base: Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 example 09_video_lcd_display (CC0-1.0)
 * Pipeline: OV5647 RAW8 -> P4 ISP -> RGB -> PPA -> LCD preview (unchanged from example)
 * Added by JianTi:
 *   - serial console command "cap"  : capture one frame
 *   - hardware JPEG encode of the captured frame
 *   - base64 dump of the JPEG over UART between JTJPEG_BEGIN / JTJPEG_END markers
 *   - optional auto-capture once after boot (Kconfig)
 * v0.2 adds (uplink.c, main.c hooks):
 *   - serial console command "up"   : capture + WiFi upload gray JPEG to gateway
 *   - serial console command "back" : return from result screen to preview
 *   - result screen shows an ASCII receipt; full answer text goes to serial log
 *     (panel has no CJK font in v0.2; CJK on-screen rendering is v0.3 work)
 * v0.3 prov adds (prov.c): NVS home-WiFi creds, SoftAP + web-form setup
 *   mode, on-screen setup screen, serial prov/wifistat commands.
 */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ppa.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include "esp_console.h"
#include "app_video.h"
#include "uplink.h"
#include "prov.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"

#include "lvgl.h"
#include "lv_demos.h"

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

static const char *TAG = "jt_main";

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *lcd_buffer[CONFIG_BSP_LCD_DPI_BUFFER_NUMS];
static lv_display_t *disp;

i2c_master_bus_handle_t i2c_bus_;

static bool dummy_draw_enabled = true;
static bool dummy_mode_delay_flag = false;

/* ---------------- JianTi capture state ---------------- */
static uint8_t *s_snapshot = NULL;          /* PSRAM frame copy waiting to be encoded */
static size_t s_snapshot_cap = 0;
static uint32_t s_snap_w = 0;
static uint32_t s_snap_h = 0;
static SemaphoreHandle_t s_snap_sem = NULL;
static volatile bool s_capture_requested = false;
static volatile bool s_capture_for_upload = false; /* v0.2: this capture feeds uplink */
static volatile bool s_snap_busy = false;
static int s_jpeg_quality = CONFIG_JT_JPEG_QUALITY;

static void free_camera_buffers(void **buffers, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (buffers[i] != NULL) {
            heap_caps_free(buffers[i]);
            buffers[i] = NULL;
        }
    }
}

static void calc_ppa_input_offset(uint32_t src_w, uint32_t src_h,
                                  uint32_t dst_w, uint32_t dst_h,
                                  uint32_t *offset_x, uint32_t *offset_y)
{
    *offset_x = (src_w > dst_w) ? (src_w - dst_w) / 2 : 0;
    *offset_y = (src_h > dst_h) ? (src_h - dst_h) / 2 : 0;

    if ((*offset_x + dst_w) > src_w)
    {
        *offset_x = 0;
    }
    if ((*offset_y + dst_h) > src_h)
    {
        *offset_y = 0;
    }
}

/* ---------------- JianTi: JPEG encode + UART dump ---------------- */
static void capture_dump_task(void *arg)
{
    jpeg_encoder_handle_t enc = NULL;
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };

    while (true) {
        if (xSemaphoreTake(s_snap_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t w = s_snap_w;
        uint32_t h = s_snap_h;

        /* Convert RGB565 snapshot -> GRAY8 for clean grayscale JPEG (homework / e-ink) */
        size_t gray_len = (size_t)w * h;
        uint8_t *gray_buf = heap_caps_malloc(gray_len, MALLOC_CAP_SPIRAM);
        if (!gray_buf) {
            ESP_LOGE(TAG, "gray buffer alloc failed");
            s_snap_busy = false;
            continue;
        }
        const uint16_t *src = (const uint16_t *)s_snapshot;
        uint8_t *dst = gray_buf;
        for (size_t i = 0; i < gray_len; i++) {
            uint16_t px = src[i];
            uint32_t r = (px >> 11) & 0x1F;
            uint32_t g = (px >> 5) & 0x3F;
            uint32_t b = px & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            dst[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        }

        if (jpeg_new_encoder_engine(&eng_cfg, &enc) != ESP_OK) {
            ESP_LOGE(TAG, "jpeg engine acquire failed");
            heap_caps_free(gray_buf);
            s_snap_busy = false;
            continue;
        }

        size_t out_cap = (size_t)w * h; /* generous upper bound for q<=95 stills */
        size_t jpeg_alloc_sz = 0;
        uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, NULL, &jpeg_alloc_sz);
        if (!out_buf) {
            ESP_LOGE(TAG, "jpeg out buffer alloc failed");
            jpeg_del_encoder_engine(enc);
            heap_caps_free(gray_buf);
            s_snap_busy = false;
            continue;
        }

        jpeg_encode_cfg_t cfg = {
            .src_type = JPEG_ENCODE_IN_FORMAT_GRAY,
            .sub_sample = JPEG_DOWN_SAMPLING_GRAY,
            .width = w,
            .height = h,
            .image_quality = (uint8_t)s_jpeg_quality,
        };
        uint32_t out_len = 0;
        esp_err_t err = jpeg_encoder_process(enc, &cfg, gray_buf, gray_len,
                                             out_buf, out_cap, &out_len);
        jpeg_del_encoder_engine(enc);
        heap_caps_free(gray_buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jpeg encode failed: %s", esp_err_to_name(err));
            heap_caps_free(out_buf);
            s_snap_busy = false;
            continue;
        }
        ESP_LOGI(TAG, "jpeg ok: %ux%u q=%d -> %u bytes", w, h, s_jpeg_quality, (unsigned)out_len);

        /* v0.2: hand the JPEG to the upload task (it copies synchronously) */
        jt_uplink_offer_jpeg(out_buf, out_len);
        if (s_capture_for_upload) {
            s_capture_for_upload = false;
            jt_uplink_kick();
        }

        /* base64-encode whole JPEG, then stream it out between markers */
        size_t b64_cap = ((out_len + 2) / 3) * 4 + 64;
        unsigned char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
        if (!b64) {
            ESP_LOGE(TAG, "b64 buffer alloc failed");
            heap_caps_free(out_buf);
            s_snap_busy = false;
            continue;
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(b64, b64_cap, &b64_len, out_buf, out_len);
        heap_caps_free(out_buf);

        esp_log_level_set("*", ESP_LOG_NONE); /* keep the stream clean */
        printf("\nJTJPEG_BEGIN %u %u %u\n", (unsigned)w, (unsigned)h, (unsigned)out_len);
        const size_t CHUNK = 512;
        for (size_t off = 0; off < b64_len; off += CHUNK) {
            size_t n = (b64_len - off > CHUNK) ? CHUNK : (b64_len - off);
            printf("%.*s\n", (int)n, (const char *)(b64 + off));
        }
        printf("JTJPEG_END\n");
        fflush(stdout);
        esp_log_level_set("*", ESP_LOG_INFO);
        heap_caps_free(b64);
        ESP_LOGI(TAG, "dump done");
        s_snap_busy = false;
    }
}

/* ---------------- JianTi: console commands ---------------- */
static int cmd_cap(int argc, char **argv)
{
    if (s_snap_busy || s_capture_requested) {
        printf("busy, try again in a few seconds\n");
        return 0;
    }
    s_capture_requested = true;
    printf("capture queued, keep the board still...\n");
    return 0;
}

static int cmd_q(int argc, char **argv)
{
    if (argc >= 2) {
        int q = atoi(argv[1]);
        if (q >= 1 && q <= 100) {
            s_jpeg_quality = q;
        }
    }
    printf("jpeg quality = %d\n", s_jpeg_quality);
    return 0;
}

/* ---------------- JianTi v0.2: upload + result screen ---------------- */
static int cmd_up(int argc, char **argv)
{
    if (jt_prov_is_setup_mode()) {
        printf("setup mode: no home WiFi yet, follow the on-screen steps first\n");
        return 0;
    }
    if (s_snap_busy || s_capture_requested) {
        printf("busy, try again in a few seconds\n");
        return 0;
    }
    s_capture_for_upload = true;
    s_capture_requested = true;
    printf("capture + upload queued, keep the board still...\n");
    return 0;
}

static int cmd_back(int argc, char **argv)
{
    jt_back_to_preview();
    printf("back to preview\n");
    return 0;
}

static int cmd_prov(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (jt_prov_erase() != ESP_OK) {
        printf("prov: erase failed\n");
        return 0;
    }
    printf("prov: saved WiFi erased, restarting into setup mode...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int cmd_wifistat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char ssid[33] = {0};
    char pass[16] = {0};
    char ip[24] = {0};
    if (jt_prov_is_setup_mode()) {
        jt_prov_ap_info(ssid, sizeof(ssid), pass, sizeof(pass), ip, sizeof(ip));
        (void)pass;
        printf("mode=setup ap=%s ip=%s saved=%s\n", ssid, ip,
               jt_prov_has_saved_creds() ? "yes" : "no");
    } else {
        printf("mode=STA saved=%s (use prov to re-provision)\n",
               jt_prov_has_saved_creds() ? "yes" : "no");
    }
    return 0;
}

/* v0.3: the result screen shows the feedback text itself in a CJK font
 * (jt_font_28, 28px, generated from Noto Sans CJK SC with expanded symbols). Without the font file the build
 * falls back to the v0.2 ASCII receipt (see JT_HAS_CJK_FONT in CMakeLists).
 * Runs in the LVGL task via lv_async_call (thread-safe). */
#ifdef JT_HAS_CJK_FONT
extern lv_font_t jt_font_28;
#endif
#ifdef JT_HAS_BIG_BOLD_FONT
extern lv_font_t jt_font_big_bold;
#endif
static char s_result_text[2048];
static bool s_result_ok = false;
static int s_result_ms = 0;
static int s_upload_ms = 0;
static int s_answer_ms = 0;

static void show_result_cb(void *ud)
{
    (void)ud;
    dummy_draw_enabled = false;
    esp_lv_adapter_set_dummy_draw(disp, false);
    lv_obj_clean(lv_scr_act());
#ifdef JT_HAS_CJK_FONT
    /* Header: 完成 + 上传秒数+作答秒数（只要数字）。 */
    lv_obj_t *head = lv_label_create(lv_scr_act());
    char hbuf[128];
    int up_s = (s_upload_ms + 500) / 1000;
    int an_s = (s_answer_ms + 500) / 1000;
    if (s_upload_ms <= 0 && s_answer_ms <= 0) {
        /* 旧网关卡没带计时：用总耗时兜底，不显示 0+0S */
        an_s = (s_result_ms + 500) / 1000;
    }
    if (s_result_ok) {
        snprintf(hbuf, sizeof(hbuf), "完成  %d+%dS", up_s, an_s);
    } else {
        snprintf(hbuf, sizeof(hbuf), "失败");
    }
    lv_obj_set_style_text_font(head, &jt_font_28, LV_PART_MAIN);
    lv_label_set_text(head, hbuf);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 10, 4);
    /* Body: feedback text in the CJK font, wrapped, finger-scrollable. */
    lv_coord_t scr_h = lv_obj_get_height(lv_scr_act());
    lv_coord_t scr_w = lv_obj_get_width(lv_scr_act());
    lv_obj_t *box = lv_obj_create(lv_scr_act());
    lv_obj_set_size(box, scr_w - 20, scr_h - 76);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 10, 46);
    lv_obj_set_scroll_dir(box, LV_SCROLL_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_style_text_font(body, &jt_font_28, LV_PART_MAIN);
    lv_obj_set_width(body, scr_w - 20);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(body, s_result_text);
    /* Footer: how to get back to preview. */
    lv_obj_t *foot = lv_label_create(lv_scr_act());
    lv_label_set_text(foot, "type 'back' for preview");
    lv_obj_align(foot, LV_ALIGN_TOP_LEFT, 10, scr_h - 25);
#else
    lv_obj_t *label = lv_label_create(lv_scr_act());
    char buf[256];
    if (s_result_ok) {
        snprintf(buf, sizeof(buf), "Done  %d+%dS\n%u bytes\ntype 'back' for preview",
                 (s_upload_ms + 500) / 1000, (s_answer_ms + 500) / 1000,
                 (unsigned)strlen(s_result_text));
    } else {
        snprintf(buf, sizeof(buf), "Upload failed\n%s\ntype 'back' for preview", s_result_text);
    }
    lv_label_set_text(label, buf);
    lv_obj_set_width(label, scr_w - 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);
#endif
}

static void show_prov_screen_cb(void *ud)
{
    (void)ud;
    char ap_ssid[33] = {0};
    char ap_pass[16] = {0};
    char ap_ip[24] = {0};
    jt_prov_ap_info(ap_ssid, sizeof(ap_ssid), ap_pass, sizeof(ap_pass), ap_ip, sizeof(ap_ip));
    (void)ap_pass;
    (void)ap_ip;
    dummy_draw_enabled = false;
    esp_lv_adapter_set_dummy_draw(disp, false);
    lv_obj_clean(lv_scr_act());
    lv_coord_t scr_w = lv_obj_get_width(lv_scr_act());
    /* Top line: explanation (28px). Below: the hotspot SSID, big + bold. */
#ifdef JT_HAS_CJK_FONT
    lv_obj_t *hint = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(hint, &jt_font_28, LV_PART_MAIN);
    lv_obj_set_width(hint, scr_w - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(hint, "网络配置请先用手机WiFi连接");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 90);
#endif
    lv_obj_t *ssid = lv_label_create(lv_scr_act());
#if defined(JT_HAS_BIG_BOLD_FONT)
    lv_obj_set_style_text_font(ssid, &jt_font_big_bold, LV_PART_MAIN);
#elif defined(JT_HAS_BIG_FONT)
    lv_obj_set_style_text_font(ssid, &jt_font_big, LV_PART_MAIN);
#elif defined(JT_HAS_CJK_FONT)
    lv_obj_set_style_text_font(ssid, &jt_font_28, LV_PART_MAIN);
#endif
    lv_obj_set_width(ssid, scr_w - 40);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(ssid, ap_ssid);
    lv_obj_align(ssid, LV_ALIGN_CENTER, 0, 30);
}

void jt_show_prov_screen(void)
{
    lv_async_call(show_prov_screen_cb, NULL);
}

void jt_show_result(bool ok, const char *text, int latency_ms,
                    int upload_ms, int answer_ms)
{
    ESP_LOGI(TAG, "result ok=%d latency=%d ms upload=%d ms answer=%d ms text=%.200s",
             ok, latency_ms, upload_ms, answer_ms, text ? text : "");
    s_result_ok = ok;
    s_result_ms = latency_ms;
    s_upload_ms = upload_ms;
    s_answer_ms = answer_ms;
    snprintf(s_result_text, sizeof(s_result_text), "%s", text ? text : "");
    lv_async_call(show_result_cb, NULL);
}

static void back_to_preview_cb(void *ud)
{
    (void)ud;
    lv_obj_clean(lv_scr_act());
    esp_lv_adapter_set_dummy_draw(disp, true);
    dummy_draw_enabled = true;
}

void jt_back_to_preview(void)
{
    lv_async_call(back_to_preview_cb, NULL);
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "jt>";
    repl_config.max_cmdline_length = 64;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&uart_config, &repl_config, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "console repl create failed");
        return;
    }
    const esp_console_cmd_t cap_cmd = {
        .command = "cap",
        .help = "capture one frame, JPEG-encode and dump it over UART",
        .hint = NULL,
        .func = &cmd_cap,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cap_cmd));
    const esp_console_cmd_t q_cmd = {
        .command = "q",
        .help = "show/set JPEG quality, e.g. q 90",
        .hint = NULL,
        .func = &cmd_q,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&q_cmd));
    const esp_console_cmd_t up_cmd = {
        .command = "up",
        .help = "capture one frame and upload the gray JPEG to the gateway",
        .hint = NULL,
        .func = &cmd_up,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&up_cmd));
    const esp_console_cmd_t back_cmd = {
        .command = "back",
        .help = "return from result screen to preview",
        .hint = NULL,
        .func = &cmd_back,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&back_cmd));
    const esp_console_cmd_t prov_cmd = {
        .command = "prov",
        .help = "erase saved home WiFi and restart into setup mode",
        .hint = NULL,
        .func = &cmd_prov,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prov_cmd));
    const esp_console_cmd_t wifistat_cmd = {
        .command = "wifistat",
        .help = "show provisioning mode and hotspot info (never the saved password)",
        .hint = NULL,
        .func = &cmd_wifistat,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifistat_cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    /* JianTi landscape stage 1: adapter rotation 90 (UI layer).
     * Preview PPA rotation and touch flags are verified separately,
     * so they stay at their portrait values in this build. */
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 0}};
    disp = bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(esp_lv_adapter_set_dummy_draw(disp, dummy_draw_enabled));

    display_panel = bsp_display_get_panel_handle();

    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));

    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    i2c_bus_ = bsp_i2c_get_handle();

    esp_err_t ret = app_video_main(i2c_bus_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "video main init failed with err=0x%x", ret);
        return;
    }

    int video_cam_fd0 = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0)
    {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(
        display_panel, 3,
        &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));

    ESP_LOGI(TAG, "Using user defined buffer");
    void *camera_buf[2] = {NULL};
    for (int i = 0; i < 2; i++)
    {
        camera_buf[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            app_video_get_buf_size(),
            MALLOC_CAP_SPIRAM);
        if (camera_buf[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate camera buffer %d", i);
            free_camera_buffers(camera_buf, 2);
            close(video_cam_fd0);
            return;
        }
    }
    ret = app_video_set_bufs(video_cam_fd0, 2, (const void **)camera_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure camera buffers: %s", esp_err_to_name(ret));
        free_camera_buffers(camera_buf, 2);
        return;
    }

    /* JianTi: snapshot buffer + capture task + console */
    s_snapshot_cap = app_video_get_buf_size();
    s_snapshot = heap_caps_malloc(s_snapshot_cap, MALLOC_CAP_SPIRAM);
    if (!s_snapshot) {
        ESP_LOGE(TAG, "snapshot buffer alloc failed");
        free_camera_buffers(camera_buf, 2);
        return;
    }
    s_snap_sem = xSemaphoreCreateBinary();
    xTaskCreate(capture_dump_task, "jt_capture", 8192, NULL, 5, NULL);

    ret = app_video_register_frame_operation_cb(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register video frame callback: %s", esp_err_to_name(ret));
        close(video_cam_fd0);
        free_camera_buffers(camera_buf, 2);
        return;
    }

    ret = app_video_stream_task_start(video_cam_fd0, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream task: %s", esp_err_to_name(ret));
        close(video_cam_fd0);
        free_camera_buffers(camera_buf, 2);
        return;
    }

    start_console();
    jt_uplink_init();
    /* v0.3 provisioning: no creds -> SoftAP + web form + setup screen.
     * STA mode skips this entirely and goes straight to preview. */
    if (jt_prov_is_setup_mode()) {
        if (jt_prov_start_setup() == ESP_OK) {
            jt_show_prov_screen();
        } else {
            ESP_LOGE(TAG, "prov setup start failed");
        }
    }
    ESP_LOGI(TAG, "JianTi cam v0.2 ready. Type 'cap' to capture, 'up' to capture+upload, 'back' for preview.");

#if CONFIG_JT_AUTOCAP_ENABLE
    vTaskDelay(pdMS_TO_TICKS(CONFIG_JT_AUTOCAP_DELAY_MS));
    if (!s_snap_busy && !s_capture_requested) {
        s_capture_requested = true;
        ESP_LOGI(TAG, "auto capture queued");
    }
#endif
}

static void camera_video_frame_operation(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data)
{
    /* JianTi: if a capture was requested, snapshot this frame first */
    if (s_capture_requested && !s_snap_busy && s_snapshot) {
        size_t n = camera_buf_len < s_snapshot_cap ? camera_buf_len : s_snapshot_cap;
        memcpy(s_snapshot, camera_buf, n);
        s_snap_w = camera_buf_hes;
        s_snap_h = camera_buf_ves;
        s_snap_busy = true;
        s_capture_requested = false;
        xSemaphoreGive(s_snap_sem);
    }

    if (!dummy_draw_enabled || dummy_mode_delay_flag)
    {
        return;
    }

    const uint32_t display_width = BSP_LCD_H_RES;
    const uint32_t display_height = BSP_LCD_V_RES;

    if (display_height == 0)
    {
        ESP_LOGE(TAG, "Display height is zero!");
        return;
    }

    uint32_t in_offset_x = 0;
    uint32_t in_offset_y = 0;
    calc_ppa_input_offset(camera_buf_hes, camera_buf_ves,
                          display_width, display_height,
                          &in_offset_x, &in_offset_y);

    uint32_t in_block_w = (camera_buf_hes > display_width) ? display_width : camera_buf_hes;
    uint32_t in_block_h = (camera_buf_ves > display_height) ? display_height : camera_buf_ves;

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = in_block_w,
        .in.block_h = in_block_h,
        .in.block_offset_x = in_offset_x,
        .in.block_offset_y = in_offset_y,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(display_width * display_height *
                                        (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3),
                                    data_cache_line_size),
        .out.pic_w = display_width,
        .out.pic_h = display_height,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .mirror_x = 0,
        .mirror_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PPA SRM failed: %d", ret);
        return;
    }

    uint32_t draw_w = (camera_buf_hes > display_width) ? display_width : camera_buf_hes;
    uint32_t draw_h = (camera_buf_ves > display_height) ? display_height : camera_buf_ves;

    if (dummy_draw_enabled && !dummy_mode_delay_flag)
    {
        ret = esp_lv_adapter_dummy_draw_blit(
            disp,
            0, 0,
            draw_w,
            draw_h,
            lcd_buffer[camera_buf_index],
            true);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Dummy draw blit failed: %d", ret);
        }
    }
}
