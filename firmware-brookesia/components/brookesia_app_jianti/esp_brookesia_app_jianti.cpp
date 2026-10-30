/*
 * 简题 App（JianTi）· 最小原型实现
 * SPDX-License-Identifier: Apache-2.0
 *
 * 结构与 Camera App 保持一致：独立任务里开 v4l2 -> dummy draw 预览 -> 抓帧
 * -> 停预览/释放缓冲 -> 回 LVGL 线程更新界面。
 * 摄像头预览、PPA 缩放、dummy draw 的开关顺序照抄 Camera App，不另写一套。
 */
#include "esp_brookesia_app_jianti.hpp"

#include "lvgl.h"
#include "esp_brookesia.hpp"

#include "bsp/touch.h"
#include "esp_cache.h"
#include "esp_lib_utils.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_cam_sensor.h"
/* 注意：linux/videodev2.h 必须放在所有 include 之后（见文件末尾）。
 * esp_http_client.h → lwip/sockets.h 会用 BSD 约定（IOC_OUT/IOC_IN）
 * 重定义 _IOR/_IOW/_IO，覆盖 esp_video linux/ioctl.h 的 Linux 约定，
 * 导致 VIDIOC_QUERYCAP 等 _IOR 类 ioctl 方向位错误 → errno=22。
 * 把 videodev2.h 放最后，让 esp_video 的 _IOR 最终生效。 */

#include "driver/jpeg_encode.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "esp_netif_ip_addr.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

/* 必须放在所有 include 之后：覆盖 lwip/sockets.h 的 BSD 约定 _IOR/_IOW/_IO */
#include "linux/videodev2.h"
/* esp_video 私有 ioctl（VIDIOC_S_SENSOR_FMT / VIDIOC_S_DQBUF_TIMEOUT）；
 * 放最后，保证宏展开时用的是 Linux 约定 _IOWR。 */
#include "esp_video_ioctl.h"

/* esp_video 未在公共头导出（实现于 esp_video_csi_device.c），
 * app 侧自声明以取 OV5647 sensor 句柄做逐档实测。 */
extern "C" esp_cam_sensor_device_t *esp_video_get_csi_video_device_sensor(void);

#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:JianTi"

LV_IMG_DECLARE(img_app_jianti);
/* 本批「20/24/36/44 全加粗」四档字库。
 * 均由 fontTools 把 78__xiaozhi-fonts/ttf/puhui-common.ttf 的轮廓沿 X 再描一遍（真·加粗，
 * 不是 label 叠字），再交 lv_font_conv 烘焙：
 *   44px bpp4 仅「拍题」2 字 —— 首页拍题大圆
 *   36px bpp4 仅「简题」2 字 —— 首页标题
 *   24px bpp2 3792 字（含全部中文 + ASCII）—— 正文/副标题/提示/按钮正文
 *   20px bpp4 95 个 ASCII —— 页码 1/1、倒计时数字（v8 起服务器名改中文 24px，不再用此字重）
 * 全页已无 14/16/18px，最小 20px；旧 font_puhui_14_1 / font_jt_cn_20_1 /
 * font_jianti_btn30 / btn38 / btn76 全部退役。 */
LV_FONT_DECLARE(font_jt_b_36_4);
LV_FONT_DECLARE(font_jt_b_44_4);
LV_FONT_DECLARE(font_jt_b_24_2);
LV_FONT_DECLARE(font_jt_b_20_4);


#define APP_NAME "JianTi"

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB565
#define JIANTI_VIDEO_FMT V4L2_PIX_FMT_RGB565
#define JIANTI_PPA_COLOR_MODE PPA_SRM_COLOR_MODE_RGB565
#define JIANTI_BYTES_PER_PIXEL 2
#elif CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#define JIANTI_VIDEO_FMT V4L2_PIX_FMT_RGB24
#define JIANTI_PPA_COLOR_MODE PPA_SRM_COLOR_MODE_RGB888
#define JIANTI_BYTES_PER_PIXEL 3
#else
#error "Unsupported BSP LCD color format"
#endif

/* 字体入口：正文/按钮/提示一律 24px 加粗，首页标题 36px 加粗，首页拍题 44px 加粗，纯西文 20px 加粗 */
#define JIANTI_CN_FONT (&font_jt_b_24_2)
#define JIANTI_TITLE_FONT (&font_jt_b_36_4)
#define JIANTI_CAPTURE_FONT (&font_jt_b_44_4)
#define JIANTI_ASCII_FONT (&font_jt_b_20_4)

/* 取景/静态图几何（LVGL 逻辑空间 800x480）
 * 相机原生 800x1280 竖图；板子按横屏使用，故 PPA 先逆时针转 90°。
 * 若实机发现方向不对，只改下面这两个宏（90 <-> 270）与镜像位。
 * 取景：源中心裁 576x1280 -> 转 90 -> 1280x576 -> 0.625 -> 800x360（不拉伸）
 * 静态：取与取景同一块中心裁切（保持"所见即所得"的放大倍率），
 *       576x1280 -> 转 90 -> 1280x576 -> contain 800x392 -> 800x360（offset y=16）
 *       注意：不再用整帧 1280x800（那只能出 560x350，题字偏小）
 */
#define JIANTI_PREVIEW_ROTATION PPA_SRM_ROTATION_ANGLE_90
#define JIANTI_PREVIEW_MIRROR_X 0
#define JIANTI_PREVIEW_MIRROR_Y 0

#define JIANTI_VIEW_W 800
#define JIANTI_VIEW_H 360     /* 上方预览区（本批已删掉下方 120px 黑条，按钮浮在页面上） */
#define JIANTI_VIEW_SCALE_STEPS 10   /* 10/16 = 0.625 */
#define JIANTI_STILL_W 800
#define JIANTI_STILL_H 392    /* 静态图区（本批已删掉下方 88px 黑条，按钮浮在图上） */

/* cam_fill 实测批：铺满 800x480 的三种办法（mode 0 = V7 原样 800x360）
 * 相机 800x1280 竖图，PPA 旋转 90 后源为 1280x800；PPA 缩放按 1/16 量化。
 *   mode 1 裁切铺满：block 768x1280 @10/16 -> 800x480，无形变，源宽左右各裁 16px
 *   mode 2 拉伸铺满：block 800x1280 @10/16 -> 800x500，CPU 竖向压到 480（约 4% 变形）
 *   mode 3 等比留边：block 800x1280 @9/16  -> 720x450，居中留边（左右 40、上下 15）
 */
#define JIANTI_FILL_W 800
#define JIANTI_FILL_H 480

/* 取景器左下「返回」/ 右下「拍照」小圆：v8 视觉直径 106（96 +10%），点击区每边再外扩 22（触摸区 150） */
#define JIANTI_CORNER_BTN_D 106
#define JIANTI_CORNER_BTN_EXT 22

/* 浮层按钮底色不透明度：85%（v8 定稿，70% 作废）。LVGL 没有 LV_OPA_85，按 255*0.85 取 217 */
#define JIANTI_FLOAT_OPA ((lv_opa_t)217)

/* 首页拍题大圆：本批恢复 300（上一批字体放大时缩到 260 不接受）。
 * 圆内「拍题」走 24px 加粗正文档（原 76px 烘焙图已退役），容器画圆不占 flash。 */
#define JIANTI_CAPTURE_D 300

/* 服务器页两张白卡：本批从 380x100 收到 260x76 的「正常比例」，
 * 内网服务器/公网服务器 只做二选一，不做任何输入框。 */
#define JIANTI_SRV_CARD_W 260
#define JIANTI_SRV_CARD_H 76


/* 配网凭据：先读简题固件留下的 NVS（namespace "jt"，键 ssid/pass），
 * 没有再用 Kconfig 兜底。Settings App 的 WlanPage 写的是 esp_wifi 自己的
 * nvs（net80211 命名空间），两边互不影响。 */
#define JIANTI_NVS_NS "jt"
#define JIANTI_NVS_KEY_SSID "ssid"
#define JIANTI_NVS_KEY_PASS "pass"
/* 服务器选择：0=内网服务器（Kconfig 里的局域网网关） 1=公网服务器（43.156.81.191:8000） */
#define JIANTI_NVS_KEY_SRV "srv"
/* 设备号：NVS 有就复用，无则用 WiFi MAC 生成一次存下，以后不再变 */
#define JIANTI_NVS_KEY_DEVID "devid"

/* WiFi 等待窗口：ESP-Hosted(remote) 关联较慢，单次给足 45s */
#define JIANTI_WIFI_BOOT_WAIT_MS 45000
#define JIANTI_WIFI_WAIT_MS 30000
/* 公网服务器：与内网同端口 8000（云上旧 8080 网关已停用，禁止改回 8080） */
#define JIANTI_GW_CLOUD "http://43.156.81.191:8000/solve"

#define JIANTI_RESP_CAP (16 * 1024)
#define JIANTI_HTTP_TIMEOUT_MS 120000
/* 卡接口回包很小（unlock/apply/preborrow 都是几百字节），单独给个小栈缓冲：
 * 不能用 JIANTI_RESP_CAP(16KB) 放栈上——autotest 任务栈只有 12KB，会栈溢出 panic。 */
#define JIANTI_CARD_RESP_CAP 1024

namespace esp_brookesia::apps
{

static constexpr uint32_t PPA_SCALE_STEPS_PER_UNIT = 16;

struct ContainFitConfig {
    uint32_t output_width;
    uint32_t output_height;
    uint32_t output_offset_x;
    uint32_t output_offset_y;
    float scale;
};

static ContainFitConfig computeContainFit(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h)
{
    ContainFitConfig fit = {0, 0, 0, 0, 0.0f};
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return fit;
    }

    const uint64_t nx = static_cast<uint64_t>(dst_w) * PPA_SCALE_STEPS_PER_UNIT;
    const uint64_t ny = static_cast<uint64_t>(dst_h) * PPA_SCALE_STEPS_PER_UNIT;
    uint32_t sx = static_cast<uint32_t>(nx / src_w);
    uint32_t sy = static_cast<uint32_t>(ny / src_h);
    uint32_t steps = (sx < sy) ? sx : sy;
    if (steps == 0) {
        return fit;
    }

    fit.output_width = static_cast<uint32_t>(static_cast<uint64_t>(src_w) * steps / PPA_SCALE_STEPS_PER_UNIT);
    fit.output_height = static_cast<uint32_t>(static_cast<uint64_t>(src_h) * steps / PPA_SCALE_STEPS_PER_UNIT);
    fit.output_offset_x = (dst_w - fit.output_width) / 2;
    fit.output_offset_y = (dst_h - fit.output_height) / 2;
    fit.scale = static_cast<float>(steps) / PPA_SCALE_STEPS_PER_UNIT;
    return fit;
}

/* ---------------- WiFi（幂等，复用既有 netif，不重建） ---------------- */

static volatile bool s_wifi_got_ip = false;
static volatile bool s_wifi_sta_connected = false;
/* 全流程一次性标志：netif / esp_wifi_init / 事件handler / esp_wifi_start 各只做一次 */
static bool s_netif_base_ready = false;
static bool s_wifi_inited = false;
static bool s_wifi_started = false;
static bool s_sta_handler_reg = false;
static esp_netif_t *s_sta_netif = nullptr;
/* STA netif 是否已从"默认"事件链解绑（只做一次） */
static bool s_sta_detached = false;

/* 把 STA netif 从 esp_wifi 的"默认"事件链解绑，再把 STA 驱动以非默认方式绑回同一
 * netif。原因见 wifiStartSta 顶部注释：ESP-Hosted 下一次 esp_wifi_start() 会投递
 * 多次 WIFI_EVENT_STA_START，默认 handler 每次都 netif_add，第二次必 assert 崩溃。
 * 解绑后默认 handler 找不到 netif → 不再 add；netif add 由我们自己只做一次。
 * 幂等；成功返回 true。 */
static bool jianti_detach_default_sta(esp_netif_t *sta)
{
    if (s_sta_detached) {
        return true;
    }
    if (sta == nullptr) {
        return false;
    }
    esp_err_t ed = esp_wifi_clear_default_wifi_driver_and_handlers(sta);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "detach default STA chain: %s", esp_err_to_name(ed));
    wifi_netif_driver_t drv = esp_wifi_create_if_driver(WIFI_IF_STA);
    if (drv == nullptr) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_wifi_create_if_driver(STA) failed");
        return false;
    }
    esp_err_t ea = esp_netif_attach(sta, drv);
    if (ea != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_netif_attach(STA) failed: %s", esp_err_to_name(ea));
        return false;
    }
    s_sta_detached = true;
    return true;
}

/* 查找已有的 STA netif（不新建）：默认键优先，再退化为遍历 */
static esp_netif_t *jianti_find_sta_netif(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        return sta;
    }
    for (esp_netif_t *it = esp_netif_next_unsafe(nullptr); it; it = esp_netif_next_unsafe(it)) {
        if (strncmp(esp_netif_get_ifkey(it), "WIFI_STA", 8) == 0) {
            return it;
        }
    }
    return nullptr;
}

static void jianti_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 默认 STA 事件链已被解绑（见 wifiStartSta），这里不做 netif 动作：
         * ESP-Hosted 下一次 esp_wifi_start() 会投递不止一次 STA_START，若让默认
         * handler 每次都 esp_netif_action_start → netif_add，就会撞
         * "netif already added" assert。netif add 由 wifiStartSta 自己只做一次。 */
        ESP_LOGI(ESP_UTILS_LOG_TAG, "wifi sta start (event, no netif action)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_wifi_sta_connected = true;
        /* 默认 CONNECTED 链已解绑，这里手动补上 up + 启动 DHCP 客户端 */
        if (s_sta_netif) {
            esp_netif_action_connected(s_sta_netif, base, id, data);
        }
        ESP_LOGI(ESP_UTILS_LOG_TAG, "wifi associated (ap connected)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        s_wifi_sta_connected = false;
        ESP_LOGW(ESP_UTILS_LOG_TAG, "wifi disconnected, retry");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_got_ip = true;
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(ESP_UTILS_LOG_TAG, "wifi online: got ip=" IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

/* urlencoded(%XX、+) → 明文，就地解码 */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(p[1]);
            int lo = hex(p[2]);
            if (hi >= 0 && lo >= 0) {
                *o++ = static_cast<char>((hi << 4) | lo);
                p += 2;
                continue;
            }
            *o++ = *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static bool read_nvs_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    bool ok = false;
    nvs_handle_t h;
    if (nvs_open(JIANTI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_sz;
        size_t pl = pass_sz;
        bool got_ssid = (nvs_get_str(h, JIANTI_NVS_KEY_SSID, ssid, &sl) == ESP_OK);
        bool got_pass = (nvs_get_str(h, JIANTI_NVS_KEY_PASS, pass, &pl) == ESP_OK);
        ok = got_ssid && got_pass && ssid[0] != '\0';
        nvs_close(h);
    }
    return ok;
}

/* ---------------- JSON 取值 ---------------- */

static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            if (c == 'n') {
                out[o++] = '\n';
            } else if (c == 't') {
                out[o++] = '\t';
            } else if (c == 'r') {
                out[o++] = '\r';
            } else if (c == '"' || c == '\\' || c == '/') {
                out[o++] = c;
            } else {
                out[o++] = c;
            }
            p += 2;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

/* 取一个整数字段（如 /card/unlock 的 remaining）；true=字段存在 */
static bool json_get_int(const char *json, const char *key, int *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

/* 取一个布尔字段（如 /card/unlock 的 unlocked）；true=字段存在 */
static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* ---------------- App ---------------- */

JianTi *JianTi::_instance = nullptr;

JianTi *JianTi::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new JianTi(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

JianTi::JianTi(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &img_app_jianti, true, use_status_bar, use_navigation_bar)
{
}

JianTi::~JianTi()
{
    close();
}

#if CONFIG_JIANTI_DEBUG_AUTOTEST
void JianTi::debugTriggerCapture(void)
{
    if (_capture_task_handle) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "capture already running");
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "debug trigger capture");
    bsp_extra_display_lock(-1);
    showPage(Page::Result);
    clearShotConfirm();
    lv_obj_clear_flag(_countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_countdown_label, "3");
    bsp_extra_display_unlock();
    startCapture();
}

void JianTi::debugOpenServer(void)
{
    ESP_LOGI(ESP_UTILS_LOG_TAG, "debug open server page");
    bsp_extra_display_lock(-1);
    showPage(Page::Server);
    bsp_extra_display_unlock();
}

void JianTi::debugClick(const char *which)
{
    if (!which) {
        return;
    }
    lv_obj_t *target = nullptr;
    if (strcmp(which, "server") == 0) {
        target = _btn_server_entry;
    } else if (strcmp(which, "lan") == 0) {
        target = _btn_lan;
    } else if (strcmp(which, "cloud") == 0) {
        target = _btn_cloud;
    } else if (strcmp(which, "back") == 0) {
        target = _btn_back;
    } else if (strcmp(which, "prev") == 0) {
        target = _btn_prev;
    } else if (strcmp(which, "next") == 0) {
        target = _btn_next;
    } else if (strcmp(which, "recap") == 0) {
        target = _btn_recap;
    } else if (strcmp(which, "pill") == 0 || strcmp(which, "solve") == 0) {
        target = _btn_solve;
    } else if (strcmp(which, "capture") == 0) {
        target = _btn_capture;
    } else if (strcmp(which, "confirm") == 0) {
        target = _btn_confirm;
    } else if (strcmp(which, "cancel") == 0) {
        target = _btn_cancel;
    } else if (strcmp(which, "shutter") == 0) {
        target = _btn_shutter;
    } else if (strcmp(which, "view_back") == 0) {
        target = _btn_view_back;
    } else if (strcmp(which, "still_ok") == 0) {
        target = _btn_still_ok;
    } else if (strcmp(which, "still_cancel") == 0) {
        target = _btn_still_cancel;
    } else if (strcmp(which, "test") == 0) {
        target = _btn_test;
    } else if (strcmp(which, "reconfig") == 0) {
        target = _btn_reconfig;
    } else if (strcmp(which, "card_apply") == 0) {
        target = _btn_card_apply;
    } else if (strcmp(which, "card_preborrow") == 0) {
        target = _btn_card_preborrow;
    } else if (strcmp(which, "card_confirm") == 0) {
        target = _btn_card_confirm;
    } else if (strcmp(which, "card_close") == 0) {
        target = _btn_card_close;
    } else if (strcmp(which, "card_cancel") == 0) {
        target = _btn_card_cancel;
    }
    if (!target) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "debugClick: no button '%s'", which);
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "debug inject click: %s", which);
    bsp_extra_display_lock(-1);
    lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
    bsp_extra_display_unlock();
}

bool JianTi::debugSetAeTarget(int v)
{
    if (_video_fd < 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "AE target: video fd not open");
        return false;
    }
    if (v < 2) {
        v = 2;
    } else if (v > 235) {
        v = 235;
    }

    /* esp_video 只实现 VIDIOC_S_EXT_CTRLS。ctrl_class 用 USER 才会走
     * s_sensor_control_map_table：V4L2_CID_EXPOSURE -> ESP_CAM_SENSOR_EXPOSURE_VAL
     * -> ov5647_set_AE_target()。 */
    struct v4l2_ext_control control = {};
    control.id = V4L2_CID_EXPOSURE;
    control.value = v;

    struct v4l2_ext_controls controls = {};
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = &control;

    int ret = ioctl(_video_fd, VIDIOC_S_EXT_CTRLS, &controls);
    if (ret != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "AE target -> %d failed: ret=%d errno=%d", v, ret, errno);
        return false;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "AE target -> %d (0x%02X)", v, v);
    return true;
}

void JianTi::debugSetFillMode(int mode)
{
    if (mode < 0) {
        mode = 0;
    } else if (mode > 3) {
        mode = 3;
    }
    _view_fill_mode = mode;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Fill mode -> %d (0=V7原样 1=裁切铺满 2=拉伸铺满 3=等比留边)",
             _view_fill_mode);
}

void JianTi::debugSetStillFill(bool on)
{
    _still_fill_480 = on;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Still fill480 -> %d", _still_fill_480 ? 1 : 0);
}

void JianTi::debugSetPreviewInterval(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    _preview_interval_ms = ms;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Preview interval -> %d ms (0=不节流)", _preview_interval_ms);
}

/* 假拍：把一段合成好的 /solve 回包直接灌进结果页，等价于「上传成功那一刻」。
 * 只给抓素材用（CONFIG_JIANTI_DEBUG_AUTOTEST），不进 delivery 构建。 */
void JianTi::debugFakePaper(const char *json)
{
    if (!json) {
        return;
    }
    const size_t n = strlen(json) + 1;
    /* parseCards 会就地临时改串（截断对象再还原），所以喂副本，不动调用方常量 */
    char *copy = static_cast<char *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        copy = static_cast<char *>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
    }
    if (!copy) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "fake paper: oom (%u bytes)", (unsigned)n);
        return;
    }
    memcpy(copy, json, n);

    ESP_LOGW(ESP_UTILS_LOG_TAG, "FAKE PAPER: inject synthesized resp, %u bytes (no camera/upload)",
             (unsigned)n);
    bsp_extra_display_lock(-1);
    showPage(Page::Result);
    bsp_extra_display_unlock();

    parseCards(copy);
    showResultText(_card_count > 0 ? _cards[0].text : "假拍注入失败");
    heap_caps_free(copy);
    ESP_LOGW(ESP_UTILS_LOG_TAG, "FAKE PAPER: parked at result page, %d cards", _card_count);
}

/* 假拍：跳过 /card/unlock（假拍不扣卡），直接把过程卡展开 */
void JianTi::debugUnlockStep(void)
{
    if (_card_count == 0 || _qkey_count == 0) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "fake unlock: no cards loaded, ignored");
        return;
    }
    _q_unlocked = true;
    ESP_LOGW(ESP_UTILS_LOG_TAG, "FAKE UNLOCK: q %d/%d key=%d (skip card ledger)",
             _q_index, _q_total, _qkeys[_q_index - 1]);
    renderQuestion();
}

/* 逐档实测 OV5647 在这块板子上实际能出的分辨率：
 * 走 VIDIOC_S_SENSOR_FMT 换档（CSI 未实现 ENUM_FRAMESIZES），每档真起流 2.5s 收帧，
 * 打印 cfg fps / 实测 fps / 输出尺寸 / 缓冲占用。测完恢复默认档 800x1280。 */
bool JianTi::debugCamProbe(void)
{
    printf("===JT_CAM_PROBE_BEGIN===\n");
    fflush(stdout);

    esp_err_t ret = initVideoDriver();
    if (ret == ESP_OK) {
        ret = openVideoDevice();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "CAMPROBE: camera open failed: %s", esp_err_to_name(ret));
        printf("CAMPROBE FAIL open=%s\n", esp_err_to_name(ret));
        printf("===JT_CAM_PROBE_END===\n");
        fflush(stdout);
        return false;
    }

    /* DQBUF 默认 portMAX_DELAY：某档不出图会把整条测试挂死，先设 2s 超时 */
    struct timeval tv = {};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ioctl(_video_fd, VIDIOC_S_DQBUF_TIMEOUT, &tv);

    esp_cam_sensor_device_t *sensor = esp_video_get_csi_video_device_sensor();
    if (!sensor) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "CAMPROBE: no sensor handle");
        printf("CAMPROBE FAIL no_sensor\n");
        printf("===JT_CAM_PROBE_END===\n");
        fflush(stdout);
        return false;
    }

    esp_cam_sensor_format_array_t fmt_arr = {};
    ret = esp_cam_sensor_query_format(sensor, &fmt_arr);
    if (ret != ESP_OK || fmt_arr.count == 0 || !fmt_arr.format_array) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "CAMPROBE: query_format failed: %s", esp_err_to_name(ret));
        printf("CAMPROBE FAIL query_format=%s\n", esp_err_to_name(ret));
        printf("===JT_CAM_PROBE_END===\n");
        fflush(stdout);
        return false;
    }
    printf("CAMPROBE count=%" PRIu32 " psram_free=%uKB\n", fmt_arr.count,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    fflush(stdout);

    bool any_ok = false;
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
    _data_cache_line_size = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
    _data_cache_line_size = 64;
#endif
    for (uint32_t i = 0; i < fmt_arr.count; i++) {
        const esp_cam_sensor_format_t *sf = &fmt_arr.format_array[i];
        const char *name = sf->name ? sf->name : "?";

        /* 收流 + 手动释放上一档缓冲（不能调 releaseCameraBuffers，它会 close(_video_fd)） */
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(_video_fd, VIDIOC_STREAMOFF, &type);
        for (int b = 0; b < CAMERA_BUFFER_COUNT; b++) {
            if (_camera_buffers[b]) {
                heap_caps_free(_camera_buffers[b]);
                _camera_buffers[b] = nullptr;
            }
            _camera_buffer_lengths[b] = 0;
        }
        _camera_width = 0;
        _camera_height = 0;
        _camera_buffer_size = 0;

        if (ioctl(_video_fd, VIDIOC_S_SENSOR_FMT, sf) != 0) {
            printf("CAMPROBE idx=%" PRIu32 " name=%s SET_FMT_FAIL errno=%d\n", i, name, errno);
            fflush(stdout);
            continue;
        }

        struct v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
            printf("CAMPROBE idx=%" PRIu32 " name=%s G_FMT_FAIL errno=%d\n", i, name, errno);
            fflush(stdout);
            continue;
        }
        if (fmt.fmt.pix.pixelformat != JIANTI_VIDEO_FMT) {
            struct v4l2_format req_fmt = fmt;
            req_fmt.fmt.pix.pixelformat = JIANTI_VIDEO_FMT;
            if (ioctl(_video_fd, VIDIOC_S_FMT, &req_fmt) != 0) {
                printf("CAMPROBE idx=%" PRIu32 " name=%s S_FMT_COLOR_FAIL errno=%d\n", i, name, errno);
                fflush(stdout);
                continue;
            }
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
                printf("CAMPROBE idx=%" PRIu32 " name=%s G_FMT2_FAIL errno=%d\n", i, name, errno);
                fflush(stdout);
                continue;
            }
        }

        const uint32_t out_w = fmt.fmt.pix.width;
        const uint32_t out_h = fmt.fmt.pix.height;
        _camera_width = out_w;
        _camera_height = out_h;
        _camera_buffer_size = fmt.fmt.pix.sizeimage ? fmt.fmt.pix.sizeimage
                              : out_w * out_h * JIANTI_BYTES_PER_PIXEL;

        /* REQBUFS(2)：esp_video_setup_buffer 每次都会销毁旧池重建，逐档直接申请 */
        struct v4l2_requestbuffers req = {};
        req.count = CAMERA_BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;
        if (ioctl(_video_fd, VIDIOC_REQBUFS, &req) != 0) {
            printf("CAMPROBE idx=%" PRIu32 " name=%s REQBUFS_FAIL errno=%d\n", i, name, errno);
            fflush(stdout);
            continue;
        }

        bool bufs_ok = true;
        for (int b = 0; b < CAMERA_BUFFER_COUNT; b++) {
            struct v4l2_buffer qb = {};
            qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            qb.memory = V4L2_MEMORY_USERPTR;
            qb.index = b;
            if (ioctl(_video_fd, VIDIOC_QUERYBUF, &qb) != 0) {
                bufs_ok = false;
                break;
            }
            const size_t len = qb.length ? qb.length : _camera_buffer_size;
            _camera_buffers[b] = static_cast<uint8_t *>(
                                     heap_caps_aligned_calloc(_data_cache_line_size, 1, len, MALLOC_CAP_SPIRAM));
            if (!_camera_buffers[b]) {
                bufs_ok = false;
                break;
            }
            _camera_buffer_lengths[b] = len;
            qb.m.userptr = (unsigned long)_camera_buffers[b];
            qb.length = len;
            if (ioctl(_video_fd, VIDIOC_QBUF, &qb) != 0) {
                bufs_ok = false;
                break;
            }
        }
        if (!bufs_ok) {
            printf("CAMPROBE idx=%" PRIu32 " name=%s BUF_FAIL errno=%d\n", i, name, errno);
            fflush(stdout);
            continue;
        }

        uint32_t got = 0;
        double fps = 0.0;
        int type2 = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_STREAMON, &type2) == 0) {
            const int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < 2500000) {
                struct v4l2_buffer db = {};
                db.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                db.memory = V4L2_MEMORY_USERPTR;
                if (ioctl(_video_fd, VIDIOC_DQBUF, &db) != 0) {
                    break;
                }
                if (db.index >= CAMERA_BUFFER_COUNT) {
                    break;
                }
                void *p = _camera_buffers[db.index];
                esp_cache_msync(p, _camera_buffer_lengths[db.index], ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                db.m.userptr = (unsigned long)p;
                db.length = _camera_buffer_lengths[db.index];
                if (ioctl(_video_fd, VIDIOC_QBUF, &db) != 0) {
                    break;
                }
                got++;
            }
            const int64_t dt_us = esp_timer_get_time() - t0;
            if (dt_us > 0) {
                fps = static_cast<double>(got) * 1000000.0 / static_cast<double>(dt_us);
            }
            ioctl(_video_fd, VIDIOC_STREAMOFF, &type2);
            any_ok = any_ok || (got > 0);
        }

        printf("CAMPROBE idx=%" PRIu32 " name=%s sensor=%ux%u out=%" PRIu32 "x%" PRIu32
               " fps_cfg=%u fps_meas=%.2f frames=%" PRIu32 " fourcc=0x%08" PRIx32
               " buf=%uKB psram_free=%uKB\n",
               i, name, sf->width, sf->height, out_w, out_h, static_cast<unsigned>(sf->fps), fps, got,
               fmt.fmt.pix.pixelformat, static_cast<unsigned>(_camera_buffer_size / 1024),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        fflush(stdout);
    }

    /* 恢复默认档（format_array[0] = 800x1280），并把相机尺寸写回成员 */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(_video_fd, VIDIOC_STREAMOFF, &type);
    for (int b = 0; b < CAMERA_BUFFER_COUNT; b++) {
        if (_camera_buffers[b]) {
            heap_caps_free(_camera_buffers[b]);
            _camera_buffers[b] = nullptr;
        }
        _camera_buffer_lengths[b] = 0;
    }
    if (ioctl(_video_fd, VIDIOC_S_SENSOR_FMT, &fmt_arr.format_array[0]) == 0) {
        struct v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) == 0) {
            _camera_width = fmt.fmt.pix.width;
            _camera_height = fmt.fmt.pix.height;
            _camera_buffer_size = fmt.fmt.pix.sizeimage ? fmt.fmt.pix.sizeimage
                                  : _camera_width * _camera_height * JIANTI_BYTES_PER_PIXEL;
            printf("CAMPROBE restore=%s out=%" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32 "\n",
                   fmt_arr.format_array[0].name ? fmt_arr.format_array[0].name : "?",
                   _camera_width, _camera_height, fmt.fmt.pix.pixelformat);
        }
    }

    printf("CAMPROBE done ok=%d\n", any_ok ? 1 : 0);
    printf("===JT_CAM_PROBE_END===\n");
    fflush(stdout);
    return any_ok;
}
#endif

bool JianTi::init(void)
{
    ESP_UTILS_LOGD("Init");
    return true;
}

bool JianTi::deinit(void)
{
    ESP_UTILS_LOGD("Deinit");
    return true;
}

/* ---------------- UI ---------------- */

void JianTi::buildUi()
{
    lv_area_t va = getVisualArea();
    int w = lv_area_get_width(&va);
    int h = lv_area_get_height(&va);
    if (w <= 0 || h <= 0) {
        w = BSP_LCD_V_RES;   /* 横屏后宽高对调 */
        h = BSP_LCD_H_RES;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF5F6F8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 一页一块容器，切页只做 hide/show */
    auto make_page = [scr, w, h]() {
        lv_obj_t *p = lv_obj_create(scr);
        lv_obj_set_size(p, w, h);
        lv_obj_center(p);
        lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(p, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        return p;
    };
    /* 文字按钮：圆角矩形 + 24px 加粗字。
     * 本批把原先 10 张烘焙文字图（assets/img_jianti_btns.c，142,208 B）全部改成文本渲染，
     * 文案直接写在这里，改字/换字号都不用重烘图。 */
    auto make_text_btn = [](lv_obj_t *parent, const char *text, int bw, int bh,
                            uint32_t bg, int radius, uint32_t fg, uint32_t border) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, bw, bh);
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, radius, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, border ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(border), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, JIANTI_CN_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(fg), LV_PART_MAIN);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        return btn;
    };
    auto make_label = [](lv_obj_t *parent, const char *text) {
        lv_obj_t *lb = lv_label_create(parent);
        lv_obj_set_style_text_font(lb, JIANTI_CN_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(lb, text);
        return lb;
    };
    /* 服务器页白卡：260x76 圆角白底 + 24px 加粗字（本批从 380x100 收到正常比例） */
    auto make_server_card = [](lv_obj_t *parent, const char *text) {
        lv_obj_t *c = lv_obj_create(parent);
        lv_obj_set_size(c, JIANTI_SRV_CARD_W, JIANTI_SRV_CARD_H);
        lv_obj_set_style_bg_color(c, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(c, 14, LV_PART_MAIN);
        lv_obj_set_style_border_width(c, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(c, lv_color_hex(0xD3DCEA), LV_PART_MAIN);
        lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lb = lv_label_create(c);
        lv_obj_set_style_text_font(lb, JIANTI_CN_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(lb, text);
        lv_obj_center(lb);
        return c;
    };

    /* ---------------- 主屏：标题 + 拍题大圆 + 服务器入口 ---------------- */
    _page_home = make_page();

    /* 左上：标题「简题」36px 加粗 + 副标题「极简作业AI助手」24px 加粗，
     * 副标题按 OUT_RIGHT_MID 挂在标题右侧 → 与标题同一行、垂直居中对齐（本批要求平齐同行） */
    lv_obj_t *title = lv_label_create(_page_home);
    lv_obj_set_style_text_font(title, JIANTI_TITLE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_label_set_text(title, "简题");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 6);

    lv_obj_t *subtitle = lv_label_create(_page_home);
    lv_obj_set_style_text_font(subtitle, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5B6472), LV_PART_MAIN);
    lv_label_set_text(subtitle, "极简作业AI助手");
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_RIGHT_MID, 14, 0);

    /* 拍题大圆 300x300：容器画圆 + 44px 加粗「拍题」（本批首页专用大字号） */
    _btn_capture = lv_obj_create(_page_home);
    lv_obj_set_size(_btn_capture, JIANTI_CAPTURE_D, JIANTI_CAPTURE_D);
    lv_obj_align(_btn_capture, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_btn_capture, lv_color_hex(0x2F6BFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_btn_capture, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_capture, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_btn_capture, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_btn_capture, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_btn_capture, 24, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(_btn_capture, lv_color_hex(0x2F6BFF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(_btn_capture, LV_OPA_30, LV_PART_MAIN);
    lv_obj_clear_flag(_btn_capture, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_btn_capture, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_btn_capture, onCaptureClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *cap_label = lv_label_create(_btn_capture);
        lv_obj_set_style_text_font(cap_label, JIANTI_CAPTURE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(cap_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(cap_label, "拍题");
        lv_obj_center(cap_label);
    }

    /* 拍题圆下方提示语：本批文案改成「轻按一下，思路便知」，24px 加粗 */
    lv_obj_t *hint = lv_label_create(_page_home);
    lv_obj_set_style_text_font(hint, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A929E), LV_PART_MAIN);
    lv_label_set_text(hint, "轻按一下，思路便知");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 195);

    _btn_server_entry = make_text_btn(_page_home, "服务器", 132, 52, 0xFFFFFF, 26, 0x5B6472, 0xD3DCEA);
    lv_obj_align(_btn_server_entry, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_add_event_cb(_btn_server_entry, onServerEntryClicked, LV_EVENT_CLICKED, this);

    _server_name_label = lv_label_create(_page_home);
    /* v8：服务器命名统一改「内网服务器 / 公网服务器」，正文 24px 加粗（不再用 20px 西文串） */
    lv_obj_set_style_text_font(_server_name_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_server_name_label, lv_color_hex(0x5B6472), LV_PART_MAIN);
    lv_label_set_text(_server_name_label, _server_id ? "公网服务器" : "内网服务器");
    lv_obj_align_to(_server_name_label, _btn_server_entry, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* ---------------- 结果页：返回+完成 / 解题红 pill / 再拍一题 / 上一题 / n/N / 下一题 ---------------- */
    _page_result = make_page();

    /* 顶边缘下滑条：只占顶部 40px，先建 → 后续按钮压在上面，互不抢点击。
     * 内容区（答案框，y>=70）不在条内，滚动不受影响。 */
    lv_obj_t *edge_result = lv_obj_create(_page_result);
    lv_obj_set_size(edge_result, w, 40);
    lv_obj_align(edge_result, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(edge_result, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(edge_result, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(edge_result, 0, LV_PART_MAIN);
    lv_obj_add_flag(edge_result, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(edge_result, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(edge_result, onTopEdgePressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(edge_result, onTopEdgeReleased, LV_EVENT_RELEASED, this);

    _btn_back = make_text_btn(_page_result, "返回", 136, 60, 0x2F6BFF, 12, 0xFFFFFF, 0);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_add_event_cb(_btn_back, onBackClicked, LV_EVENT_CLICKED, this);

    /* 本批：顶部「完成」状态字整块删除，不留占位（它只挂在返回按钮右侧，删掉不影响其它控件） */

    /* 再拍一题：右上角灰 pill */
    _btn_recap = make_text_btn(_page_result, "再拍一题", 180, 60, 0xDCE3EF, 12, 0x1F2329, 0);
    lv_obj_align(_btn_recap, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_add_event_cb(_btn_recap, onRecapClicked, LV_EVENT_CLICKED, this);

    /* 解题红 pill：顶部正中，压在答案区上沿；测试版点它只解锁「过程与答案」卡，不扣卡 */
    _btn_solve = make_text_btn(_page_result, "解题", 176, 54, 0xE33A2E, 12, 0xFFFFFF, 0);
    lv_obj_align(_btn_solve, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_add_flag(_btn_solve, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_btn_solve, onSolveClicked, LV_EVENT_CLICKED, this);

    /* 答案区：本批改成「不带框、不留大空地」——
     * 去白底/去圆角/去边距，直接铺在页面底色上，纵向一直伸到「上一题/下一题」上沿。 */
    _answer_box = lv_obj_create(_page_result);
    lv_obj_set_size(_answer_box, w - 24, h - 84 - 84);
    lv_obj_align(_answer_box, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(_answer_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_answer_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_answer_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_answer_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(_answer_box, 22, LV_PART_MAIN);
    lv_obj_set_scroll_dir(_answer_box, LV_DIR_VER);
    /* 思路卡 / 过程卡上下排（竖向 flex），解锁后过程卡接在思路卡下面 */
    lv_obj_set_flex_flow(_answer_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_answer_box, 14, LV_PART_MAIN);
    /* 明确的滚动提示：右侧 8px 常显滚动条（浅灰底 + 深灰滑块）。
     * 本批要求「过程卡必须在屏上看得见」，滚动条就是给用户的那条提示。 */
    lv_obj_set_style_width(_answer_box, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(_answer_box, lv_color_hex(0xD8DEE8), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(_answer_box, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(_answer_box, 4, LV_PART_SCROLLBAR);
    lv_obj_set_scrollbar_mode(_answer_box, LV_SCROLLBAR_MODE_ON);

    _answer_label = lv_label_create(_answer_box);
    lv_obj_set_width(_answer_label, w - 74);
    lv_label_set_long_mode(_answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_answer_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(_answer_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_answer_label, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(_answer_label, 8, LV_PART_MAIN);
    lv_label_set_text(_answer_label, "对准作业，点下面的拍题");

    /* 过程与答案卡：默认隐藏，点「解题」解锁后填字并显示（独立 label，才能滚到它） */
    _step_label = lv_label_create(_answer_box);
    lv_obj_set_width(_step_label, w - 74);
    lv_label_set_long_mode(_step_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_step_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(_step_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_step_label, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(_step_label, 8, LV_PART_MAIN);
    lv_label_set_text(_step_label, "");
    lv_obj_add_flag(_step_label, LV_OBJ_FLAG_HIDDEN);

    _btn_prev = make_text_btn(_page_result, "上一题", 152, 60, 0xDCE3EF, 12, 0x1F2329, 0);
    lv_obj_align(_btn_prev, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    lv_obj_add_event_cb(_btn_prev, onPrevClicked, LV_EVENT_CLICKED, this);

    _btn_next = make_text_btn(_page_result, "下一题", 152, 60, 0x2F6BFF, 12, 0xFFFFFF, 0);
    lv_obj_align(_btn_next, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
    lv_obj_add_event_cb(_btn_next, onNextClicked, LV_EVENT_CLICKED, this);

    /* n/N 是动态数字，走 20px 加粗 ASCII 档（全页最小 20px） */
    _pager_label = lv_label_create(_page_result);
    lv_obj_set_style_text_font(_pager_label, JIANTI_ASCII_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_pager_label, lv_color_hex(0x5B6472), LV_PART_MAIN);
    lv_label_set_text(_pager_label, "1/1");
    lv_obj_align(_pager_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* --- 倒计时拍摄 UI（初始隐藏） --- */
    _countdown_label = lv_label_create(_page_result);
    lv_obj_set_style_text_font(_countdown_label, JIANTI_ASCII_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_countdown_label, lv_color_hex(0xE64545), LV_PART_MAIN);
    lv_label_set_text(_countdown_label, "");
    lv_obj_align(_countdown_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_countdown_label, LV_OBJ_FLAG_HIDDEN);

    /* 拍后确认 UI（初始隐藏） */
    _shot_img = lv_image_create(_page_result);
    lv_obj_set_size(_shot_img, 400, 250);
    lv_obj_align(_shot_img, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_border_width(_shot_img, 0, LV_PART_MAIN);
    lv_obj_add_flag(_shot_img, LV_OBJ_FLAG_HIDDEN);

    _btn_confirm = lv_btn_create(_page_result);
    lv_obj_set_size(_btn_confirm, 200, 64);
    lv_obj_align(_btn_confirm, LV_ALIGN_CENTER, -110, 160);
    lv_obj_set_style_bg_color(_btn_confirm, lv_color_hex(0x27AE60), LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_confirm, 22, LV_PART_MAIN);
    lv_obj_t *confirm_label = lv_label_create(_btn_confirm);
    lv_obj_set_style_text_font(confirm_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_label_set_text(confirm_label, "确认");
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(_btn_confirm, onConfirmClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(_btn_confirm, LV_OBJ_FLAG_HIDDEN);

    _btn_cancel = lv_btn_create(_page_result);
    lv_obj_set_size(_btn_cancel, 200, 64);
    lv_obj_align(_btn_cancel, LV_ALIGN_CENTER, 110, 160);
    lv_obj_set_style_bg_color(_btn_cancel, lv_color_hex(0xE64545), LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_cancel, 22, LV_PART_MAIN);
    lv_obj_t *cancel_label = lv_label_create(_btn_cancel);
    lv_obj_set_style_text_font(cancel_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(_btn_cancel, onCancelClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(_btn_cancel, LV_OBJ_FLAG_HIDDEN);

    /* --- 扣卡弹窗（8026 卡账本测试版，初始隐藏） ---
     * 三态：0=没卡双入口 1=预支确认 2=连不上服务器。
     * 关闭只关弹窗，申请单仍待批（关闭 != 取消）。 */
    _card_mask = lv_obj_create(_page_result);
    lv_obj_set_size(_card_mask, w, h);
    lv_obj_align(_card_mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_card_mask, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_card_mask, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(_card_mask, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_card_mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_card_mask, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_card_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_card_mask, LV_OBJ_FLAG_CLICKABLE);   /* 吃掉下层点击 */
    lv_obj_add_flag(_card_mask, LV_OBJ_FLAG_HIDDEN);

    _card_panel = lv_obj_create(_card_mask);
    lv_obj_set_size(_card_panel, 640, 360);
    lv_obj_align(_card_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_card_panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(_card_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_card_panel, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_card_panel, 20, LV_PART_MAIN);
    lv_obj_clear_flag(_card_panel, LV_OBJ_FLAG_SCROLLABLE);

    _card_title = lv_label_create(_card_panel);
    lv_obj_set_style_text_font(_card_title, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_card_title, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_label_set_text(_card_title, "没卡了");
    lv_obj_align(_card_title, LV_ALIGN_TOP_MID, 0, 0);

    _card_body = lv_label_create(_card_panel);
    lv_obj_set_width(_card_body, 590);
    lv_label_set_long_mode(_card_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_card_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_card_body, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_card_body, lv_color_hex(0x5B6472), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(_card_body, 8, LV_PART_MAIN);
    lv_label_set_text(_card_body, "");
    lv_obj_align(_card_body, LV_ALIGN_TOP_MID, 0, 48);

    {
        struct CardBtnDef {
            lv_obj_t **slot;
            const char *text;
            uint32_t color;
            lv_event_cb_t cb;
        };
        const CardBtnDef defs[] = {
            {&_btn_card_apply, "向家长申请", 0x2F6FED, onCardApplyClicked},
            {&_btn_card_preborrow, "自己来预支", 0xE07B00, onCardPreborrowClicked},
            {&_btn_card_confirm, "确认", 0x27AE60, onCardConfirmClicked},
            {&_btn_card_cancel, "取消", 0x8A919C, onCardCancelClicked},
            {&_btn_card_close, "关闭", 0x8A919C, onCardCloseClicked},
        };
        for (const CardBtnDef &d : defs) {
            lv_obj_t *btn = lv_btn_create(_card_panel);
            lv_obj_set_size(btn, 220, 60);
            lv_obj_set_style_bg_color(btn, lv_color_hex(d.color), LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, JIANTI_CN_FONT, LV_PART_MAIN);
            lv_label_set_text(lbl, d.text);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, d.cb, LV_EVENT_CLICKED, this);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
            *d.slot = btn;
        }
    }

    /* 发送成功/失败 toast：压在结果页最上层，1.5s 自动消失 */
    _card_toast_label = lv_label_create(_page_result);
    lv_obj_set_style_text_font(_card_toast_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_card_toast_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_card_toast_label, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_card_toast_label, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(_card_toast_label, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_card_toast_label, 14, LV_PART_MAIN);
    lv_label_set_text(_card_toast_label, "");
    lv_obj_align(_card_toast_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_card_toast_label, LV_OBJ_FLAG_HIDDEN);

    /* ---------------- 取景器页：上方实时预览、下方左右两个小圆按钮 ---------------- */
    _page_view = make_page();
    /* 本批：页面底色改透明，整块实心黑底删除，透出底板纸面（0xF5F6F8） */

    _preview_img = lv_image_create(_page_view);
    /* v8 正式版默认「裁切铺满」：控件先按 800x480 建，preparePreviewTargets 再按实际几何校正 */
    lv_obj_set_size(_preview_img, JIANTI_FILL_W, JIANTI_FILL_H);
    lv_obj_align(_preview_img, LV_ALIGN_TOP_MID, 0, 0);
    /* 图像控件本身也不铺黑：没有数据时是透明，不是实心黑板 */
    lv_obj_set_style_bg_opa(_preview_img, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_preview_img, 0, LV_PART_MAIN);

    /* 本批：底部大黑框（原 120px #14171C 整条）整块删除，
     * 两个圆直接浮在取景器页左下 / 右下，页面底色即黑，不再有分界条。 */

    /* 左下角灰圆「返回」：圆看着小，点击区外扩 20px（视觉小、触摸大） */
    _btn_view_back = lv_btn_create(_page_view);
    lv_obj_set_size(_btn_view_back, JIANTI_CORNER_BTN_D, JIANTI_CORNER_BTN_D);
    lv_obj_align(_btn_view_back, LV_ALIGN_BOTTOM_LEFT, 28, -12);
    lv_obj_set_style_bg_color(_btn_view_back, lv_color_hex(0x3A4048), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_btn_view_back, JIANTI_FLOAT_OPA, LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_view_back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_btn_view_back, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_btn_view_back, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_btn_view_back, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(_btn_view_back, JIANTI_CORNER_BTN_EXT);
    lv_obj_t *vback_label = lv_label_create(_btn_view_back);
    lv_obj_set_style_text_font(vback_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(vback_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(vback_label, "返回");
    lv_obj_center(vback_label);
    lv_obj_add_event_cb(_btn_view_back, onViewBackClicked, LV_EVENT_CLICKED, this);

    /* 右下角红圆「拍照」：同上，触摸区外扩一圈 */
    _btn_shutter = lv_btn_create(_page_view);
    lv_obj_set_size(_btn_shutter, JIANTI_CORNER_BTN_D, JIANTI_CORNER_BTN_D);
    lv_obj_align(_btn_shutter, LV_ALIGN_BOTTOM_RIGHT, -28, -12);
    lv_obj_set_style_bg_color(_btn_shutter, lv_color_hex(0xE64545), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_btn_shutter, JIANTI_FLOAT_OPA, LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_shutter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_btn_shutter, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_btn_shutter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_btn_shutter, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(_btn_shutter, JIANTI_CORNER_BTN_EXT);
    lv_obj_t *shutter_label = lv_label_create(_btn_shutter);
    lv_obj_set_style_text_font(shutter_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(shutter_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(shutter_label, "拍照");
    lv_obj_center(shutter_label);
    lv_obj_add_event_cb(_btn_shutter, onShutterClicked, LV_EVENT_CLICKED, this);

    _view_status = lv_label_create(_page_view);
    lv_obj_set_style_text_font(_view_status, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_view_status, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_view_status, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_view_status, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_view_status, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(_view_status, 8, LV_PART_MAIN);
    lv_label_set_text(_view_status, "预览中…");
    lv_obj_align(_view_status, LV_ALIGN_TOP_LEFT, 12, 10);

    /* ---------------- 静态确认页：放大静态图 + 确认/取消 ---------------- */
    _page_still = make_page();
    /* 本批：页面底色改透明，整块实心黑底删除，透出底板纸面（0xF5F6F8） */

    _still_img = lv_image_create(_page_still);
    /* v8 正式版默认「静态确认也铺满」：控件先按 800x480 建，prepareStillTargets/renderStillFrame 再按实际几何校正 */
    lv_obj_set_size(_still_img, JIANTI_FILL_W, JIANTI_FILL_H);
    lv_obj_align(_still_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(_still_img, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_still_img, 0, LV_PART_MAIN);

    _still_hint = lv_label_create(_page_still);
    lv_obj_set_style_text_font(_still_hint, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_still_hint, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_still_hint, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_still_hint, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_still_hint, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(_still_hint, 8, LV_PART_MAIN);
    lv_label_set_text(_still_hint, "已抓拍，确认后上传识别");
    lv_obj_align(_still_hint, LV_ALIGN_TOP_LEFT, 12, 10);

    /* 本批：原 340x64 长条取消/确认整块换成圆钮——与取景器同款：
     * 直径 96、视觉圆、点击区外扩 20（96+20*2 = 136x136 触摸）、底色 70% 半透明。
     * 左灰「取消」、右绿「确认」，位置与取景器左返回/右拍照一致，不换手。 */

    _btn_still_cancel = lv_btn_create(_page_still);
    lv_obj_set_size(_btn_still_cancel, JIANTI_CORNER_BTN_D, JIANTI_CORNER_BTN_D);
    lv_obj_align(_btn_still_cancel, LV_ALIGN_BOTTOM_LEFT, 28, -12);
    lv_obj_set_style_bg_color(_btn_still_cancel, lv_color_hex(0x3A4048), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_btn_still_cancel, JIANTI_FLOAT_OPA, LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_still_cancel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_btn_still_cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_btn_still_cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_btn_still_cancel, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(_btn_still_cancel, JIANTI_CORNER_BTN_EXT);
    lv_obj_t *cancel2_label = lv_label_create(_btn_still_cancel);
    lv_obj_set_style_text_font(cancel2_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel2_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(cancel2_label, "取消");
    lv_obj_center(cancel2_label);
    lv_obj_add_event_cb(_btn_still_cancel, onStillCancelClicked, LV_EVENT_CLICKED, this);

    _btn_still_ok = lv_btn_create(_page_still);
    lv_obj_set_size(_btn_still_ok, JIANTI_CORNER_BTN_D, JIANTI_CORNER_BTN_D);
    lv_obj_align(_btn_still_ok, LV_ALIGN_BOTTOM_RIGHT, -28, -12);
    lv_obj_set_style_bg_color(_btn_still_ok, lv_color_hex(0x27AE60), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_btn_still_ok, JIANTI_FLOAT_OPA, LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_still_ok, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_btn_still_ok, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_btn_still_ok, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_btn_still_ok, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(_btn_still_ok, JIANTI_CORNER_BTN_EXT);
    lv_obj_t *ok_label = lv_label_create(_btn_still_ok);
    lv_obj_set_style_text_font(ok_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ok_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(ok_label, "确认");
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(_btn_still_ok, onStillOkClicked, LV_EVENT_CLICKED, this);

    /* ---------------- 服务器选择页：只有两个按钮 + 返回 ---------------- */
    _page_server = make_page();

    lv_obj_t *edge_server = lv_obj_create(_page_server);
    lv_obj_set_size(edge_server, w, 40);
    lv_obj_align(edge_server, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(edge_server, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(edge_server, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(edge_server, 0, LV_PART_MAIN);
    lv_obj_add_flag(edge_server, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(edge_server, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(edge_server, onTopEdgePressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(edge_server, onTopEdgeReleased, LV_EVENT_RELEASED, this);

    _btn_srv_back = make_text_btn(_page_server, "返回", 136, 60, 0x2F6BFF, 12, 0xFFFFFF, 0);
    lv_obj_align(_btn_srv_back, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_add_event_cb(_btn_srv_back, onBackClicked, LV_EVENT_CLICKED, this);

    /* v8：整页改成从顶部往下紧凑排（TOP_MID 绝对定位），间距统一 12：
     * 内网服务器 84、公网服务器 172（卡片 76 高）、测试连接 260（60 高）、重新配网 332（60 高）、结果 402。
     * 「测试连接 / 重新配网」整体再上移 12px，紧贴上方按钮，不留大空档；
     * 卡片 260x76 里只放 5 个字（24px），不重叠不串行；
     * 全程只有按钮，没有任何输入框/软键盘。 */
    _btn_lan = make_server_card(_page_server, "内网服务器");
    lv_obj_align(_btn_lan, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_add_event_cb(_btn_lan, onLanClicked, LV_EVENT_CLICKED, this);

    _btn_cloud = make_server_card(_page_server, "公网服务器");
    lv_obj_align(_btn_cloud, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_add_event_cb(_btn_cloud, onCloudClicked, LV_EVENT_CLICKED, this);

    /* 测试连接按钮：公网服务器正下方，间距 12 */
    _btn_test = lv_btn_create(_page_server);
    lv_obj_set_size(_btn_test, 260, 60);
    lv_obj_align(_btn_test, LV_ALIGN_TOP_MID, 0, 260);
    lv_obj_set_style_bg_color(_btn_test, lv_color_hex(0x4A6CF7), LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_test, 24, LV_PART_MAIN);
    lv_obj_t *test_label = lv_label_create(_btn_test);
    lv_obj_set_style_text_font(test_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_label_set_text(test_label, "测试连接");
    lv_obj_center(test_label);
    lv_obj_add_event_cb(_btn_test, onTestClicked, LV_EVENT_CLICKED, this);

    /* 重新配网按钮：测试连接正下方，间距 12 */
    _btn_reconfig = lv_btn_create(_page_server);
    lv_obj_set_size(_btn_reconfig, 260, 60);
    lv_obj_align(_btn_reconfig, LV_ALIGN_TOP_MID, 0, 332);
    lv_obj_set_style_bg_color(_btn_reconfig, lv_color_hex(0xE64545), LV_PART_MAIN);
    lv_obj_set_style_radius(_btn_reconfig, 24, LV_PART_MAIN);
    lv_obj_t *reconfig_label = lv_label_create(_btn_reconfig);
    lv_obj_set_style_text_font(reconfig_label, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_label_set_text(reconfig_label, "重新配网");
    lv_obj_center(reconfig_label);
    lv_obj_add_event_cb(_btn_reconfig, onReconfigClicked, LV_EVENT_CLICKED, this);

    /* 测试结果标签 */
    _label_test_result = lv_label_create(_page_server);
    lv_obj_set_style_text_font(_label_test_result, JIANTI_CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(_label_test_result, lv_color_hex(0x1F2329), LV_PART_MAIN);
    lv_obj_align(_label_test_result, LV_ALIGN_TOP_MID, 0, 402);
    lv_label_set_text(_label_test_result, "");

    showPage(Page::Home);
}

void JianTi::showPage(Page page)
{
    if (!_page_home || !_page_result || !_page_server) {
        return;
    }
    lv_obj_t *pages[] = {_page_home, _page_view, _page_still, _page_result, _page_server};
    for (lv_obj_t *p : pages) {
        if (p) {
            lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_t *target = nullptr;
    switch (page) {
    case Page::Home:
        target = _page_home;
        break;
    case Page::Viewfinder:
        target = _page_view;
        break;
    case Page::Still:
        target = _page_still;
        break;
    case Page::Result:
        target = _page_result;
        break;
    case Page::Server:
        target = _page_server;
        break;
    }
    if (target) {
        lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- 服务器选择（只两个按钮，不做输入/软键盘） ---------------- */

const char *JianTi::gatewayUrl(void) const
{
    return _server_id ? JIANTI_GW_CLOUD : CONFIG_JIANTI_GATEWAY_URL;
}

void JianTi::loadServerPref(void)
{
    nvs_handle_t nh;
    uint8_t v = 0;
    if (nvs_open(JIANTI_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        if (nvs_get_u8(nh, JIANTI_NVS_KEY_SRV, &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(nh);
    }
    _server_id = (v == 1) ? 1 : 0;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "server pref: %s", _server_id ? "cloud" : "lan");
}

void JianTi::saveServerPref(uint8_t id)
{
    _server_id = (id == 1) ? 1 : 0;
    nvs_handle_t nh;
    if (nvs_open(JIANTI_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        esp_err_t err = nvs_set_u8(nh, JIANTI_NVS_KEY_SRV, _server_id);
        if (err == ESP_OK) {
            err = nvs_commit(nh);
        }
        nvs_close(nh);
        if (err == ESP_OK) {
            ESP_LOGI(ESP_UTILS_LOG_TAG, "server switched to %s", _server_id ? JIANTI_GW_CLOUD : "lan");
        } else {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "save server pref failed: %s", esp_err_to_name(err));
        }
    }
}

void JianTi::onServerEntryClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: server button -> server page");
    bsp_extra_display_lock(-1);
    self->showPage(Page::Server);
    bsp_extra_display_unlock();
}

void JianTi::onLanClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: lan(内网服务器) button -> switch gateway");
    self->saveServerPref(0);
    bsp_extra_display_lock(-1);
    lv_label_set_text(self->_server_name_label, "内网服务器");
    self->showPage(Page::Home);
    bsp_extra_display_unlock();
}

void JianTi::onCloudClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: cloud(公网服务器) button -> switch gateway");
    self->saveServerPref(1);
    bsp_extra_display_lock(-1);
    lv_label_set_text(self->_server_name_label, "公网服务器");
    self->showPage(Page::Home);
    bsp_extra_display_unlock();
}

void JianTi::onBackClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: back button -> home");
    bsp_extra_display_lock(-1);
    self->showPage(Page::Home);
    bsp_extra_display_unlock();
}

void JianTi::onRecapClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    /* v8：批文里的全链路是「再拍一题回首页」，原来这里会自动再进一次取景器，
     * 停不到首页。改成走 leaveViewfinder()：停预览流 + 释放静态图 + 回首页，
     * 由用户自己再点「拍题」重新进相机。 */
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: recap button -> home, stop stream");
    if (self) {
        self->leaveViewfinder();
    }
}

void JianTi::updatePager(void)
{
    if (!_pager_label) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", _q_index, _q_total);
    lv_label_set_text(_pager_label, buf);
}

/* ---------------- 结果页按题渲染：8025 cards -> 一题一页 ----------------
 * 文字逐字取自卡的 title/text（清洗+重组后的原文），板端不改字、不拼行；
 * 题键取 title 里「第N题」/「第N题(k)」，小题算独立题，与 server 分题一致。 */

void JianTi::resetCards(void)
{
    _card_count = 0;
    _qkey_count = 0;
    _q_unlocked = false;
    _card_arena_used = 0;
}

void JianTi::parseCards(char *json)
{
    resetCards();
    if (!json) {
        return;
    }
    if (!_card_arena) {
        _card_arena = static_cast<char *>(heap_caps_malloc(JIANTI_CARD_ARENA, MALLOC_CAP_SPIRAM));
        if (!_card_arena) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "cards: arena alloc failed");
            return;
        }
    }

    char *arr = strstr(json, "\"cards\"");
    if (!arr) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "cards: absent in resp, fallback to whole text");
        return;
    }
    arr = strchr(arr, '[');
    if (!arr) {
        return;
    }

    char *p = arr + 1;
    while (*p && _card_count < JIANTI_MAX_CARDS) {
        char *ob = strchr(p, '{');
        char *arr_end = strchr(p, ']');
        if (!ob || (arr_end && arr_end < ob)) {
            break;
        }
        char *oe = strchr(ob, '}');
        if (!oe) {
            break;
        }

        /* 临时截断本卡对象，复用 json_get_string 取字段，取完立刻还原 */
        const char saved = *oe;
        *oe = '\0';

        Card &c = _cards[_card_count];
        c.title[0] = '\0';
        c.text = nullptr;
        c.qkey = 0;
        c.is_step = false;

        bool ok = json_get_string(ob, "title", c.title, sizeof(c.title));
        if (ok && c.title[0] != '\0') {
            c.is_step = (strstr(c.title, "过程") != nullptr);
            /* 题键：第N题 -> N*100；第N题(k) -> N*100+k */
            const char *q = strstr(c.title, "第");
            if (q) {
                q += strlen("第");
                int n = 0;
                while (*q >= '0' && *q <= '9') {
                    n = n * 10 + (*q - '0');
                    q++;
                }
                /* 跳过「题」字（3 字节）再判小题括号 */
                if (static_cast<unsigned char>(q[0]) == 0xE9 &&
                    static_cast<unsigned char>(q[1]) == 0xA2 &&
                    static_cast<unsigned char>(q[2]) == 0x98) {
                    q += 3;
                }
                int k = 0;
                if (*q == '(') {
                    q++;
                    while (*q >= '0' && *q <= '9') {
                        k = k * 10 + (*q - '0');
                        q++;
                    }
                } else if (static_cast<unsigned char>(q[0]) == 0xEF &&
                           static_cast<unsigned char>(q[1]) == 0xBC &&
                           static_cast<unsigned char>(q[2]) == 0x88) {  /* 全角（ U+FF08 */
                    q += 3;
                    while (*q >= '0' && *q <= '9') {
                        k = k * 10 + (*q - '0');
                        q++;
                    }
                }
                c.qkey = n * 100 + k;
            }

            /* text 原文直接落 PSRAM arena，逐字不改 */
            size_t room = (_card_arena_used < static_cast<size_t>(JIANTI_CARD_ARENA))
                          ? (JIANTI_CARD_ARENA - _card_arena_used) : 0;
            if (room > 1) {
                char *dst = _card_arena + _card_arena_used;
                if (json_get_string(ob, "text", dst, room)) {
                    size_t len = strlen(dst);
                    c.text = dst;
                    _card_arena_used += len + 1;
                }
            }
        }

        *oe = saved;

        if (ok && c.title[0] != '\0' && c.text != nullptr) {
            ESP_LOGI(ESP_UTILS_LOG_TAG, "card[%d]: qkey=%d %s title=\"%s\" %u chars",
                     _card_count, c.qkey, c.is_step ? "step" : "idea",
                     c.title, (unsigned)strlen(c.text));
            _card_count++;
        }

        p = oe + 1;
    }

    ESP_LOGI(ESP_UTILS_LOG_TAG, "cards: parsed %d (of max %d)", _card_count, JIANTI_MAX_CARDS);
}

/* ---- 公式清洗：网关回包里的 LaTeX 残码 → 屏上可读（v9 演示批） ----
 * 回包里会出现 \( \) \[ \] \qquad \ne、x^{|m|} 这类残码（见 logs/gw8027_resp_cards_*.json）。
 * 板端只做「字符级改写」，不改文案含义，也不引数学排版引擎：
 *   \ne→≠  \le→≤  \ge→≥  \times→×  \div→÷  \pm→±  \cdot→·  \infty→∞  \pi→π
 *   \sqrt{x}→√(x)   \frac{a}{b}→(a)/(b)   \text{..}/\mathrm{..}→只留内容
 *   \qquad→4 空格  \quad→2 空格  \, \; \: \!（空格）→1 空格  \\→换行
 *   \( \) \[ \] $ $$ \left \right→删除（\right. 的点一起删）
 *   ^{..}/_{..}：24px 字库只有 ² ³ 两个上标字形，匹配上就换，否则退成 ^(..)/_(..)
 * 未知命令 \foo→去反斜杠保留 foo（\sin→sin、\log→log）。
 * 24px 字库已核对：≠ ≤ ≥ × ÷ ± · ∞ √ π ° ² ³ ∠ △ ∵ ∴ → ← ⇒ ⊥ ∥ ∑ ∫ ≈ α β γ θ Δ 全部在字库内。 */
#define JIANTI_PRETTY_CAP (8 * 1024)

static char *jt_pretty_buf(void)
{
    static char *buf = nullptr;
    if (!buf) {
        buf = static_cast<char *>(heap_caps_malloc(JIANTI_PRETTY_CAP, MALLOC_CAP_SPIRAM));
    }
    return buf;
}

static void jt_pretty_math(const char *src, char *dst, size_t cap)
{
    if (!dst || cap < 2) {
        return;
    }
    size_t o = 0;
    auto put = [&](const char *s, size_t n) {
        if (o + 1 >= cap) {
            size_t room = cap - 1 - o;
            n = (n < room) ? n : room;
        }
        if (n) {
            memcpy(dst + o, s, n);
            o += n;
        }
    };
    auto puts_str = [&](const char *s) {
        if (s) {
            put(s, strlen(s));
        }
    };

    if (!src) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && o + 1 < cap; ) {
        const char c = src[i];
        if (c == '~') {                 /* LaTeX 不换行空格 */
            puts_str(" ");
            ++i;
            continue;
        }
        if (c != '\\' && c != '^' && c != '_' && c != '$') {
            put(&src[i], 1);
            ++i;
            continue;
        }
        if (c == '$') {                 /* 行内/行间公式定界符直接丢 */
            ++i;
            continue;
        }

        if (c == '^' || c == '_') {
            char grp[96] = {0};
            size_t gi = 0;
            const char *p = src + i + 1;
            if (*p == '{') {
                ++p;
                while (*p && *p != '}' && gi + 1 < sizeof(grp)) {
                    if (*p == '\\' && p[1]) {
                        ++p;            /* 组内命令只留命令名，丢反斜杠 */
                    }
                    grp[gi++] = *p++;
                }
                if (*p == '}') {
                    ++p;
                }
            } else if (*p) {
                grp[gi++] = *p++;
            }
            grp[gi] = '\0';
            i = static_cast<size_t>(p - src);
            if (gi == 0) {
                continue;
            }
            if (c == '^' && gi == 1 && grp[0] == '2') {
                puts_str("²");
            } else if (c == '^' && gi == 1 && grp[0] == '3') {
                puts_str("³");
            } else if (gi == 1) {
                char tmp[4] = {c, grp[0], '\0', '\0'};
                puts_str(tmp);
            } else {
                char tmp[110];
                snprintf(tmp, sizeof(tmp), "%c(%s)", c, grp);
                puts_str(tmp);
            }
            continue;
        }

        /* 反斜杠命令 */
        ++i;
        if (src[i] == '\0') {
            break;
        }
        const char n = src[i];
        if (n == '\\') {
            puts_str("\n");
            ++i;
            continue;
        }
        if (n == '(' || n == ')' || n == '[' || n == ']') {
            ++i;
            continue;
        }
        if (n == ',' || n == ';' || n == ':' || n == '!' || n == ' ') {
            puts_str(" ");
            ++i;
            continue;
        }
        if (n == '{' || n == '}' || n == '$' || n == '%' || n == '&' || n == '#' || n == '_') {
            put(&src[i], 1);
            ++i;
            continue;
        }
        if (!((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z'))) {
            /* 非字母命令（\1 之类）：丢掉反斜杠即可 */
            put(&src[i], 1);
            ++i;
            continue;
        }

        const size_t cs = i;
        while ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z')) {
            ++i;
        }
        const size_t clen = i - cs;
        auto is_cmd = [&](const char *name) {
            return clen == strlen(name) && strncmp(src + cs, name, clen) == 0;
        };
        auto take_group = [&](char *out, size_t out_cap) -> bool {
            if (src[i] != '{') {
                return false;
            }
            ++i;
            size_t gi = 0;
            int depth = 1;
            while (src[i] != '\0' && depth > 0) {
                if (src[i] == '{') {
                    ++depth;
                } else if (src[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
                if (gi + 1 < out_cap) {
                    out[gi++] = src[i];
                }
                ++i;
            }
            out[gi] = '\0';
            return true;
        };

        if (is_cmd("frac")) {
            char a[96] = {0};
            char b[96] = {0};
            if (take_group(a, sizeof(a)) && take_group(b, sizeof(b))) {
                puts_str("(");
                puts_str(a);
                puts_str(")/(");
                puts_str(b);
                puts_str(")");
            }
            continue;
        }
        if (is_cmd("sqrt")) {
            if (src[i] == '[') {            /* 开 n 次方，丢掉下标 */
                while (src[i] != '\0' && src[i] != ']') {
                    ++i;
                }
                if (src[i] == ']') {
                    ++i;
                }
            }
            char a[96] = {0};
            if (take_group(a, sizeof(a))) {
                puts_str("√(");
                puts_str(a);
                puts_str(")");
            }
            continue;
        }
        if (is_cmd("text") || is_cmd("mathrm") || is_cmd("mathbf") || is_cmd("mathit") ||
            is_cmd("operatorname") || is_cmd("boldsymbol")) {
            char a[160] = {0};
            if (take_group(a, sizeof(a))) {
                puts_str(a);
            }
            continue;
        }
        if (is_cmd("left") || is_cmd("right")) {
            if (is_cmd("right") && src[i] == '.') {
                ++i;
            }
            continue;
        }

        const char *rep = nullptr;
        if (is_cmd("neq") || is_cmd("ne")) {
            rep = "≠";
        } else if (is_cmd("leq") || is_cmd("le")) {
            rep = "≤";
        } else if (is_cmd("geq") || is_cmd("ge")) {
            rep = "≥";
        } else if (is_cmd("times")) {
            rep = "×";
        } else if (is_cmd("div")) {
            rep = "÷";
        } else if (is_cmd("pm")) {
            rep = "±";
        } else if (is_cmd("cdot")) {
            rep = "·";
        } else if (is_cmd("infty")) {
            rep = "∞";
        } else if (is_cmd("pi")) {
            rep = "π";
        } else if (is_cmd("alpha")) {
            rep = "α";
        } else if (is_cmd("beta")) {
            rep = "β";
        } else if (is_cmd("gamma")) {
            rep = "γ";
        } else if (is_cmd("theta")) {
            rep = "θ";
        } else if (is_cmd("Delta")) {
            rep = "Δ";
        } else if (is_cmd("triangle")) {
            rep = "△";
        } else if (is_cmd("angle")) {
            rep = "∠";
        } else if (is_cmd("because")) {
            rep = "∵";
        } else if (is_cmd("therefore")) {
            rep = "∴";
        } else if (is_cmd("perp")) {
            rep = "⊥";
        } else if (is_cmd("parallel")) {
            rep = "∥";
        } else if (is_cmd("approx")) {
            rep = "≈";
        } else if (is_cmd("sum")) {
            rep = "∑";
        } else if (is_cmd("int")) {
            rep = "∫";
        } else if (is_cmd("degree")) {
            rep = "°";
        } else if (is_cmd("to") || is_cmd("rightarrow")) {
            rep = "→";
        } else if (is_cmd("Rightarrow")) {
            rep = "⇒";
        } else if (is_cmd("qquad")) {
            rep = "    ";
        } else if (is_cmd("quad")) {
            rep = "  ";
        }
        if (rep) {
            puts_str(rep);
            continue;
        }
        /* 未知命令：丢掉反斜杠，保留命令名（\sin → sin） */
        put(src + cs, clen);
    }

    dst[o] = '\0';
}

void JianTi::rebuildQuestionIndex(void)
{
    _qkey_count = 0;
    for (int i = 0; i < _card_count; i++) {
        bool seen = false;
        for (int j = 0; j < _qkey_count; j++) {
            if (_qkeys[j] == _cards[i].qkey) {
                seen = true;
                break;
            }
        }
        if (!seen && _qkey_count < JIANTI_MAX_CARDS) {
            _qkeys[_qkey_count++] = _cards[i].qkey;
        }
    }
    _q_total = (_qkey_count > 0) ? _qkey_count : 1;
    if (_q_index < 1) {
        _q_index = 1;
    }
    if (_q_index > _q_total) {
        _q_index = _q_total;
    }
}

/* 当前题默认只出思路卡；_q_unlocked 时把该题「过程与答案」卡填进下面的 _step_label。
 * 两块文字分开放在同一个竖向 flex 滚动区里：解锁后过程卡是独立 label，
 * 才能 scroll_to_view 把它滚进可视区（上一版把两段拼进同一个 label，
 * 每帧都滚回顶部，图上看不出过程卡展开，本批修掉）。 */
void JianTi::renderQuestion(void)
{
    if (!_answer_label || !_step_label || _card_count == 0 || _qkey_count == 0) {
        return;
    }
    if (!_view_buf) {
        _view_buf = static_cast<char *>(heap_caps_malloc(JIANTI_VIEW_CAP, MALLOC_CAP_SPIRAM));
        if (!_view_buf) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "render: view buffer alloc failed");
            return;
        }
    }
    if (!_step_buf) {
        _step_buf = static_cast<char *>(heap_caps_malloc(JIANTI_VIEW_CAP, MALLOC_CAP_SPIRAM));
        if (!_step_buf) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "render: step buffer alloc failed");
            return;
        }
    }

    const int key = _qkeys[_q_index - 1];
    _view_buf[0] = '\0';
    _step_buf[0] = '\0';
    size_t io = 0;
    size_t so = 0;

    auto append = [](char *buf, size_t &o, const char *s) {
        if (!s || o + 1 >= static_cast<size_t>(JIANTI_VIEW_CAP)) {
            return;
        }
        size_t room = static_cast<size_t>(JIANTI_VIEW_CAP) - 1 - o;
        size_t n = strlen(s);
        if (n > room) {
            n = room;
        }
        memcpy(buf + o, s, n);
        o += n;
        buf[o] = '\0';
    };

    bool first_idea = true;
    bool first_step = true;
    for (int i = 0; i < _card_count; i++) {
        const Card &c = _cards[i];
        if (c.qkey != key) {
            continue;
        }
        /* 过程卡只在解锁后填；未解锁时 _step_buf 为空 -> 过程 label 隐藏 */
        if (c.is_step && !_q_unlocked) {
            continue;
        }
        char *buf = c.is_step ? _step_buf : _view_buf;
        size_t &o = c.is_step ? so : io;
        bool &first = c.is_step ? first_step : first_idea;
        if (!first) {
            append(buf, o, "\n\n");
        }
        /* title 独立一行 + text 逐字原文（都来自卡，无任何改写） */
        append(buf, o, c.title);
        append(buf, o, "\n");
        append(buf, o, c.text);
        first = false;
    }

    if (io == 0) {
        snprintf(_view_buf, JIANTI_VIEW_CAP, "%s", "（本题暂无可显示卡片）");
    }

    /* 公式清洗：网关回包带 \qquad/\ne/^x 之类的 LaTeX 残码，落屏前转成可显示字符。
     * 只在本机做，不改网关（批文：不改 43 网关）。 */
    char *pretty = jt_pretty_buf();
    if (pretty) {
        jt_pretty_math(_view_buf, pretty, JIANTI_PRETTY_CAP);
        snprintf(_view_buf, JIANTI_VIEW_CAP, "%s", pretty);
        if (so > 0) {
            jt_pretty_math(_step_buf, pretty, JIANTI_PRETTY_CAP);
            snprintf(_step_buf, JIANTI_VIEW_CAP, "%s", pretty);
        }
    } else {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "render: pretty buffer alloc failed, raw text shown");
    }

    bsp_extra_display_lock(-1);
    lv_label_set_text(_answer_label, _view_buf);
    if (so > 0) {
        lv_label_set_text(_step_label, _step_buf);
        lv_obj_clear_flag(_step_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(_step_label, "");
        lv_obj_add_flag(_step_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(_answer_box, 0, LV_ANIM_OFF);
    lv_obj_update_layout(_answer_box);
    if (so > 0 && _q_unlocked) {
        /* 过程卡高于视口时 scroll_to_view 会把卡顶推出上沿（v8_06 顶部截断）。
         * 改成按过程卡 y 坐标对齐：顶部留 8px，标题与首行公式完整可见。 */
        lv_coord_t top = lv_obj_get_y(_step_label);
        lv_obj_scroll_to_y(_answer_box, (top > 8) ? (top - 8) : 0, LV_ANIM_OFF);
    }
    bsp_extra_display_unlock();

    ESP_LOGI(ESP_UTILS_LOG_TAG,
             "render: q %d/%d key=%d unlocked=%d, idea %u chars, step %u chars (step label %s)",
             _q_index, _q_total, key, (int)_q_unlocked, (unsigned)strlen(_view_buf),
             (unsigned)strlen(_step_buf), (so > 0) ? "shown" : "hidden");
}

/* 有卡就按题渲染；无卡（老格式/解析失败）回退整段 text + 1/1 */
void JianTi::showResultText(const char *fallback_text)
{
    rebuildQuestionIndex();
    if (_card_count > 0 && _qkey_count > 0) {
        _q_index = 1;
        _q_unlocked = false;
        bsp_extra_display_lock(-1);
        updatePager();
        bsp_extra_display_unlock();
        renderQuestion();
        ESP_LOGI(ESP_UTILS_LOG_TAG, "result: %d cards / %d questions, page 1/%d (idea only)",
                 _card_count, _qkey_count, _q_total);
    } else {
        _q_index = 1;
        _q_total = 1;
        _q_unlocked = false;
        bsp_extra_display_lock(-1);
        updatePager();
        bsp_extra_display_unlock();
        setAnswer(fallback_text);
        ESP_LOGW(ESP_UTILS_LOG_TAG, "result: no cards, whole-text fallback, 1/1");
    }
}

/* ---------------- 扣解题卡（8026 卡账本测试版） ---------------- */

/* 设备号：NVS(jt/devid) 有就复用；没有就用 MAC 生成 JTP4-<12HEX> 存下。
 * 只做标识+显示，不是绑定；存进 NVS 后以后不再变。
 * MAC 走 esp_efuse_mac_get_default()：读 efuse 出厂基 MAC，不依赖 WiFi 栈初始化
 * （早前用 esp_read_mac(WIFI_STA) 在 App run() 里拿到全 0，坏值还写进了 NVS，
 *  所以这里把全 0 判为无效值 → 重新生成并覆盖）。 */
void JianTi::loadDeviceId(void)
{
    char buf[sizeof(_device_id)] = {};
    size_t len = sizeof(buf);
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(JIANTI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        got = (nvs_get_str(h, JIANTI_NVS_KEY_DEVID, buf, &len) == ESP_OK) && buf[0] != '\0';
        nvs_close(h);
    }
    if (got) {
        /* 校验 "JTP4-" + 12 位十六进制，且不能是全 0（历史坏值 JTP4-000000000000） */
        bool valid = (strlen(buf) == 17 && strncmp(buf, "JTP4-", 5) == 0);
        bool allzero = true;
        for (const char *p = buf + 5; valid && *p; ++p) {
            if (*p != '0') {
                allzero = false;
            }
            if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'))) {
                valid = false;
            }
        }
        if (valid && !allzero) {
            snprintf(_device_id, sizeof(_device_id), "%s", buf);
            ESP_LOGI(ESP_UTILS_LOG_TAG, "device id (NVS reuse): %s", _device_id);
            return;
        }
        ESP_LOGW(ESP_UTILS_LOG_TAG, "device id: bad NVS value '%s', regenerate", buf);
    }

    uint8_t mac[6] = {};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "read efuse mac failed: %s", esp_err_to_name(err));
    }
    snprintf(_device_id, sizeof(_device_id), "JTP4-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "device id (%s): %s",
             got ? "regenerate: bad NVS value" : "efuse generated", _device_id);

    if (nvs_open(JIANTI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e2 = nvs_set_str(h, JIANTI_NVS_KEY_DEVID, _device_id);
        if (e2 == ESP_OK) {
            e2 = nvs_commit(h);
        }
        nvs_close(h);
        if (e2 != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "save device id failed: %s", esp_err_to_name(e2));
        }
    }
}

/* POST 网关 /card/<path>，统一带 X-JT-Device 头。
 * 返回 HTTP 状态码；0 表示网络失败/未初始化。resp 收 JSON 原文。 */
int JianTi::cardPost(const char *path, const char *body, char *resp, size_t resp_sz)
{
    if (resp && resp_sz) {
        resp[0] = '\0';
    }
    /* gatewayUrl() 形如 http://host:port/solve；换成 http://host:port/card/xxx */
    char url[160];
    const char *base = gatewayUrl();
    const char *slash = strrchr(base, '/');
    size_t keep = (slash && slash > base) ? static_cast<size_t>(slash - base) : strlen(base);
    if (keep + strlen(path) + 1 >= sizeof(url)) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "card url too long: %s%s", base, path);
        return 0;
    }
    memcpy(url, base, keep);
    url[keep] = '\0';
    strncat(url, path, sizeof(url) - strlen(url) - 1);

    struct RespCtx {
        char *buf;
        size_t cap;
        size_t len;
    } ctx = {resp, resp_sz, 0};

    auto handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
            auto *c = static_cast<RespCtx *>(evt->user_data);
            if (!c || !c->buf || c->cap < 2) {
                return ESP_OK;
            }
            size_t room = c->cap - 1 - c->len;
            size_t n = (static_cast<size_t>(evt->data_len) < room) ? static_cast<size_t>(evt->data_len) : room;
            memcpy(c->buf + c->len, evt->data, n);
            c->len += n;
            c->buf[c->len] = '\0';
        }
        return ESP_OK;
    };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    cfg.event_handler = handler;
    cfg.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "card http init failed: %s", url);
        return 0;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-JT-Device", _device_id);
    esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;

    ESP_LOGI(ESP_UTILS_LOG_TAG, "card POST %s dev=%s err=%s status=%d %lld ms resp=%.300s",
             path, _device_id, esp_err_to_name(err), status, ms, resp ? resp : "");
    return (err == ESP_OK) ? status : 0;
}

/* 点解题先问 server：有卡扣 1 张返回 true；没卡弹双入口、连不上弹提示，返回 false。 */
bool JianTi::cardUnlock(void)
{
    const int key = _qkeys[_q_index - 1];
    char body[160];
    snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"qkey\":%d}", _device_id, key);
    char resp[JIANTI_CARD_RESP_CAP];

    int status = cardPost("/card/unlock", body, resp, sizeof(resp));
    if (status != 200) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Solve: unlock failed (http=%d), no card deducted", status);
        showCardPanel(2);
        return false;
    }

    bool unlocked = false;
    int rem = 0;
    json_get_int(resp, "remaining", &rem);
    _card_last_remaining = rem;
    if (!json_get_bool(resp, "unlocked", &unlocked) || !unlocked) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Solve: NO CARD dev=%s qkey=%d remaining=%d -> dual entry",
                 _device_id, key, rem);
        showCardPanel(0);
        return false;
    }
    bool charged = true;
    json_get_bool(resp, "charged", &charged);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Solve: %s dev=%s qkey=%d remaining=%d",
             charged ? "card deducted 1 / 真扣1张" : "already unlocked / 同一题不重扣",
             _device_id, key, rem);
    return true;
}

/* 双入口「向家长申请」：发 /card/apply，成功 toast「发送成功」并立刻回思路页。 */
void JianTi::doCardApply(void)
{
    const int key = _qkeys[_q_index - 1];
    char body[160];
    snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"qkey\":%d}", _device_id, key);
    char resp[JIANTI_CARD_RESP_CAP];

    int status = cardPost("/card/apply", body, resp, sizeof(resp));
    bool ok = (status == 200);
    char app_id[64] = {};
    if (ok) {
        json_get_string(resp, "application_id", app_id, sizeof(app_id));
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Apply: dev=%s qkey=%d -> %s (http=%d id=%s)",
             _device_id, key, ok ? "SENT 发送成功" : "FAILED 发送失败", status, app_id);

    hideCardPanel();            /* 发完立刻回思路页，不留在弹窗等家长 */
    showCardToast(ok ? "发送成功" : "发送失败");
}

/* 预支确认「确认」：发 /card/preborrow，每次给 1 张、不限次数、记账。 */
void JianTi::doCardPreborrow(void)
{
    const int key = _qkeys[_q_index - 1];
    char body[160];
    snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"qkey\":%d}", _device_id, key);
    char resp[JIANTI_CARD_RESP_CAP];

    int status = cardPost("/card/preborrow", body, resp, sizeof(resp));
    bool unlocked = false;
    int rem = 0;
    json_get_int(resp, "remaining", &rem);
    _card_last_remaining = rem;
    if (status == 200) {
        json_get_bool(resp, "unlocked", &unlocked);
    }
    if (!unlocked) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Preborrow: failed (http=%d) dev=%s qkey=%d", status, _device_id, key);
        hideCardPanel();
        showCardToast("预支失败");
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Preborrow: +1 unlocked dev=%s qkey=%d remaining=%d",
             _device_id, key, rem);
    hideCardPanel();
    _q_unlocked = true;
    renderQuestion();
}

/* 弹窗三态：0=没卡双入口 1=预支确认 2=连不上服务器 */
void JianTi::showCardPanel(int mode)
{
    if (!_card_mask || !_card_panel) {
        return;
    }
    _card_panel_mode = mode;
    /* 先全隐，再按模式点亮 */
    lv_obj_add_flag(_btn_card_apply, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_card_preborrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_card_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_card_cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_card_close, LV_OBJ_FLAG_HIDDEN);

    if (mode == 0) {
        lv_label_set_text(_card_title, "没卡了");
        lv_label_set_text(_card_body, "可以请家长批张卡，也可以自己先预支一张。");
        lv_obj_clear_flag(_btn_card_apply, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_btn_card_preborrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_btn_card_close, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_btn_card_apply, LV_ALIGN_BOTTOM_LEFT, 16, -16);
        lv_obj_align(_btn_card_preborrow, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
        lv_obj_align(_btn_card_close, LV_ALIGN_TOP_RIGHT, -8, -8);
        lv_obj_set_size(_btn_card_close, 100, 48);
    } else if (mode == 1) {
        lv_label_set_text(_card_title, "自己来预支");
        lv_label_set_text(_card_body, "预支 1 张，家长在记录里能看到。\n自己做的主，保护好屁股。");
        lv_obj_clear_flag(_btn_card_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_btn_card_cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_btn_card_confirm, LV_ALIGN_BOTTOM_LEFT, 16, -16);
        lv_obj_align(_btn_card_cancel, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    } else {
        lv_label_set_text(_card_title, "连不上服务器");
        lv_label_set_text(_card_body, "这次没扣卡，等网络好了再点解题。");
        lv_obj_clear_flag(_btn_card_close, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_btn_card_close, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_size(_btn_card_close, 220, 60);
    }
    lv_obj_clear_flag(_card_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_card_mask);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "CardPanel: show mode=%d dev=%s q=%d/%d", mode, _device_id, _q_index, _q_total);
}

void JianTi::hideCardPanel(void)
{
    if (!_card_mask) {
        return;
    }
    lv_obj_add_flag(_card_mask, LV_OBJ_FLAG_HIDDEN);
    if (_card_panel_mode >= 0) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "CardPanel: hide (was mode=%d)", _card_panel_mode);
    }
    _card_panel_mode = -1;
}

void JianTi::showCardToast(const char *text)
{
    if (!_card_toast_label) {
        return;
    }
    lv_label_set_text(_card_toast_label, text ? text : "");
    lv_obj_clear_flag(_card_toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_card_toast_label);
    if (_card_toast_timer) {
        lv_timer_delete(_card_toast_timer);
        _card_toast_timer = nullptr;
    }
    _card_toast_timer = lv_timer_create(cardToastTimerCb, 1500, this);
}

void JianTi::cardToastTimerCb(lv_timer_t *t)
{
    auto *self = static_cast<JianTi *>(lv_timer_get_user_data(t));
    if (self && self->_card_toast_label) {
        lv_obj_add_flag(self->_card_toast_label, LV_OBJ_FLAG_HIDDEN);
    }
    self->_card_toast_timer = nullptr;
    lv_timer_delete(t);
}

void JianTi::onCardApplyClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (self) {
        self->doCardApply();
    }
}

void JianTi::onCardPreborrowClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (self) {
        self->showCardPanel(1);   /* 先弹确认，不直接预支 */
    }
}

void JianTi::onCardConfirmClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (self) {
        self->doCardPreborrow();
    }
}

void JianTi::onCardCancelClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (self) {
        self->hideCardPanel();
    }
}

void JianTi::onCardCloseClicked(lv_event_t *e)
{
    /* 关闭只关弹窗；申请单仍待批（关闭 != 取消） */
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (self) {
        self->hideCardPanel();
    }
}

/* 点解题：先问 server /card/unlock。
 * 已解锁再点 -> 只收起，不重复扣卡；没卡 -> 弹双入口。 */
bool JianTi::toggleSolveUnlock(void)
{
    if (_card_count == 0 || _qkey_count == 0) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Solve: no cards loaded, ignored");
        return false;
    }
    const int key = _qkeys[_q_index - 1];
    bool has_step = false;
    for (int i = 0; i < _card_count; i++) {
        if (_cards[i].qkey == key && _cards[i].is_step) {
            has_step = true;
            break;
        }
    }

    /* 已解锁：再点只收起，不问 server、不重复扣卡 */
    if (_q_unlocked) {
        _q_unlocked = false;
        ESP_LOGI(ESP_UTILS_LOG_TAG, "Solve: collapse (q %d/%d key=%d), no card deducted",
                 _q_index, _q_total, key);
        renderQuestion();
        return true;
    }
    if (!has_step) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Solve: q %d/%d has no process card, ignored", _q_index, _q_total);
        return false;
    }

    if (!cardUnlock()) {
        return false;   /* cardUnlock 内部已弹双入口/连不上提示 */
    }
    _q_unlocked = true;
    renderQuestion();
    return true;
}

void JianTi::onSolveClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    self->toggleSolveUnlock();
}

void JianTi::onPrevClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    /* 1/N 时禁用：只记日志，不动作、不跳主屏 */
    if (self->_q_index <= 1) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "UI: prev ignored (already first %d/%d), stay", self->_q_index, self->_q_total);
        return;
    }
    self->_q_index--;
    self->_q_unlocked = false;   /* 换题回到默认：只看思路 */
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: prev -> question %d/%d", self->_q_index, self->_q_total);
    bsp_extra_display_lock(-1);
    self->updatePager();
    bsp_extra_display_unlock();
    self->renderQuestion();
}

void JianTi::onNextClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    if (self->_q_index >= self->_q_total) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "UI: next ignored (already last %d/%d), stay", self->_q_index, self->_q_total);
        return;
    }
    self->_q_index++;
    self->_q_unlocked = false;   /* 换题回到默认：只看思路 */
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: next -> question %d/%d", self->_q_index, self->_q_total);
    bsp_extra_display_lock(-1);
    self->updatePager();
    bsp_extra_display_unlock();
    self->renderQuestion();
}

void JianTi::onTopEdgePressed(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    self->_edge_press_y = p.y;
}

void JianTi::onTopEdgeReleased(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    lv_coord_t dy = p.y - self->_edge_press_y;
    if (dy < 60) {
        return;   /* 不算下滑，忽略 */
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Gesture: top-edge swipe down (dy=%d) -> home capture page", (int)dy);
    bsp_extra_display_lock(-1);
    self->showPage(Page::Home);
    bsp_extra_display_unlock();
}

void JianTi::setAnswer(const char *text)
{
    if (!_answer_label) {
        return;
    }
    bsp_extra_display_lock(-1);
    char *pretty = jt_pretty_buf();
    if (pretty && text) {
        jt_pretty_math(text, pretty, JIANTI_PRETTY_CAP);
        lv_label_set_text(_answer_label, pretty);
    } else {
        lv_label_set_text(_answer_label, text ? text : "");
    }
    if (_step_label) {
        lv_label_set_text(_step_label, "");
        lv_obj_add_flag(_step_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (_answer_box) {
        lv_obj_scroll_to_y(_answer_box, 0, LV_ANIM_OFF);
    }
    bsp_extra_display_unlock();
}

void JianTi::setStatus(const char *text)
{
    setAnswer(text);
}

/* ---------------- 生命周期 ---------------- */

/* SoftAP 配网：NVS 无凭据时启动，手机连 JianTi-XXXX -> 192.168.4.1 填 WiFi */
static httpd_handle_t s_prov_server = nullptr;
static volatile bool s_prov_done = false;

static esp_err_t prov_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>简题配网</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;}"
        "input{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;}"
        "button{width:100%;padding:12px;background:#4A6CF7;color:#fff;border:none;border-radius:8px;font-size:16px;}"
        "</style></head><body>"
        "<h2>简题配网</h2>"
        "<form method='POST' action='/save'>"
        "<input name='ssid' placeholder='WiFi名称' required><br>"
        "<input name='pass' type='password' placeholder='WiFi密码' required><br>"
        "<button type='submit'>保存并重启</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t prov_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    /* 解析 application/x-www-form-urlencoded */
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        size_t n = end - p;
        if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
        strncpy(ssid, p, n);
    }
    p = strstr(buf, "pass=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        size_t n = end - p;
        if (n >= sizeof(pass)) n = sizeof(pass) - 1;
        strncpy(pass, p, n);
    }

    /* form-urlencoded 解码（%XX / +），否则中文 SSID、含特殊字符密码会写错 */
    url_decode(ssid);
    url_decode(pass);

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    /* 写 NVS */
    nvs_handle_t nh;
    if (nvs_open(JIANTI_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_str(nh, JIANTI_NVS_KEY_SSID, ssid);
        nvs_set_str(nh, JIANTI_NVS_KEY_PASS, pass);
        nvs_commit(nh);
        nvs_close(nh);
        ESP_LOGI(ESP_UTILS_LOG_TAG, "prov saved ssid=%s", ssid);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>已保存，正在重启...</h2>", HTTPD_RESP_USE_STRLEN);

    s_prov_done = true;
    return ESP_OK;
}

static void start_provisioning_ap(void)
{
    if (!s_netif_base_ready) {
        esp_netif_init();
        esp_event_loop_create_default();
        s_netif_base_ready = true;
    }

    /* 复用/建 AP netif，避免重复创建导致 netif_add assert */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!ap_netif) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "create AP netif failed");
        return;
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&cfg);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_wifi_init failed: %s", esp_err_to_name(e));
            return;
        }
        s_wifi_inited = true;
    }

    wifi_config_t ap_config = {};
    snprintf(reinterpret_cast<char *>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "JianTi-%02X%02X",
             (unsigned)(esp_random() & 0xFF), (unsigned)(esp_random() & 0xFF));
    ap_config.ap.ssid_len = strlen(reinterpret_cast<const char *>(ap_config.ap.ssid));
    ap_config.ap.channel = 1;
    ap_config.ap.password[0] = '\0';
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    /* 配网同样会 esp_wifi_start()。开机时 STA netif 已存在，若不先解绑默认 STA
     * 事件链，多次 STA_START 会让默认 handler 反复 netif_add → assert 崩溃。 */
    jianti_detach_default_sta(jianti_find_sta_netif());

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "AP start failed: %s", esp_err_to_name(e));
    }
    s_wifi_started = true;
    s_wifi_got_ip = false;

    ESP_LOGI(ESP_UTILS_LOG_TAG, "SoftAP started: ssid=%s (open), open http://192.168.4.1",
             ap_config.ap.ssid);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_prov_server, &http_cfg) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = prov_get_handler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(s_prov_server, &uri_get);

        httpd_uri_t uri_post = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = prov_post_handler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(s_prov_server, &uri_post);
        ESP_LOGI(ESP_UTILS_LOG_TAG, "HTTP server started on 192.168.4.1");
    } else {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "httpd_start failed");
    }
}

static void stop_provisioning_ap(void)
{
    if (s_prov_server) {
        httpd_stop(s_prov_server);
        s_prov_server = nullptr;
    }
    esp_wifi_stop();
}

/* 开机自联网：后台任务，不阻塞 UI */
static void jianti_wifi_boot_task(void *arg)
{
    static_cast<JianTi *>(arg)->wifiBootStart();
    vTaskDelete(nullptr);
}

bool JianTi::run(void)
{
    ESP_UTILS_LOGD("Run");

    /* 设备号先定：NVS 有就复用，无则用 WiFi MAC 生成并存 NVS（只做标识，不是绑定） */
    loadDeviceId();
    loadServerPref();

    /* 有凭据 → 正常进 UI + 后台自联网；无凭据 → 进 SoftAP 配网。
     * 凭据来源：① 简题 NVS("jt") ② esp_wifi 自身已存的 STA 配置。 */
    char ssid[64] = {0};
    char pass[64] = {0};
    bool has_creds = read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (!has_creds) {
        /* 读 esp_wifi 自身 NVS 之前必须先 init（本流程下 boot 阶段无人 init） */
        if (!s_netif_base_ready) {
            esp_netif_init();
            esp_event_loop_create_default();
            s_netif_base_ready = true;
        }
        if (!s_wifi_inited) {
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            if (esp_wifi_init(&cfg) == ESP_OK) {
                s_wifi_inited = true;
            }
        }
        if (s_wifi_inited) {
            wifi_config_t cur = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0] != '\0') {
                has_creds = true;
                ESP_LOGI(ESP_UTILS_LOG_TAG, "found stored STA config (ssid=%s)",
                         reinterpret_cast<const char *>(cur.sta.ssid));
            }
        }
    }

    if (!has_creds) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "no WiFi creds -> provisioning AP (JianTi-XXXX / 192.168.4.1)");
        /* 显示配网提示 */
        bsp_extra_display_lock(-1);
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0xF5F6F8), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *title = lv_label_create(scr);
        lv_obj_set_style_text_font(title, JIANTI_CN_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(0x1F2329), LV_PART_MAIN);
        lv_label_set_text(title, "请配网");
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

        lv_obj_t *info = lv_label_create(scr);
        lv_obj_set_style_text_font(info, JIANTI_CN_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(info, lv_color_hex(0x5B6472), LV_PART_MAIN);
        lv_label_set_text(info, "手机连热点 JianTi-XXXX\n浏览器打开 192.168.4.1\n填家里 WiFi 名称和密码");
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, 20);
        bsp_extra_display_unlock();

        start_provisioning_ap();

        /* 等配网完成 */
        while (!s_prov_done) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        stop_provisioning_ap();
        ESP_LOGI(ESP_UTILS_LOG_TAG, "provisioning saved, restart into STA");
        esp_restart();
        return true;
    }

    bsp_extra_display_lock(-1);
    buildUi();
    bsp_extra_display_unlock();

    /* 开机自联网：后台任务用已存凭据建 STA，不阻塞 UI，不依赖系统 Settings/WLAN */
    static bool s_boot_wifi_once = false;
    if (!s_boot_wifi_once) {
        s_boot_wifi_once = true;
        xTaskCreate(jianti_wifi_boot_task, "jt_wifi_boot", 6144, this, 5, nullptr);
    }

    return true;
}

bool JianTi::back(void)
{
    ESP_UTILS_LOGD("Back");
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool JianTi::close(void)
{
    ESP_UTILS_LOGD("Close");

    _viewfinder_active = false;
    _still_shown = false;
    _shutter_request = false;

    if (_capture_task_handle) {
        _capture_running = false;
        /* 给抓拍任务 3s 收尾（停流/释放缓冲），超时就不再等 */
        for (int i = 0; i < 30 && _capture_task_handle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    releasePreviewTargets();

    if (_snapshot) {
        heap_caps_free(_snapshot);
        _snapshot = nullptr;
        _snapshot_cap = 0;
    }

    bsp_extra_display_lock(-1);
    if (_card_toast_timer) {
        lv_timer_delete(_card_toast_timer);
        _card_toast_timer = nullptr;
    }
    if (_page_home) {
        lv_obj_del(_page_home);
        _page_home = nullptr;
    }
    _page_view = nullptr;
    _page_still = nullptr;
    _page_result = nullptr;
    _page_server = nullptr;
    _preview_img = nullptr;
    _btn_shutter = nullptr;
    _btn_view_back = nullptr;
    _view_status = nullptr;
    _still_img = nullptr;
    _btn_still_ok = nullptr;
    _btn_still_cancel = nullptr;
    _still_hint = nullptr;
    _answer_box = nullptr;
    _answer_label = nullptr;
    _step_label = nullptr;
    _status_label = nullptr;
    _pager_label = nullptr;
    _server_name_label = nullptr;
    _btn_capture = nullptr;
    _btn_server_entry = nullptr;
    _btn_back = nullptr;
    _btn_recap = nullptr;
    _btn_prev = nullptr;
    _btn_next = nullptr;
    _btn_srv_back = nullptr;
    _btn_lan = nullptr;
    _btn_cloud = nullptr;
    _btn_test = nullptr;
    _btn_reconfig = nullptr;
    _label_test_result = nullptr;
    _countdown_label = nullptr;
    _shot_img = nullptr;
    _btn_confirm = nullptr;
    _btn_cancel = nullptr;
    _card_mask = nullptr;
    _card_panel = nullptr;
    _card_title = nullptr;
    _card_body = nullptr;
    _btn_card_apply = nullptr;
    _btn_card_preborrow = nullptr;
    _btn_card_confirm = nullptr;
    _btn_card_cancel = nullptr;
    _btn_card_close = nullptr;
    _card_toast_label = nullptr;
    _card_panel_mode = -1;
    _shot_ready = false;
    bsp_extra_display_unlock();

    return true;
}

bool JianTi::pause(void)
{
    ESP_UTILS_LOGD("Pause");
    return true;
}

bool JianTi::resume(void)
{
    ESP_UTILS_LOGD("Resume");
    return true;
}

/* ---------------- 抓拍任务 ---------------- */

void JianTi::onCaptureClicked(lv_event_t *e)
{
    JianTi *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    if (self->_viewfinder_active) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "UI: capture ignored (viewfinder already active)");
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: capture button -> viewfinder (no countdown)");
    self->enterViewfinder();
}

/* ---------------- 取景器 / 静态确认（LVGL 层） ---------------- */

void JianTi::onShutterClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    if (!self->_viewfinder_active || self->_still_shown) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "UI: shutter ignored (not in live viewfinder)");
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: shutter -> grab one still frame");
    self->_shutter_request = true;

    bsp_extra_display_lock(-1);
    if (self->_view_status) {
        lv_label_set_text(self->_view_status, "抓拍中…");
    }
    bsp_extra_display_unlock();
}

void JianTi::onViewBackClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: viewfinder back -> home, stop stream");
    self->leaveViewfinder();
}

void JianTi::onStillOkClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self || !self->_still_shown) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Still: CONFIRMED -> encode %" PRIu32 "x%" PRIu32 " + upload (paper chain)",
             self->_still_w, self->_still_h);

    /* 1) 先把确认页这张静态帧编码成 JPEG（不动相机参数，只编码已确认的这张图） */
    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    const bool enc_ok = self->encodeJpegRgb565(self->_still_buf, self->_still_w, self->_still_h, &jpeg, &jpeg_len);

    /* 2) 收掉静态页、停预览流，结果页先显示进度 */
    self->_still_shown = false;
    self->_viewfinder_active = false;
    self->_capture_running = false;   /* 预览任务看到即 STREAMOFF 退出 */

    bsp_extra_display_lock(-1);
    if (self->_still_img) {
        lv_image_set_src(self->_still_img, nullptr);
    }
    self->showPage(Page::Result);
    self->resetCards();   /* 清掉上一轮的卡，等本轮 8025 回包再按题渲染 */
    self->_q_index = 1;
    self->_q_total = 1;
    self->updatePager();
    if (self->_answer_label) {
        lv_label_set_text(self->_answer_label, enc_ok ? "已确认，正在上传识别…" : "静态图编码失败");
    }
    lv_refr_now(NULL);
    bsp_extra_display_unlock();

    if (!enc_ok || !jpeg) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Still: confirm abort, jpeg encode failed");
        return;
    }

    /* 3) 真上传：静态图 JPEG -> 网关 -> AI 文本 -> 结果页 */
    char msg[256] = {0};
    static char s_text[4096];
    s_text[0] = '\0';
    const char *gw = self->gatewayUrl();
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Upload: POST %s, jpeg=%u bytes", gw, (unsigned)jpeg_len);

    if (gw[0] == '\0') {
        self->setAnswer("网关地址未配置");
    } else if (!self->wifiEnsureConnected()) {
        self->setAnswer("WiFi 没连上，先到服务器页配网");
    } else if (self->uploadJpeg(jpeg, jpeg_len, s_text, sizeof(s_text))) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "AI answer (%u chars): %s", (unsigned)strlen(s_text), s_text);
        self->showResultText(s_text[0] ? s_text : "网关返回空");
    } else {
        snprintf(msg, sizeof(msg), "上传失败（网关 %s）", gw);
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Upload: FAILED %s", msg);
        self->setAnswer(msg);
    }
    heap_caps_free(jpeg);

    bsp_extra_display_lock(-1);
    self->updatePager();
    lv_refr_now(NULL);
    bsp_extra_display_unlock();

    ESP_LOGI(ESP_UTILS_LOG_TAG, "Still: confirm done, parked at result page");
}

void JianTi::onStillCancelClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Still: CANCELLED, still frame discarded, no upload");

    self->_still_shown = false;
    bsp_extra_display_lock(-1);
    lv_image_set_src(self->_still_img, nullptr);
    if (self->_view_status) {
        lv_label_set_text(self->_view_status, "已取消，未上传");
    }
    self->showPage(Page::Viewfinder);
    bsp_extra_display_unlock();
}

bool JianTi::enterViewfinder(void)
{
    /* 上一次取景任务可能还在收尾（STREAMOFF / 释放缓冲），等它退出再建新任务 */
    for (int i = 0; i < 60 && _capture_task_handle; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (_capture_task_handle) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Viewfinder: previous task still running, abort");
        return false;
    }

    _still_shown = false;
    _shutter_request = false;

    bsp_extra_display_lock(-1);
    if (_view_status) {
        lv_label_set_text(_view_status, "预览中…");
    }
    showPage(Page::Viewfinder);
    bsp_extra_display_unlock();

    _viewfinder_active = true;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Viewfinder: enter -> create stream");
    if (!startCapture()) {
        _viewfinder_active = false;
        return false;
    }
    return true;
}

void JianTi::leaveViewfinder(void)
{
    _viewfinder_active = false;
    _still_shown = false;
    _shutter_request = false;
    /* 预览任务看到这个标志就 STREAMOFF + 释放缓冲后退出 */
    _capture_running = false;

    bsp_extra_display_lock(-1);
    if (_still_img) {
        lv_image_set_src(_still_img, nullptr);
    }
    showPage(Page::Home);
    bsp_extra_display_unlock();
}

esp_err_t JianTi::computePreviewGeometry(void)
{
    if (_camera_width == 0 || _camera_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t cam_w = _camera_width;
    const uint32_t cam_h = _camera_height;
    uint32_t steps = JIANTI_VIEW_SCALE_STEPS;   /* 10/16 = 0.625 */
    uint32_t ppa_out_h = JIANTI_VIEW_H;

    _preview_w = JIANTI_VIEW_W;          /* LVGL 画布宽（恒 800） */
    _preview_h = JIANTI_VIEW_H;          /* 画布高：铺满模式改 480 */
    _preview_block_h = cam_h;            /* 竖向始终用整高，靠横向裁/缩适配 */
    _preview_out_off_x = 0;
    _preview_out_off_y = 0;

    switch (_view_fill_mode) {
    case 1:                              /* a. 裁切铺满：800x480 无形变 */
        ppa_out_h = JIANTI_FILL_H;
        _preview_h = JIANTI_FILL_H;
        break;
    case 2:                              /* b. 拉伸铺满：整宽 -> 800x500，CPU 压 480 */
        ppa_out_h = 0;                   /* 0 = 按整宽反推 */
        _preview_h = JIANTI_FILL_H;
        break;
    case 3:                              /* c. 等比留边：9/16 -> 720x450 居中 */
        steps = 9;
        ppa_out_h = 0;
        _preview_h = JIANTI_FILL_H;
        break;
    default:                             /* 0 = V7 原样 800x360 */
        break;
    }

    uint32_t block_w;
    if (ppa_out_h == 0) {
        block_w = cam_w;
        ppa_out_h = static_cast<uint32_t>(static_cast<uint64_t>(block_w) * steps / PPA_SCALE_STEPS_PER_UNIT);
    } else {
        block_w = static_cast<uint32_t>(static_cast<uint64_t>(ppa_out_h) * PPA_SCALE_STEPS_PER_UNIT / steps);
        if (block_w > cam_w) {
            block_w = cam_w;
        }
        ppa_out_h = static_cast<uint32_t>(static_cast<uint64_t>(block_w) * steps / PPA_SCALE_STEPS_PER_UNIT);
    }

    _preview_block_w = block_w;
    _preview_block_off_x = (cam_w - block_w) / 2;
    _preview_block_off_y = (cam_h - _preview_block_h) / 2;
    _preview_scale = static_cast<float>(steps) / PPA_SCALE_STEPS_PER_UNIT;
    _preview_scale_x = _preview_scale;
    _preview_scale_y = _preview_scale;
    _ppa_out_w = static_cast<uint32_t>(static_cast<uint64_t>(_preview_block_h) * steps / PPA_SCALE_STEPS_PER_UNIT);
    _ppa_out_h = ppa_out_h;

    if (_view_fill_mode == 3) {
        _preview_out_off_x = (JIANTI_FILL_W - _ppa_out_w) / 2;
        _preview_out_off_y = (JIANTI_FILL_H - _ppa_out_h) / 2;
    }
    return ESP_OK;
}

esp_err_t JianTi::preparePreviewTargets(void)
{
    ESP_RETURN_ON_ERROR(computePreviewGeometry(), ESP_UTILS_LOG_TAG, "Compute preview geometry failed");

    if (_ppa_srm_handle == nullptr) {
        ppa_client_config_t ppa_srm_config = {
            .oper_type = PPA_OPERATION_SRM,
        };
        ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_srm_config, &_ppa_srm_handle),
                            ESP_UTILS_LOG_TAG, "Register PPA SRM failed");
    }

    /* 画布尺寸随铺满方式变（800x360 / 800x480），尺寸变了就整块重分配 */
    const size_t need_preview = static_cast<size_t>(_preview_w) * _preview_h * JIANTI_BYTES_PER_PIXEL;
    if (_preview_buf_size != need_preview) {
        for (int i = 0; i < 2; i++) {
            if (_preview_buf[i]) {
                heap_caps_free(_preview_buf[i]);
                _preview_buf[i] = nullptr;
            }
        }
        _preview_buf_size = 0;
    }
    for (int i = 0; i < 2; i++) {
        if (!_preview_buf[i]) {
            _preview_buf[i] = static_cast<uint8_t *>(
                                  heap_caps_aligned_calloc(_data_cache_line_size, 1, need_preview,
                                          MALLOC_CAP_SPIRAM));
            if (!_preview_buf[i]) {
                ESP_LOGE(ESP_UTILS_LOG_TAG, "Preview buffer %d alloc failed: %u bytes", i,
                         static_cast<unsigned>(need_preview));
                releasePreviewTargets();
                return ESP_ERR_NO_MEM;
            }
        }
        memset(&_preview_dsc[i], 0, sizeof(_preview_dsc[i]));
        _preview_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        _preview_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        _preview_dsc[i].header.w = _preview_w;
        _preview_dsc[i].header.h = _preview_h;
        _preview_dsc[i].header.stride = _preview_w * JIANTI_BYTES_PER_PIXEL;
        _preview_dsc[i].data = _preview_buf[i];
        _preview_dsc[i].data_size = static_cast<uint32_t>(need_preview);
    }
    _preview_buf_size = need_preview;
    _preview_idx = 0;

    /* 换铺法时画布可能同尺寸（800x480 -> 800x480），缓冲不重分配，
     * 留边区域会残留上一种铺法的旧帧。这里清成白底并回写，
     * 让"留边"是真的留边，而不是上一帧的残影。 */
    for (int i = 0; i < 2; i++) {
        memset(_preview_buf[i], 0xFF, need_preview);
        esp_cache_msync(_preview_buf[i], need_preview, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    /* 拉伸模式：PPA 先出 800x500 的中间帧，再 CPU 压成 800x480 */
    const size_t need_tmp = (_view_fill_mode == 2)
                            ? static_cast<size_t>(_ppa_out_w) * _ppa_out_h * JIANTI_BYTES_PER_PIXEL : 0;
    if (_stretch_tmp_size != need_tmp) {
        if (_stretch_tmp) {
            heap_caps_free(_stretch_tmp);
            _stretch_tmp = nullptr;
        }
        _stretch_tmp_size = 0;
    }
    if (need_tmp > 0 && !_stretch_tmp) {
        _stretch_tmp = static_cast<uint8_t *>(
                           heap_caps_aligned_calloc(_data_cache_line_size, 1, need_tmp, MALLOC_CAP_SPIRAM));
        if (!_stretch_tmp) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Stretch tmp alloc failed: %u bytes",
                     static_cast<unsigned>(need_tmp));
            releasePreviewTargets();
            return ESP_ERR_NO_MEM;
        }
        _stretch_tmp_size = need_tmp;
    }

    _still_w = _still_fill_480 ? JIANTI_FILL_W : JIANTI_STILL_W;
    _still_h = _still_fill_480 ? JIANTI_FILL_H : JIANTI_STILL_H;
    const size_t need_still = static_cast<size_t>(_still_w) * _still_h * JIANTI_BYTES_PER_PIXEL;
    if (_still_buf_size != need_still) {
        if (_still_buf) {
            heap_caps_free(_still_buf);
            _still_buf = nullptr;
        }
        _still_buf_size = 0;
    }
    if (!_still_buf) {
        _still_buf = static_cast<uint8_t *>(
                         heap_caps_aligned_calloc(_data_cache_line_size, 1, need_still, MALLOC_CAP_SPIRAM));
        if (!_still_buf) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Still buffer alloc failed: %u bytes",
                     static_cast<unsigned>(need_still));
            releasePreviewTargets();
            return ESP_ERR_NO_MEM;
        }
    }
    _still_buf_size = need_still;
    memset(&_still_dsc, 0, sizeof(_still_dsc));
    _still_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    _still_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    _still_dsc.header.w = _still_w;
    _still_dsc.header.h = _still_h;
    _still_dsc.header.stride = _still_w * JIANTI_BYTES_PER_PIXEL;
    _still_dsc.data = _still_buf;
    _still_dsc.data_size = static_cast<uint32_t>(need_still);

    /* 控件尺寸跟随画布（铺满模式下预览图改 800x480，按钮仍浮在图上） */
    bsp_extra_display_lock(-1);
    if (_preview_img) {
        lv_obj_set_size(_preview_img, _preview_w, _preview_h);
        lv_obj_align(_preview_img, LV_ALIGN_TOP_MID, 0, 0);
    }
    if (_still_img) {
        lv_obj_set_size(_still_img, _still_w, _still_h);
        lv_obj_align(_still_img, LV_ALIGN_TOP_MID, 0, 0);
    }
    bsp_extra_display_unlock();

    ESP_LOGI(ESP_UTILS_LOG_TAG,
             "Preview targets: cam=%" PRIu32 "x%" PRIu32 " mode=%d block=%ux%u off=(%u,%u) scale=%.3f"
             " -> ppa=%" PRIu32 "x%" PRIu32 " canvas=%" PRIu32 "x%" PRIu32 " outoff=(%u,%u) still=%" PRIu32 "x%" PRIu32,
             _camera_width, _camera_height, _view_fill_mode, static_cast<unsigned>(_preview_block_w),
             static_cast<unsigned>(_preview_block_h), static_cast<unsigned>(_preview_block_off_x),
             static_cast<unsigned>(_preview_block_off_y), _preview_scale, _ppa_out_w, _ppa_out_h,
             _preview_w, _preview_h, _preview_out_off_x, _preview_out_off_y, _still_w, _still_h);
    return ESP_OK;
}

esp_err_t JianTi::renderPreviewFrame(const void *camera_buf)
{
    if (!camera_buf || !_preview_buf[0] || !_preview_buf[1] || !_preview_img) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool squash = (_view_fill_mode == 2);
    if (squash && !_stretch_tmp) {
        return ESP_ERR_INVALID_STATE;
    }

    const int idx = _preview_idx;
    uint8_t *dst = _preview_buf[idx];
    uint8_t *ppa_dst = squash ? _stretch_tmp : dst;
    const uint32_t ppa_w = squash ? _ppa_out_w : _preview_w;
    const uint32_t ppa_h = squash ? _ppa_out_h : _preview_h;
    const size_t ppa_size = squash ? _stretch_tmp_size : _preview_buf_size;

    ppa_srm_oper_config_t srm_config = {};
    srm_config.in.buffer = const_cast<void *>(camera_buf);
    srm_config.in.pic_w = _camera_width;
    srm_config.in.pic_h = _camera_height;
    srm_config.in.block_w = static_cast<uint32_t>(_preview_block_w);
    srm_config.in.block_h = static_cast<uint32_t>(_preview_block_h);
    srm_config.in.block_offset_x = static_cast<uint32_t>(_preview_block_off_x);
    srm_config.in.block_offset_y = static_cast<uint32_t>(_preview_block_off_y);
    srm_config.in.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.out.buffer = ppa_dst;
    srm_config.out.buffer_size = static_cast<uint32_t>(ppa_size);
    srm_config.out.pic_w = ppa_w;
    srm_config.out.pic_h = ppa_h;
    srm_config.out.block_offset_x = squash ? 0 : _preview_out_off_x;
    srm_config.out.block_offset_y = squash ? 0 : _preview_out_off_y;
    srm_config.out.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.rotation_angle = JIANTI_PREVIEW_ROTATION;
    srm_config.scale_x = _preview_scale_x;
    srm_config.scale_y = _preview_scale_y;
    srm_config.mirror_x = JIANTI_PREVIEW_MIRROR_X;
    srm_config.mirror_y = JIANTI_PREVIEW_MIRROR_Y;
    srm_config.rgb_swap = 0;
    srm_config.byte_swap = 0;
    srm_config.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_srm_handle, &srm_config);
    if (ret != ESP_OK) {
        return ret;
    }

    if (squash) {
        /* 竖向最近邻 500 -> 480（横向不变，约 4% 竖向压缩即"拉伸变形"的来源） */
        ret = esp_cache_msync(_stretch_tmp, _stretch_tmp_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (ret != ESP_OK) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Stretch tmp cache sync failed: %s", esp_err_to_name(ret));
        }
        const size_t stride = static_cast<size_t>(_preview_w) * JIANTI_BYTES_PER_PIXEL;
        for (uint32_t y = 0; y < _preview_h; y++) {
            const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * _ppa_out_h / _preview_h);
            memcpy(dst + static_cast<size_t>(y) * stride,
                   _stretch_tmp + static_cast<size_t>(sy) * stride, stride);
        }
    }

    ret = esp_cache_msync(dst, _preview_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Preview cache sync failed: %s", esp_err_to_name(ret));
    }

    bsp_extra_display_lock(-1);
    lv_image_set_src(_preview_img, &_preview_dsc[idx]);
    lv_obj_invalidate(_preview_img);
    bsp_extra_display_unlock();

    _preview_idx ^= 1;
    return ESP_OK;
}

esp_err_t JianTi::renderStillFrame(const void *camera_buf)
{
    if (!camera_buf || !_still_buf || !_still_img) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_off_x;
    uint32_t block_off_y;
    uint32_t out_off_x = 0;
    uint32_t out_off_y = 0;
    float scale;

    if (_still_fill_480) {
        /* 与「裁切铺满」同几何：block 768x1280 @10/16 -> 800x480，无形变铺满 */
        const uint32_t steps = JIANTI_VIEW_SCALE_STEPS;
        block_h = _camera_height;
        block_w = static_cast<uint32_t>(static_cast<uint64_t>(JIANTI_FILL_H) *
                                        PPA_SCALE_STEPS_PER_UNIT / steps);
        if (block_w > _camera_width) {
            block_w = _camera_width;
        }
        block_off_x = (_camera_width - block_w) / 2;
        block_off_y = (_camera_height - block_h) / 2;
        scale = static_cast<float>(steps) / PPA_SCALE_STEPS_PER_UNIT;
    } else {
        /* 与取景同一块中心裁切：旋转后源尺寸 = block_h x block_w；
         * 等比 contain 进 800x392（不拉伸），与取景器同放大倍率。 */
        const ContainFitConfig fit = computeContainFit(_preview_block_h, _preview_block_w, _still_w, _still_h);
        ESP_RETURN_ON_FALSE(fit.scale > 0.0f, ESP_ERR_INVALID_SIZE, ESP_UTILS_LOG_TAG, "Invalid still fit");
        block_w = static_cast<uint32_t>(_preview_block_w);
        block_h = static_cast<uint32_t>(_preview_block_h);
        block_off_x = static_cast<uint32_t>(_preview_block_off_x);
        block_off_y = static_cast<uint32_t>(_preview_block_off_y);
        out_off_x = fit.output_offset_x;
        out_off_y = fit.output_offset_y;
        scale = fit.scale;
    }

    ppa_srm_oper_config_t srm_config = {};
    srm_config.in.buffer = const_cast<void *>(camera_buf);
    srm_config.in.pic_w = _camera_width;
    srm_config.in.pic_h = _camera_height;
    srm_config.in.block_w = block_w;
    srm_config.in.block_h = block_h;
    srm_config.in.block_offset_x = block_off_x;
    srm_config.in.block_offset_y = block_off_y;
    srm_config.in.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.out.buffer = _still_buf;
    srm_config.out.buffer_size = static_cast<uint32_t>(_still_buf_size);
    srm_config.out.pic_w = _still_w;
    srm_config.out.pic_h = _still_h;
    srm_config.out.block_offset_x = out_off_x;
    srm_config.out.block_offset_y = out_off_y;
    srm_config.out.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.rotation_angle = JIANTI_PREVIEW_ROTATION;
    srm_config.scale_x = scale;
    srm_config.scale_y = scale;
    srm_config.mirror_x = JIANTI_PREVIEW_MIRROR_X;
    srm_config.mirror_y = JIANTI_PREVIEW_MIRROR_Y;
    srm_config.rgb_swap = 0;
    srm_config.byte_swap = 0;
    srm_config.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_srm_handle, &srm_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_cache_msync(_still_buf, _still_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Still cache sync failed: %s", esp_err_to_name(ret));
    }

    bsp_extra_display_lock(-1);
    lv_image_set_src(_still_img, &_still_dsc);
    lv_obj_invalidate(_still_img);
    showPage(Page::Still);
    bsp_extra_display_unlock();

    /* 置位后预览循环暂停刷新，直到确认/取消回到取景器 */
    _still_shown = true;

    ESP_LOGI(ESP_UTILS_LOG_TAG,
             "Still: %" PRIu32 "x%" PRIu32 " (fill480=%d block=%ux%u scale=%.3f outoff=(%u,%u)), memory only",
             _still_w, _still_h, _still_fill_480 ? 1 : 0, block_w, block_h, scale, out_off_x, out_off_y);
    return ESP_OK;
}

void JianTi::releasePreviewTargets(void)
{
    for (int i = 0; i < 2; i++) {
        if (_preview_buf[i]) {
            heap_caps_free(_preview_buf[i]);
            _preview_buf[i] = nullptr;
        }
        memset(&_preview_dsc[i], 0, sizeof(_preview_dsc[i]));
    }
    _preview_buf_size = 0;
    _preview_idx = 0;

    if (_stretch_tmp) {
        heap_caps_free(_stretch_tmp);
        _stretch_tmp = nullptr;
    }
    _stretch_tmp_size = 0;

    if (_still_buf) {
        heap_caps_free(_still_buf);
        _still_buf = nullptr;
    }
    memset(&_still_dsc, 0, sizeof(_still_dsc));
    _still_buf_size = 0;
}

void JianTi::showShotConfirm(void)
{
    if (!_shot_img || !_snapshot || _snapshot_len == 0) {
        return;
    }
    bsp_extra_display_lock(-1);
    /* 隐藏答案框/分页/导航，显示拍图+确认取消 */
    lv_obj_add_flag(_answer_label->parent, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_pager_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_prev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_recap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_solve, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_countdown_label, LV_OBJ_FLAG_HIDDEN);

    /* 把 snapshot 显示为静态图 */
    _shot_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    _shot_dsc.header.w = _camera_width;
    _shot_dsc.header.h = _camera_height;
    _shot_dsc.header.stride = _camera_width * JIANTI_BYTES_PER_PIXEL;
    _shot_dsc.data = _snapshot;
    _shot_dsc.data_size = _snapshot_len;
    lv_image_set_src(_shot_img, &_shot_dsc);
    lv_obj_clear_flag(_shot_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    _shot_ready = true;
    setStatus("拍好了，确认发送还是取消重拍？");
    bsp_extra_display_unlock();
}

void JianTi::clearShotConfirm(void)
{
    bsp_extra_display_lock(-1);
    lv_obj_add_flag(_shot_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_answer_label->parent, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_pager_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_prev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_recap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_btn_solve, LV_OBJ_FLAG_HIDDEN);
    _shot_ready = false;
    bsp_extra_display_unlock();
}

void JianTi::onConfirmClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self || !self->_shot_ready) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: confirm -> upload shot");
    self->clearShotConfirm();
    self->setStatus("正在上传…");

    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    char msg[256] = {0};
    if (self->encodeJpeg(&jpeg, &jpeg_len)) {
        static char s_text[4096];
        s_text[0] = '\0';
        if (self->gatewayUrl()[0] == '\0') {
            self->setStatus("网关地址未配置");
        } else if (!self->wifiEnsureConnected()) {
            self->setStatus("WiFi 没连上，先到服务器页配网");
        } else if (self->uploadJpeg(jpeg, jpeg_len, s_text, sizeof(s_text))) {
            self->showResultText(s_text[0] ? s_text : "网关返回空");
        } else {
            snprintf(msg, sizeof(msg), "上传失败（网关 %s）", self->gatewayUrl());
            self->setStatus(msg);
        }
        heap_caps_free(jpeg);
    } else {
        self->setStatus("JPEG 编码失败");
    }

    if (self->_snapshot) {
        heap_caps_free(self->_snapshot);
        self->_snapshot = nullptr;
        self->_snapshot_cap = 0;
    }
}

void JianTi::onCancelClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: cancel -> back to home, reshoot");
    self->clearShotConfirm();
    if (self->_snapshot) {
        heap_caps_free(self->_snapshot);
        self->_snapshot = nullptr;
        self->_snapshot_cap = 0;
    }
    bsp_extra_display_lock(-1);
    self->showPage(Page::Home);
    bsp_extra_display_unlock();
}

void JianTi::onTestClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: test connection");
    self->doServerTest();
}

void JianTi::onReconfigClicked(lv_event_t *e)
{
    auto *self = static_cast<JianTi *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "UI: reconfig wifi");
    self->doWifiReconfig();
}

bool JianTi::startCapture()
{
    if (_capture_task_handle) {
        return true;
    }

    _capture_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
                         captureTaskEntry,
                         "jianti_capture",
                         CAPTURE_TASK_STACK_SIZE,
                         this,
                         CAPTURE_TASK_PRIORITY,
                         &_capture_task_handle,
                         0);
    if (ret != pdPASS) {
        _capture_running = false;
        _capture_task_handle = nullptr;
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Create capture task failed");
        setStatus("任务创建失败");
        return false;
    }
    return true;
}

void JianTi::captureTaskEntry(void *arg)
{
    static_cast<JianTi *>(arg)->captureTask();
}

void JianTi::captureTask()
{
    /* 取景任务：进拍题建流 -> 持续预览；_shutter_request 置位时抓一帧静态图；
     * 离开拍题（_capture_running=false）才 STREAMOFF 停流。无 3-2-1 倒计时。 */
    esp_err_t ret = initVideoDriver();
    if (ret == ESP_OK) {
        ret = openVideoDevice();
    }
    if (ret == ESP_OK) {
        ret = setupCameraBuffers();
    }
    if (ret == ESP_OK) {
        ret = preparePreviewTargets();
    }
    if (ret == ESP_OK) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_STREAMON, &type) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_STREAMON failed");
            ret = ESP_FAIL;
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Viewfinder: stream start failed: %s", esp_err_to_name(ret));
        bsp_extra_display_lock(-1);
        if (_view_status) {
            lv_label_set_text(_view_status, "取景启动失败");
        }
        bsp_extra_display_unlock();
    } else {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "Viewfinder: stream on, preview %" PRIu32 "x%" PRIu32, _preview_w,
                 _preview_h);
    }

    int frames = 0;
    int fps_frames = 0;
    int64_t fps_t0 = esp_timer_get_time();
    while (_capture_running && ret == ESP_OK) {
        struct v4l2_buffer v4l2_buf = {};
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_buf.memory = V4L2_MEMORY_USERPTR;

        if (ioctl(_video_fd, VIDIOC_DQBUF, &v4l2_buf) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_DQBUF failed");
            ret = ESP_FAIL;
            break;
        }
        if (v4l2_buf.index >= CAMERA_BUFFER_COUNT) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Invalid buffer index: %u", static_cast<unsigned>(v4l2_buf.index));
            ret = ESP_ERR_INVALID_STATE;
            break;
        }

        void *cam = _camera_buffers[v4l2_buf.index];
        esp_cache_msync(cam, _camera_buffer_lengths[v4l2_buf.index], ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        if (_shutter_request && frames >= PREVIEW_WARMUP_FRAMES && !_still_shown) {
            /* 用户点拍摄：抓这一帧到内存，放大显示静态确认页（不存 TF/SD、不上传） */
            _shutter_request = false;
            esp_err_t sr = renderStillFrame(cam);
            if (sr == ESP_OK) {
                ESP_LOGI(ESP_UTILS_LOG_TAG, "Shutter: frame %d -> still page, memory only", frames);
            } else {
                ESP_LOGE(ESP_UTILS_LOG_TAG, "Shutter: render still failed: %s", esp_err_to_name(sr));
                bsp_extra_display_lock(-1);
                if (_view_status) {
                    lv_label_set_text(_view_status, "抓拍失败，请重试");
                }
                bsp_extra_display_unlock();
            }
        } else if (!_still_shown) {
            renderPreviewFrame(cam);
        }

        v4l2_buf.m.userptr = (unsigned long)cam;
        v4l2_buf.length = _camera_buffer_lengths[v4l2_buf.index];
        if (ioctl(_video_fd, VIDIOC_QBUF, &v4l2_buf) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_QBUF failed");
            ret = ESP_FAIL;
            break;
        }

        frames++;
        fps_frames++;
        int64_t now = esp_timer_get_time();
        if (now - fps_t0 >= 2000000) {
            ESP_LOGI(ESP_UTILS_LOG_TAG, "Preview fps=%.1f mode=%d frames=%d canvas=%" PRIu32 "x%" PRIu32,
                     static_cast<float>(fps_frames) * 1000000.0f / static_cast<float>(now - fps_t0),
                     _view_fill_mode, frames, _preview_w, _preview_h);
            fps_frames = 0;
            fps_t0 = now;
        }
        if (_preview_interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(_preview_interval_ms));
        } else {
            vTaskDelay(1);
        }
    }

    if (_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(_video_fd, VIDIOC_STREAMOFF, &type);
    }
    releaseCameraBuffers();
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Viewfinder: stream off (%d frames)", frames);

    _still_shown = false;
    _shutter_request = false;
    _viewfinder_active = false;
    _capture_running = false;
    _capture_task_handle = nullptr;
    vTaskDelete(NULL);
}

/* ---------------- 摄像头（照 Camera App） ---------------- */

esp_err_t JianTi::initVideoDriver()
{
    if (_video_driver_initialized) {
        return ESP_OK;
    }

    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = false,
                .i2c_handle = bsp_i2c_get_handle(),
                .freq = CONFIG_BSP_I2C_CLK_SPEED_HZ,
            },
            .reset_pin = GPIO_NUM_NC,
            .pwdn_pin = GPIO_NUM_NC,
        },
    };

    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };

    esp_err_t ret = esp_video_init(&cam_config);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        _video_driver_initialized = true;
        return ESP_OK;
    }

    ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t JianTi::openVideoDevice()
{
    if (_video_fd >= 0) {
        return ESP_OK;
    }

    // Open the MIPI-CSI capture device (/dev/video0). When the sensor provides
    // isp_info, frames are routed through the hardware ISP (RAW->RGB565).
    _video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (_video_fd < 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Open %s failed, errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        return ESP_FAIL;
    }

    struct v4l2_capability capability = {};
    if (ioctl(_video_fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_QUERYCAP on %s failed, errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        ::close(_video_fd);
        _video_fd = -1;
        return ESP_FAIL;
    }

    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_G_FMT failed, errno=%d", errno);
        ::close(_video_fd);
        _video_fd = -1;
        return ESP_FAIL;
    }
    if (format.fmt.pix.width == 0 || format.fmt.pix.height == 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Camera returned an invalid capture size");
        ::close(_video_fd);
        _video_fd = -1;
        return ESP_ERR_INVALID_SIZE;
    }

    // The CSI node keeps the sensor's native resolution; only request the
    // target color format when the default differs.
    if (format.fmt.pix.pixelformat != JIANTI_VIDEO_FMT) {
        struct v4l2_format requested_format = format;
        requested_format.fmt.pix.pixelformat = JIANTI_VIDEO_FMT;
        if (ioctl(_video_fd, VIDIOC_S_FMT, &requested_format) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_S_FMT color conversion failed for %" PRIu32 "x%" PRIu32 ", errno=%d",
                     format.fmt.pix.width, format.fmt.pix.height, errno);
            ::close(_video_fd);
            _video_fd = -1;
            return ESP_FAIL;
        }

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_FMT, &format) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_G_FMT after color conversion failed, errno=%d", errno);
            ::close(_video_fd);
            _video_fd = -1;
            return ESP_FAIL;
        }
    }

    if (format.fmt.pix.pixelformat != JIANTI_VIDEO_FMT) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Unsupported camera pixel format: 0x%08" PRIx32,
                 format.fmt.pix.pixelformat);
        ::close(_video_fd);
        _video_fd = -1;
        return ESP_ERR_NOT_SUPPORTED;
    }

    _camera_width = format.fmt.pix.width;
    _camera_height = format.fmt.pix.height;
    _camera_buffer_size = format.fmt.pix.sizeimage;
    if (_camera_buffer_size == 0) {
        _camera_buffer_size = _camera_width * _camera_height * JIANTI_BYTES_PER_PIXEL;
    }

    ESP_LOGI(ESP_UTILS_LOG_TAG, "%s ready: %" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32 ", buffer: %u bytes",
             ESP_VIDEO_MIPI_CSI_DEVICE_NAME, _camera_width, _camera_height,
             format.fmt.pix.pixelformat, static_cast<unsigned>(_camera_buffer_size));
    return ESP_OK;
}

esp_err_t JianTi::setupCameraBuffers()
{
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
    _data_cache_line_size = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
    _data_cache_line_size = 64;
#endif

    struct v4l2_requestbuffers req = {};
    req.count = CAMERA_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_REQBUFS failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < CAMERA_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;

        if (ioctl(_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_QUERYBUF %d failed", i);
            return ESP_FAIL;
        }

        size_t alloc_size = buf.length ? buf.length : _camera_buffer_size;
        _camera_buffer_lengths[i] = alloc_size;
        _camera_buffers[i] = heap_caps_aligned_calloc(_data_cache_line_size, 1, alloc_size, MALLOC_CAP_SPIRAM);
        if (!_camera_buffers[i]) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Camera buffer %d allocation failed", i);
            return ESP_ERR_NO_MEM;
        }

        buf.m.userptr = (unsigned long)_camera_buffers[i];
        buf.length = alloc_size;

        if (ioctl(_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_QBUF %d failed", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t JianTi::startDummyPreview()
{
    _display = lv_display_get_default();
    ESP_RETURN_ON_FALSE(_display != nullptr, ESP_ERR_INVALID_STATE, ESP_UTILS_LOG_TAG,
                        "LVGL display is not ready");

    _display_panel = bsp_display_get_panel_handle();
    ESP_RETURN_ON_FALSE(_display_panel != nullptr, ESP_ERR_INVALID_STATE, ESP_UTILS_LOG_TAG,
                        "LCD panel is not ready");

    esp_err_t ret = ESP_OK;
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS >= 3
    ret = esp_lcd_dpi_panel_get_frame_buffer(_display_panel, CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
                                             &_lcd_buffers[0], &_lcd_buffers[1], &_lcd_buffers[2]);
#elif CONFIG_BSP_LCD_DPI_BUFFER_NUMS == 2
    ret = esp_lcd_dpi_panel_get_frame_buffer(_display_panel, CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
                                             &_lcd_buffers[0], &_lcd_buffers[1]);
#else
#error "JianTi preview needs at least two LCD frame buffers"
#endif
    ESP_RETURN_ON_ERROR(ret, ESP_UTILS_LOG_TAG, "Get LCD frame buffers failed");
    _current_lcd_buffer_index = 0;

    if (_ppa_srm_handle == nullptr) {
        ppa_client_config_t ppa_srm_config = {
            .oper_type = PPA_OPERATION_SRM,
        };
        ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_srm_config, &_ppa_srm_handle),
                            ESP_UTILS_LOG_TAG, "Register PPA SRM failed");
    }

    /* 预览阶段另开一个触摸句柄时，旋转与触摸标志必须和显示创建时成对，
     * 否则会出现"点右下、认左上"的对角错位。 */
    bsp_display_cfg_t touch_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    ret = bsp_touch_new(&touch_cfg, &_touch_handle);
    if (ret != ESP_OK) {
        stopDummyPreview();
        return ret;
    }

    ret = esp_lv_adapter_set_dummy_draw(_display, true);
    if (ret != ESP_OK) {
        stopDummyPreview();
        return ret;
    }
    _dummy_enabled = true;

    ret = esp_lv_adapter_pause(-1);
    if (ret != ESP_OK) {
        stopDummyPreview();
        return ret;
    }
    _lvgl_paused = true;

    const size_t lcd_buffer_size =
        static_cast<size_t>(BSP_LCD_H_RES) * BSP_LCD_V_RES * JIANTI_BYTES_PER_PIXEL;
    for (int i = 0; i < CONFIG_BSP_LCD_DPI_BUFFER_NUMS; i++) {
        memset(_lcd_buffers[i], 0, lcd_buffer_size);
        ret = esp_cache_msync(_lcd_buffers[i], lcd_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (ret != ESP_OK) {
            stopDummyPreview();
            return ret;
        }
    }

    return ESP_OK;
}

void JianTi::stopDummyPreview()
{
    if (_dummy_enabled && _display) {
        esp_lv_adapter_set_dummy_draw(_display, false);
        _dummy_enabled = false;
    }

    if (_lvgl_paused) {
        esp_lv_adapter_resume();
        _lvgl_paused = false;
    }

    if (_touch_handle) {
        esp_lcd_touch_del(_touch_handle);
        _touch_handle = nullptr;
    }

    memset(_lcd_buffers, 0, sizeof(_lcd_buffers));
    _display_panel = nullptr;
    _display = nullptr;
    _current_lcd_buffer_index = 0;
}

void JianTi::releaseCameraBuffers()
{
    for (int i = 0; i < CAMERA_BUFFER_COUNT; i++) {
        if (_camera_buffers[i]) {
            heap_caps_free(_camera_buffers[i]);
            _camera_buffers[i] = nullptr;
        }
        _camera_buffer_lengths[i] = 0;
    }

    if (_video_fd >= 0) {
        ::close(_video_fd);
        _video_fd = -1;
    }
}

esp_err_t JianTi::handleFrame()
{
    const int lcd_buffer_index =
        (_current_lcd_buffer_index + 1) % CONFIG_BSP_LCD_DPI_BUFFER_NUMS;
    ESP_RETURN_ON_FALSE(_lcd_buffers[lcd_buffer_index] != nullptr, ESP_ERR_INVALID_STATE,
                        ESP_UTILS_LOG_TAG, "LCD frame buffer not ready");

    struct v4l2_buffer v4l2_buf = {};
    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(_video_fd, VIDIOC_DQBUF, &v4l2_buf) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_DQBUF failed");
        return ESP_FAIL;
    }
    if (v4l2_buf.index >= CAMERA_BUFFER_COUNT) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Invalid buffer index: %u", static_cast<unsigned>(v4l2_buf.index));
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t display_w = BSP_LCD_H_RES;
    const uint32_t display_h = BSP_LCD_V_RES;
    const ContainFitConfig fit = computeContainFit(_camera_width, _camera_height, display_w, display_h);
    ESP_RETURN_ON_FALSE(fit.scale > 0.0f, ESP_ERR_INVALID_SIZE, ESP_UTILS_LOG_TAG, "Invalid camera fit");

    ppa_srm_oper_config_t srm_config = {};
    srm_config.in.buffer = _camera_buffers[v4l2_buf.index];
    srm_config.in.pic_w = _camera_width;
    srm_config.in.pic_h = _camera_height;
    srm_config.in.block_w = _camera_width;
    srm_config.in.block_h = _camera_height;
    srm_config.in.block_offset_x = 0;
    srm_config.in.block_offset_y = 0;
    srm_config.in.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.out.buffer = _lcd_buffers[lcd_buffer_index];
    srm_config.out.buffer_size = ALIGN_UP(display_w * display_h * JIANTI_BYTES_PER_PIXEL,
                                          _data_cache_line_size);
    srm_config.out.pic_w = display_w;
    srm_config.out.pic_h = display_h;
    srm_config.out.block_offset_x = fit.output_offset_x;
    srm_config.out.block_offset_y = fit.output_offset_y;
    srm_config.out.srm_cm = JIANTI_PPA_COLOR_MODE;

    srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm_config.scale_x = fit.scale;
    srm_config.scale_y = fit.scale;
    srm_config.mirror_x = 0;
    srm_config.mirror_y = 0;
    srm_config.rgb_swap = 0;
    srm_config.byte_swap = 0;
    srm_config.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_srm_handle, &srm_config);
    if (ret == ESP_OK && _dummy_enabled) {
        ret = esp_lv_adapter_dummy_draw_blit(_display, 0, 0, display_w, display_h,
                                            _lcd_buffers[lcd_buffer_index], true);
        if (ret == ESP_OK) {
            _current_lcd_buffer_index = lcd_buffer_index;
        }
    }

    v4l2_buf.m.userptr = (unsigned long)_camera_buffers[v4l2_buf.index];
    v4l2_buf.length = _camera_buffer_lengths[v4l2_buf.index];
    if (ioctl(_video_fd, VIDIOC_QBUF, &v4l2_buf) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "VIDIOC_QBUF failed");
        return ESP_FAIL;
    }

    return ret;
}

/* 抓一帧真图：DQBUF -> 拷进 _snapshot -> QBUF */
esp_err_t JianTi::grabFrame()
{
    const size_t need = static_cast<size_t>(_camera_width) * _camera_height * JIANTI_BYTES_PER_PIXEL;
    if (!_snapshot || _snapshot_cap < need) {
        if (_snapshot) {
            heap_caps_free(_snapshot);
        }
        _snapshot = static_cast<uint8_t *>(heap_caps_aligned_calloc(_data_cache_line_size, 1, need,
                                                                   MALLOC_CAP_SPIRAM));
        if (!_snapshot) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Snapshot alloc failed: %u bytes", (unsigned)need);
            _snapshot_cap = 0;
            return ESP_ERR_NO_MEM;
        }
        _snapshot_cap = need;
    }

    struct v4l2_buffer v4l2_buf = {};
    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(_video_fd, VIDIOC_DQBUF, &v4l2_buf) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Grab DQBUF failed");
        return ESP_FAIL;
    }
    if (v4l2_buf.index >= CAMERA_BUFFER_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_cache_msync(_camera_buffers[v4l2_buf.index], _camera_buffer_lengths[v4l2_buf.index],
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Snapshot cache sync failed: %s", esp_err_to_name(ret));
    }

    memcpy(_snapshot, _camera_buffers[v4l2_buf.index], need);
    _snapshot_len = need;

    v4l2_buf.m.userptr = (unsigned long)_camera_buffers[v4l2_buf.index];
    v4l2_buf.length = _camera_buffer_lengths[v4l2_buf.index];
    if (ioctl(_video_fd, VIDIOC_QBUF, &v4l2_buf) != 0) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Grab QBUF failed");
        return ESP_FAIL;
    }

    ESP_LOGI(ESP_UTILS_LOG_TAG, "Snapshot: %" PRIu32 "x%" PRIu32 " (%u bytes)",
             _camera_width, _camera_height, (unsigned)_snapshot_len);
    return ESP_OK;
}

/* ---------------- JPEG 编码 ---------------- */

bool JianTi::encodeJpeg(uint8_t **out, size_t *out_len)
{
    if (!_snapshot || _snapshot_len == 0) {
        return false;
    }
    return encodeJpegRgb565(_snapshot, _camera_width, _camera_height, out, out_len);
}

/* 任意 RGB565 帧 → 灰度 JPEG（静态确认页这张图也走这里，不改相机参数） */
bool JianTi::encodeJpegRgb565(const uint8_t *src_buf, uint32_t w, uint32_t h, uint8_t **out, size_t *out_len)
{
    if (!src_buf || w == 0 || h == 0 || !out || !out_len) {
        return false;
    }

    const size_t px_count = static_cast<size_t>(w) * h;

    uint8_t *gray = static_cast<uint8_t *>(heap_caps_malloc(px_count, MALLOC_CAP_SPIRAM));
    if (!gray) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "gray buffer alloc failed");
        return false;
    }

    const uint16_t *src = reinterpret_cast<const uint16_t *>(src_buf);
    for (size_t i = 0; i < px_count; i++) {
        uint16_t px = src[i];
        uint32_t r = (px >> 11) & 0x1F;
        uint32_t g = (px >> 5) & 0x3F;
        uint32_t b = px & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        gray[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
    }

    jpeg_encoder_handle_t enc = nullptr;
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 2000,
    };
    if (jpeg_new_encoder_engine(&eng_cfg, &enc) != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "jpeg engine acquire failed");
        heap_caps_free(gray);
        return false;
    }

    size_t out_cap = px_count;
    size_t alloc_sz = 0;
    uint8_t *buf = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(out_cap, nullptr, &alloc_sz));
    if (!buf) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "jpeg out buffer alloc failed");
        jpeg_del_encoder_engine(enc);
        heap_caps_free(gray);
        return false;
    }

    jpeg_encode_cfg_t cfg = {};
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_GRAY;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_GRAY;
    cfg.width = w;
    cfg.height = h;
    cfg.image_quality = static_cast<uint8_t>(CONFIG_JIANTI_JPEG_QUALITY);

    uint32_t out_bytes = 0;
    esp_err_t err = jpeg_encoder_process(enc, &cfg, gray, px_count, buf, out_cap, &out_bytes);
    jpeg_del_encoder_engine(enc);
    heap_caps_free(gray);

    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "jpeg encode failed: %s", esp_err_to_name(err));
        heap_caps_free(buf);
        return false;
    }

    ESP_LOGI(ESP_UTILS_LOG_TAG, "JPEG: %ux%u q=%d -> %u bytes",
             (unsigned)w, (unsigned)h, CONFIG_JIANTI_JPEG_QUALITY, (unsigned)out_bytes);
    *out = buf;
    *out_len = out_bytes;
    return true;
}

/* ---------------- WiFi + 上传 ---------------- */

/* 幂等：复用既有 STA netif（绝不重建）。netif 基座 / esp_wifi_init / 事件 handler /
 * esp_wifi_start 各只做一次；已联网直接返回。wait_ms>0 时最多等这么久拿 IP。
 * 不依赖系统 Settings/WLAN；不要求用户去点 Settings。 */
bool JianTi::wifiStartSta(uint32_t wait_ms)
{
    if (s_wifi_got_ip) {
        return true;
    }

    if (!s_netif_base_ready) {
        esp_err_t e = esp_netif_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_netif_init failed: %s", esp_err_to_name(e));
            return false;
        }
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "event loop failed: %s", esp_err_to_name(e));
            return false;
        }
        s_netif_base_ready = true;
    }

    /* 复用现有 STA netif：esp_wifi_remote(ESP-Hosted) 场景下 ifkey 查询可能返回 NULL，
     * 再遍历所有 netif 找 STA；确实没有才建一次，避免重复创建 → netif_add assert。 */
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!s_sta_netif) {
            esp_netif_t *it = esp_netif_next_unsafe(nullptr);
            while (it) {
                const char *key = esp_netif_get_ifkey(it);
                if (key && strncmp(key, "WIFI_STA", 8) == 0) {
                    s_sta_netif = it;
                    ESP_LOGI(ESP_UTILS_LOG_TAG, "reuse existing STA netif: ifkey=%s", key);
                    break;
                }
                it = esp_netif_next_unsafe(it);
            }
        }
        if (!s_sta_netif) {
            s_sta_netif = esp_netif_create_default_wifi_sta();
            ESP_LOGI(ESP_UTILS_LOG_TAG, "created default STA netif");
        }
        if (!s_sta_netif) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "no STA netif");
            return false;
        }
    }

    if (!s_wifi_inited) {
        /* WiFi 基本已被系统(Settings)初始化过。先用 get_mode 探测：能取到模式说明
         * 驱动已在工作，就不再 esp_wifi_init（重复 init 无必要，也避免扰动从机）。 */
        wifi_mode_t probe = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&probe) == ESP_OK) {
            ESP_LOGI(ESP_UTILS_LOG_TAG, "wifi already inited (mode=%d), skip esp_wifi_init", (int)probe);
        } else {
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            esp_err_t e = esp_wifi_init(&cfg);
            if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_wifi_init failed: %s", esp_err_to_name(e));
                return false;
            }
        }
        s_wifi_inited = true;
    }

    if (!s_sta_handler_reg) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &jianti_wifi_event, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &jianti_wifi_event, nullptr);
        s_sta_handler_reg = true;
    }

    char ssid[64] = {0};
    char pass[64] = {0};
    bool have_creds = false;

    /* 1. 简题自己的 NVS（namespace "jt"），配网页写的就是这里 */
    if (read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        have_creds = true;
        ESP_LOGI(ESP_UTILS_LOG_TAG, "creds from NVS jt (ssid=%s)", ssid);
    }

    /* 2. 兼容 esp_wifi 自身 NVS 里已存的 STA 配置（历史上配过） */
    if (!have_creds) {
        wifi_config_t cur = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0] != '\0') {
            snprintf(ssid, sizeof(ssid), "%s", reinterpret_cast<const char *>(cur.sta.ssid));
            snprintf(pass, sizeof(pass), "%s", reinterpret_cast<const char *>(cur.sta.password));
            have_creds = true;
            ESP_LOGI(ESP_UTILS_LOG_TAG, "creds from existing STA config (ssid=%s)", ssid);
        }
    }

    /* 3. Kconfig 兜底（默认留空） */
    if (!have_creds && CONFIG_JIANTI_WIFI_SSID[0] != '\0') {
        snprintf(ssid, sizeof(ssid), "%s", CONFIG_JIANTI_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", CONFIG_JIANTI_WIFI_PASS);
        have_creds = true;
        ESP_LOGW(ESP_UTILS_LOG_TAG, "creds from Kconfig fallback (ssid=%s)", ssid);
    }

    if (!have_creds) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "no WiFi creds: provisioning needed");
        return false;
    }

    /* 只在第一次发起连接；之后由事件回调自动重连，不再重复 set_config。
     *
     * ★ 实测结论（ESP32-P4 + ESP-Hosted/esp_wifi_remote）★
     * 开机时从机(C6)的 Wi-Fi 已随 esp_wifi_init 起来，STA netif 也已由默认流程建好。
     * 此时再调 esp_wifi_start()，事件会投递 WIFI_EVENT_STA_START（实测不止一次），
     * 默认 handler(wifi_default_action_sta_start) 每次都做一次 esp_netif_action_start
     * → netif_add，于是 lwIP 报 "netif already added" assert（netif.c:420）→ 崩溃重启。
     * 但若干脆不调 esp_wifi_start()，驱动又没起来，esp_wifi_connect() 只回
     * ESP_ERR_WIFI_NOT_STARTED，永远连不上（实测）。
     *
     * 所以按下面顺序做，既起得来又只 add 一次：
     * ① 把该 STA netif 从"默认"事件链解绑：清掉 esp_wifi 内部的 STA 槽并销毁默认
     *    驱动句柄；此后无论 STA_START 来几次，默认 handler 都找不到 netif → 不再 add。
     * ② 用非"默认注册"的方式把 STA 驱动重新绑回同一个 netif（esp_netif_attach 只
     *    换驱动、不动 lwIP 的 netif 名/实例 → 是复用，不是重建；也不再占用默认槽）。
     * ③ esp_wifi_start() 把 Wi-Fi 起来。
     * ④ 我们自己把 STA netif 只 add/up 一次（等价于默认 handler 那一次 netif_add）。
     * 全程复用既有上网口，不重建 netif、不重启 Wi-Fi。 */
    if (!s_wifi_started) {
        if (!jianti_detach_default_sta(s_sta_netif)) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "detach default STA chain failed");
            return false;
        }

        wifi_mode_t cur_mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&cur_mode) != ESP_OK || cur_mode != WIFI_MODE_STA) {
            esp_err_t e0 = esp_wifi_set_mode(WIFI_MODE_STA);
            if (e0 != ESP_OK) {
                ESP_LOGW(ESP_UTILS_LOG_TAG, "set_mode: %s", esp_err_to_name(e0));
            }
        }

        wifi_config_t wcfg = {};
        snprintf(reinterpret_cast<char *>(wcfg.sta.ssid), sizeof(wcfg.sta.ssid), "%s", ssid);
        snprintf(reinterpret_cast<char *>(wcfg.sta.password), sizeof(wcfg.sta.password), "%s", pass);
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        if (e != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "set_config failed: %s", esp_err_to_name(e));
            return false;
        }

        e = esp_wifi_start();
        if (e != ESP_OK && e != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_wifi_start failed: %s", esp_err_to_name(e));
            return false;
        }

        /* 默认链路被解绑后，IDF 原本在其 START/CONNECTED handler 里做的两件事要自己补：
         * ① 给 netif 设上 Wi-Fi MAC；② 注册 STA 收包回调（否则收不到任何包，
         *    DHCP 永远拿不到 IP）。这两步与 IDF wifi_start() 里的顺序一致。 */
        wifi_netif_driver_t drv = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(s_sta_netif));
        uint8_t mac[6] = {0};
        if (drv && esp_wifi_get_if_mac(drv, mac) == ESP_OK) {
            esp_netif_set_mac(s_sta_netif, mac);
        }
        if (drv && esp_wifi_register_if_rxcb(drv, esp_netif_receive, s_sta_netif) != ESP_OK) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "esp_wifi_register_if_rxcb(STA) failed");
        }

        /* 自己只做一次 netif add（默认链路已解绑，不会有第二次） */
        if (s_sta_netif && !esp_netif_is_netif_up(s_sta_netif)) {
            esp_netif_action_start(s_sta_netif, nullptr, 0, nullptr);
            ESP_LOGI(ESP_UTILS_LOG_TAG, "sta netif add/up done (single, manual)");
        }

        e = esp_wifi_connect();
        if (e != ESP_OK) {
            /* 不崩、不重启：串口明示失败；事件回调会自动重连，后台轮询继续观察 */
            ESP_LOGE(ESP_UTILS_LOG_TAG, "esp_wifi_connect failed: %s (will keep retrying)",
                     esp_err_to_name(e));
        } else {
            ESP_LOGI(ESP_UTILS_LOG_TAG, "STA self-connect requested (ssid=%s)", ssid);
        }
        s_wifi_started = true;
    }

    if (wait_ms == 0) {
        return s_wifi_got_ip;
    }

    uint32_t ticks = wait_ms / 100;
    for (uint32_t i = 0; i < ticks && !s_wifi_got_ip; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_wifi_got_ip) {
        return true;
    }
    ESP_LOGE(ESP_UTILS_LOG_TAG, "wifi not up yet (ssid=%s, associated=%d)", ssid, (int)s_wifi_sta_connected);
    return false;
}

bool JianTi::wifiEnsureConnected()
{
    return wifiStartSta(JIANTI_WIFI_WAIT_MS);
}

void JianTi::wifiBootStart()
{
    ESP_LOGI(ESP_UTILS_LOG_TAG, "boot self-connect: begin (NVS creds -> STA)");
    bool ok = wifiStartSta(JIANTI_WIFI_BOOT_WAIT_MS);
    if (ok) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "boot self-connect: ONLINE");
    } else {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "boot self-connect: not up yet, background retry keeps trying");
    }
}

bool JianTi::uploadJpeg(const uint8_t *jpeg, size_t len, char *text, size_t text_sz)
{
    static char *s_resp = nullptr;
    static size_t s_resp_len = 0;
    if (!s_resp) {
        s_resp = static_cast<char *>(heap_caps_malloc(JIANTI_RESP_CAP, MALLOC_CAP_SPIRAM));
        if (!s_resp) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "resp buffer alloc failed");
            return false;
        }
    }
    s_resp_len = 0;
    s_resp[0] = '\0';

    struct RespCtx {
        char *buf;
        size_t cap;
        size_t *len;
    } ctx = {s_resp, JIANTI_RESP_CAP, &s_resp_len};

    auto handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
            auto *c = static_cast<RespCtx *>(evt->user_data);
            if (!c || !c->buf) {
                return ESP_OK;
            }
            size_t room = c->cap - 1 - *c->len;
            size_t n = (static_cast<size_t>(evt->data_len) < room) ? static_cast<size_t>(evt->data_len) : room;
            memcpy(c->buf + *c->len, evt->data, n);
            *c->len += n;
            c->buf[*c->len] = '\0';
        }
        return ESP_OK;
    };

    esp_http_client_config_t cfg = {};
    cfg.url = gatewayUrl();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = JIANTI_HTTP_TIMEOUT_MS;
    cfg.event_handler = handler;
    cfg.user_data = &ctx;

    ESP_LOGI(ESP_UTILS_LOG_TAG, "upload: dev=%s url=%s %u bytes", _device_id, cfg.url, (unsigned)len);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "http client init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-JT-Device", _device_id);
    esp_http_client_set_post_field(client, reinterpret_cast<const char *>(jpeg), static_cast<int>(len));

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "upload: err=%s status=%d %lld ms resp=%u bytes",
             esp_err_to_name(err), status, ms, (unsigned)s_resp_len);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "upload resp: %.400s", s_resp);
    /* 回包全文分段落串口日志（留档对照 8025 卡文字，逐字核对用） */
    for (size_t off = 0; off < s_resp_len; off += 1000) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "resp[%u]: %.1000s", (unsigned)off, s_resp + off);
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "upload failed: %.200s", s_resp);
        return false;
    }
    if (!json_get_string(s_resp, "text", text, text_sz)) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "bad json: %.200s", s_resp);
        return false;
    }
    /* 8025 /solve 的 cards[]：解析成按题渲染用的卡（文字逐字原文） */
    this->parseCards(s_resp);
    return true;
}

/* ---------------- 服务器测试 + 配网 ---------------- */

bool JianTi::serverHealthCheck(const char *url)
{
    if (!url || url[0] == '\0') {
        return false;
    }
    /* 从 /solve 推 /health */
    char health[128];
    const char *p = strstr(url, "/solve");
    if (p) {
        size_t n = p - url;
        snprintf(health, sizeof(health), "%.*s/health", (int)n, url);
    } else {
        snprintf(health, sizeof(health), "%s/health", url);
    }

    esp_http_client_config_t cfg = {};
    cfg.url = health;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(ESP_UTILS_LOG_TAG, "health %s: err=%s status=%d", health, esp_err_to_name(err), status);
    return (err == ESP_OK && status == 200);
}

void JianTi::doServerTest(void)
{
    if (!_label_test_result) {
        return;
    }
    bsp_extra_display_lock(-1);
    lv_label_set_text(_label_test_result, "测试中…");
    bsp_extra_display_unlock();

    /* 分层报：WiFi / health / 当前服务器 */
    char line1[64] = "WiFi: 未关联";
    char line2[64] = "当前: 内网服务器 不通 / 公网服务器 不通";

    if (s_wifi_got_ip) {
        snprintf(line1, sizeof(line1), "WiFi: 已关联(有IP)");
    } else {
        /* 尝试连一次 */
        if (wifiEnsureConnected()) {
            snprintf(line1, sizeof(line1), "WiFi: 已关联(有IP)");
        } else {
            snprintf(line1, sizeof(line1), "WiFi: 未关联(无IP)");
        }
    }

    /* health 检查：两个都测 */
    bool lan_ok = serverHealthCheck(CONFIG_JIANTI_GATEWAY_URL);
    bool cloud_ok = serverHealthCheck(JIANTI_GW_CLOUD);

    /* v8：结果标签压到 2 行（24px 三行会掉出 480 屏底），服务器名统一新叫法 */
    if (_server_id == 0) {
        snprintf(line2, sizeof(line2), "当前: 内网服务器 %s / 公网服务器 %s",
                 lan_ok ? "通" : "不通", cloud_ok ? "通" : "不通");
    } else {
        snprintf(line2, sizeof(line2), "当前: 公网服务器 %s / 内网服务器 %s",
                 cloud_ok ? "通" : "不通", lan_ok ? "通" : "不通");
    }

    char result[192];
    snprintf(result, sizeof(result), "%s\n%s", line1, line2);
    bsp_extra_display_lock(-1);
    lv_label_set_text(_label_test_result, result);
    bsp_extra_display_unlock();
}

void JianTi::doWifiReconfig(void)
{
    if (!_label_test_result) {
        return;
    }
    bsp_extra_display_lock(-1);
    lv_label_set_text(_label_test_result, "配网中：重启后开热点\nJianTi-XXXX，手机浏览器\n192.168.4.1 填家里WiFi");
    bsp_extra_display_unlock();

    /* 清简题 NVS 里的 WiFi 凭据 */
    nvs_handle_t nh;
    if (nvs_open(JIANTI_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_erase_key(nh, JIANTI_NVS_KEY_SSID);
        nvs_erase_key(nh, JIANTI_NVS_KEY_PASS);
        nvs_commit(nh);
        nvs_close(nh);
        ESP_LOGI(ESP_UTILS_LOG_TAG, "cleared jt NVS wifi creds");
    }

    /* 清 Settings 的 STA 配置（net80211 命名空间由 esp_wifi 自己管，这里只断连） */
    esp_wifi_disconnect();

    /* 延迟 2s 让用户看到提示，然后重启进 SoftAP 配网 */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

} // namespace esp_brookesia::apps
