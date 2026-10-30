/*
 * 简题 App（JianTi）· 最小原型
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一个 Brookesia App：大拍题按钮 -> 摄像头预览（复用 Camera App 的 dummy draw 顺序）
 * -> 拍一张 -> 发本地网关 /solve -> 顶部显示解题文本。
 * 只做最小闭环，不做卡、批改、管理。
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "driver/ppa.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

namespace esp_brookesia::apps
{

class JianTi : public systems::phone::App
{
public:
    static JianTi *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~JianTi();

    /* 开机自联网：用 NVS 已存凭据建 STA，不依赖系统 Settings/WLAN。
     * 由文件内静态任务函数调用，故为 public。 */
    void wifiBootStart(void);

#if CONFIG_JIANTI_DEBUG_AUTOTEST
    /* 远程联调：等价于点一下"拍题"按钮 */
    void debugTriggerCapture(void);
    /* 远程联调：等价于点一下"服务器"入口 */
    void debugOpenServer(void);
    /* 远程联调：往按钮上注入 LV_EVENT_CLICKED，走真实回调路径（不是物理触摸） */
    void debugClick(const char *which);
    /* 远程联调：运行时改 OV5647 AE 目标（2..235，默认 0x50），只用于取证扫描 */
    bool debugSetAeTarget(int v);
    /* 远程联调：逐档实测 OV5647 支持的分辨率（尺寸/帧率/缓冲），串口打清单 */
    bool debugCamProbe(void);
    /* 远程联调：切取景器铺满方式 0=V7原样 1=裁切铺满 2=拉伸铺满 3=等比留边 */
    void debugSetFillMode(int mode);
    /* 远程联调：静态确认页是否也用 800x480 铺满 */
    void debugSetStillFill(bool on);
    /* 远程联调：预览节流间隔（ms），0=不节流（测帧率上限用） */
    void debugSetPreviewInterval(int ms);
    /* 远程联调：假拍 —— 直接把合成回包注入结果页（不走相机、不走上传） */
    void debugFakePaper(const char *json);
    /* 远程联调：假拍 —— 跳过网关扣卡，直接解锁过程卡 */
    void debugUnlockStep(void);
#endif

protected:
    JianTi(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    static JianTi *_instance;

    static constexpr int CAMERA_BUFFER_COUNT = 2;
    static constexpr int CAPTURE_TASK_STACK_SIZE = 16 * 1024;
    static constexpr int CAPTURE_TASK_PRIORITY = 8;
    /* 预览多少帧后才受理快门（让 AE 先稳一下，避免黑图） */
    static constexpr int PREVIEW_WARMUP_FRAMES = 40;
    /* 取景帧节流：约 25fps */
    static constexpr int PREVIEW_FRAME_INTERVAL_MS = 40;

    /* --- UI（800x480 横屏，五页：主屏 / 取景器 / 静态确认 / 结果页 / 服务器选择页） --- */
    enum class Page { Home, Viewfinder, Still, Result, Server };

    lv_obj_t *_page_home = nullptr;
    lv_obj_t *_page_view = nullptr;    /* 取景器：上预览、下大拍摄按钮 */
    lv_obj_t *_page_still = nullptr;   /* 静态图确认：放大图 + 确认/取消 */
    lv_obj_t *_page_result = nullptr;
    lv_obj_t *_page_server = nullptr;

    lv_obj_t *_status_label = nullptr;    /* 主屏：状态字（v8 起不再有"完成"，仅作状态提示，不可点） */
    lv_obj_t *_answer_box = nullptr;      /* 结果页：竖向滚动区（思路卡 + 过程卡） */
    lv_obj_t *_answer_label = nullptr;    /* 结果页：思路卡文本 */
    lv_obj_t *_step_label = nullptr;      /* 结果页：过程与答案卡（解锁后显示） */
    lv_obj_t *_pager_label = nullptr;     /* 结果页：n/N（ASCII） */
    lv_obj_t *_server_name_label = nullptr; /* 主屏：当前服务器名（24px 中文：内网服务器/公网服务器） */

    lv_obj_t *_btn_capture = nullptr;     /* 主屏：拍题大圆 */
    lv_obj_t *_btn_server_entry = nullptr;/* 主屏：服务器入口 */
    lv_obj_t *_btn_back = nullptr;        /* 结果页：返回 */
    lv_obj_t *_btn_recap = nullptr;       /* 结果页：再拍一题 */
    lv_obj_t *_btn_solve = nullptr;       /* 结果页：顶部正中「解题」pill（测试版只解锁过程卡，不扣卡） */
    lv_obj_t *_btn_prev = nullptr;        /* 结果页：上一题 */
    lv_obj_t *_btn_next = nullptr;        /* 结果页：下一题 */
    lv_obj_t *_btn_srv_back = nullptr;    /* 服务器页：返回 */
    lv_obj_t *_btn_lan = nullptr;         /* 服务器页：内网服务器 */
    lv_obj_t *_btn_cloud = nullptr;       /* 服务器页：公网服务器 */
    lv_obj_t *_btn_test = nullptr;        /* 服务器页：测试连接 */
    lv_obj_t *_btn_reconfig = nullptr;    /* 服务器页：重新配网 */
    lv_obj_t *_label_test_result = nullptr; /* 服务器页：测试结果 */

    /* --- 倒计时拍摄 --- */
    lv_obj_t *_countdown_label = nullptr;  /* 结果页：倒计时大字 3/2/1 */
    lv_obj_t *_shot_img = nullptr;         /* 结果页：刚拍的静态图 */
    lv_obj_t *_btn_confirm = nullptr;      /* 结果页：确认发送 */
    lv_obj_t *_btn_cancel = nullptr;       /* 结果页：取消重拍 */
    lv_img_dsc_t _shot_dsc = {};           /* 静态图描述 */
    bool _shot_ready = false;              /* 拍好待确认 */

    /* --- 取景器页（LVGL 层显示预览，不暂停 LVGL） --- */
    lv_obj_t *_preview_img = nullptr;      /* 实时预览图（v8 起 800x480 铺满） */
    lv_obj_t *_btn_shutter = nullptr;      /* 右下角小圆「拍照」 */
    lv_obj_t *_btn_view_back = nullptr;    /* 左下角小圆「返回」 */
    lv_obj_t *_view_status = nullptr;      /* 预览左上角状态字 */

    /* --- 静态确认页 --- */
    lv_obj_t *_still_img = nullptr;        /* 放大后的静态图（v8 起 800x480 铺满画布） */
    lv_obj_t *_btn_still_ok = nullptr;     /* 确认（编码静态图上传网关，出答案） */
    lv_obj_t *_btn_still_cancel = nullptr; /* 取消（弃图回取景器） */
    lv_obj_t *_still_hint = nullptr;       /* 静态页提示字 */

    uint8_t _server_id = 0;               /* 0=内网服务器 1=公网服务器 */
    int _q_index = 1;                     /* 当前题号（1 起） */
    int _q_total = 1;                     /* 题数（按 8025 卡 title 的题键去重后计数） */
    lv_coord_t _edge_press_y = 0;         /* 顶边缘下滑起点 */

    /* --- 结果页按题渲染（解析 8025 /solve 回包的 cards，一题一页） ---
     * 文字一律逐字取自卡里的 title/text，板端不改字、不拼行。 */
    static constexpr int JIANTI_MAX_CARDS = 16;
    static constexpr int JIANTI_CARD_TITLE_MAX = 48;
    static constexpr int JIANTI_CARD_ARENA = 24 * 1024;  /* PSRAM：卡 text 原文 */
    static constexpr int JIANTI_VIEW_CAP = 8 * 1024;     /* PSRAM：拼给 lv_label 的显示串 */

    struct Card {
        char title[JIANTI_CARD_TITLE_MAX];
        const char *text;   /* 指向 _card_arena，逐字原文 */
        int qkey;           /* 第N题 -> N*100；第N题(k) -> N*100+k（小题算独立题） */
        bool is_step;       /* true = 过程与答案卡，false = 思路卡 */
    };

    Card _cards[JIANTI_MAX_CARDS] = {};
    int _card_count = 0;
    int _qkeys[JIANTI_MAX_CARDS] = {};   /* 去重后的题键，按出现顺序 */
    int _qkey_count = 0;
    bool _q_unlocked = false;            /* 当前题是否已点解题（已解锁：只收起/展开，不重复扣卡） */
    char *_card_arena = nullptr;         /* PSRAM */
    size_t _card_arena_used = 0;
    char *_view_buf = nullptr;           /* PSRAM：思路卡显示串 */
    char *_step_buf = nullptr;           /* PSRAM：过程与答案卡显示串 */

    /* --- 扣解题卡（8026 卡账本测试版） ---
     * 设备号：NVS 有就复用，无则用 WiFi MAC 生成一次存 NVS，以后不变。
     * 解题 -> /card/unlock；没卡 -> 双入口（向家长申请 / 自己来预支）。 */
    char _device_id[32] = {};
    int _card_last_remaining = -1;       /* 最近一次扣卡/预支后的余额（串口与弹窗用） */
    lv_obj_t *_card_mask = nullptr;      /* 结果页上的半透明遮罩（弹窗容器） */
    lv_obj_t *_card_panel = nullptr;     /* 白色圆角卡 */
    lv_obj_t *_card_title = nullptr;     /* 弹窗标题 */
    lv_obj_t *_card_body = nullptr;      /* 弹窗正文（申请/预支提醒） */
    lv_obj_t *_btn_card_apply = nullptr;     /* 「向家长申请」 */
    lv_obj_t *_btn_card_preborrow = nullptr; /* 「自己来预支」 */
    lv_obj_t *_btn_card_confirm = nullptr;   /* 预支确认「确认」 */
    lv_obj_t *_btn_card_cancel = nullptr;    /* 预支确认「取消」 */
    lv_obj_t *_btn_card_close = nullptr;     /* 「关闭」 */
    lv_obj_t *_card_toast_label = nullptr;   /* 发送成功/失败提示 */
    lv_timer_t *_card_toast_timer = nullptr;
    int _card_panel_mode = -1;           /* -1=隐藏 0=双入口 1=预支确认 2=连不上服务器 */

    void loadDeviceId(void);
    const char *deviceId(void) const { return _device_id; }
    /* POST 网关 /card/<path>，带 X-JT-Device；返回 HTTP 状态（0=网络失败），resp 收 JSON */
    int cardPost(const char *path, const char *body, char *resp, size_t resp_sz);
    /* 点解题时问 server：true=已解锁可显示过程；false=没卡（已弹双入口）或失败 */
    bool cardUnlock(void);
    void doCardApply(void);
    void doCardPreborrow(void);
    void showCardPanel(int mode);
    void hideCardPanel(void);
    void showCardToast(const char *text);
    static void onCardApplyClicked(lv_event_t *e);
    static void onCardPreborrowClicked(lv_event_t *e);
    static void onCardConfirmClicked(lv_event_t *e);
    static void onCardCancelClicked(lv_event_t *e);
    static void onCardCloseClicked(lv_event_t *e);
    static void cardToastTimerCb(lv_timer_t *t);

    const char *gatewayUrl(void) const;
    void loadServerPref(void);
    void saveServerPref(uint8_t id);
    void showPage(Page page);

    static void onServerEntryClicked(lv_event_t *e);
    static void onLanClicked(lv_event_t *e);
    static void onCloudClicked(lv_event_t *e);
    static void onBackClicked(lv_event_t *e);
    static void onRecapClicked(lv_event_t *e);
    static void onPrevClicked(lv_event_t *e);
    static void onNextClicked(lv_event_t *e);
    static void onTopEdgePressed(lv_event_t *e);
    static void onTopEdgeReleased(lv_event_t *e);
    static void onTestClicked(lv_event_t *e);
    static void onReconfigClicked(lv_event_t *e);
    static void onConfirmClicked(lv_event_t *e);
    static void onCancelClicked(lv_event_t *e);
    static void onShutterClicked(lv_event_t *e);
    static void onViewBackClicked(lv_event_t *e);
    static void onStillOkClicked(lv_event_t *e);
    static void onStillCancelClicked(lv_event_t *e);
    static void onSolveClicked(lv_event_t *e);
    void updatePager(void);
    /* 结果页按题渲染：解析 8025 卡 -> 一题一页 -> 解题解锁过程卡 */
    void resetCards(void);
    void parseCards(char *json);
    void rebuildQuestionIndex(void);
    void renderQuestion(void);
    void showResultText(const char *fallback_text);
    bool toggleSolveUnlock(void);
    void showShotConfirm(void);
    void clearShotConfirm(void);
    void doServerTest(void);
    void doWifiReconfig(void);

    /* --- 取景器 / 静态确认（LVGL 层，PPA 输出写 PSRAM） --- */
    bool enterViewfinder(void);
    void leaveViewfinder(void);
    esp_err_t computePreviewGeometry(void);
    esp_err_t preparePreviewTargets(void);
    esp_err_t renderPreviewFrame(const void *camera_buf);
    esp_err_t renderStillFrame(const void *camera_buf);
    void releasePreviewTargets(void);

    uint8_t *_preview_buf[2] = {};
    size_t _preview_buf_size = 0;
    lv_img_dsc_t _preview_dsc[2] = {};
    int _preview_idx = 0;
    uint32_t _preview_w = 0;
    uint32_t _preview_h = 0;
    size_t _preview_block_w = 0;      /* PPA 输入裁块宽（旋转前） */
    size_t _preview_block_h = 0;
    size_t _preview_block_off_x = 0;
    size_t _preview_block_off_y = 0;
    float _preview_scale = 0.0f;

    /* --- 铺满方式（cam_fill 实测批） --- */
    int _view_fill_mode = 1;          /* 0=V7原样 1=裁切 2=拉伸 3=等比留边（v8 正式版默认 1=裁切铺满） */
    bool _still_fill_480 = true;      /* 静态确认页是否也按 800x480 铺满（v8 正式版默认开） */
    float _preview_scale_x = 0.0f;    /* PPA 横/纵缩放（拉伸模式下可不等比） */
    float _preview_scale_y = 0.0f;
    uint32_t _ppa_out_w = 0;          /* PPA 实际输出尺寸（旋转后） */
    uint32_t _ppa_out_h = 0;
    uint32_t _preview_out_off_x = 0;  /* 输出图在 800x480 画布上的偏移 */
    uint32_t _preview_out_off_y = 0;
    int _preview_interval_ms = PREVIEW_FRAME_INTERVAL_MS;
    uint8_t *_stretch_tmp = nullptr;  /* 拉伸模式：PPA 中间缓冲 */
    size_t _stretch_tmp_size = 0;

    uint8_t *_still_buf = nullptr;
    size_t _still_buf_size = 0;
    lv_img_dsc_t _still_dsc = {};
    uint32_t _still_w = 0;
    uint32_t _still_h = 0;

    volatile bool _viewfinder_active = false; /* 在拍题链路里（取景器或静态页） */
    volatile bool _still_shown = false;       /* 当前停在静态确认页，暂停预览刷新 */
    volatile bool _shutter_request = false;   /* 点了拍摄，等下一帧受理 */

    /* --- 摄像头 / PPA --- */
    lv_display_t *_display = nullptr;
    esp_lcd_panel_handle_t _display_panel = nullptr;
    ppa_client_handle_t _ppa_srm_handle = nullptr;
    esp_lcd_touch_handle_t _touch_handle = nullptr;
    void *_lcd_buffers[CONFIG_BSP_LCD_DPI_BUFFER_NUMS] = {};
    int _current_lcd_buffer_index = 0;
    void *_camera_buffers[CAMERA_BUFFER_COUNT] = {};
    size_t _camera_buffer_lengths[CAMERA_BUFFER_COUNT] = {};
    size_t _camera_buffer_size = 0;
    uint32_t _camera_width = 0;
    uint32_t _camera_height = 0;
    size_t _data_cache_line_size = 64;
    int _video_fd = -1;
    bool _video_driver_initialized = false;
    bool _dummy_enabled = false;
    bool _lvgl_paused = false;

    /* --- 抓拍 / 上传 --- */
    TaskHandle_t _capture_task_handle = nullptr;
    volatile bool _capture_running = false;
    uint8_t *_snapshot = nullptr;   /* RGB565 原图 */
    size_t _snapshot_len = 0;
    size_t _snapshot_cap = 0;

    bool startCapture();
    void captureTask();
    static void captureTaskEntry(void *arg);

    esp_err_t initVideoDriver();
    esp_err_t openVideoDevice();
    esp_err_t setupCameraBuffers();
    esp_err_t startDummyPreview();
    void stopDummyPreview();
    void releaseCameraBuffers();
    esp_err_t handleFrame();
    esp_err_t grabFrame();

    bool encodeJpeg(uint8_t **out, size_t *out_len);
    bool encodeJpegRgb565(const uint8_t *src, uint32_t w, uint32_t h, uint8_t **out, size_t *out_len);
    bool uploadJpeg(const uint8_t *jpeg, size_t len, char *text, size_t text_sz);
    bool wifiEnsureConnected();
    /* 幂等：复用既有 STA netif（绝不重建）。wait_ms>0 时最多等这么久拿 IP。 */
    bool wifiStartSta(uint32_t wait_ms);
    bool serverHealthCheck(const char *url);

    void buildUi();
    void setAnswer(const char *text);
    void setStatus(const char *text);

    static void onCaptureClicked(lv_event_t *e);
};

} // namespace esp_brookesia::apps
