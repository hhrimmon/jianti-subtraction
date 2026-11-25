/**
 ******************************************************************************
 * @file        mipi_cam.c
 * @version     V1.0
 * @brief       mipicamera驱动代码(含拍照/照片浏览/自动保存SD卡)
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#include "mipi_cam.h"
#include "mipi_lcd.h"
#include "key.h"
#include "piclib.h"
#include <stdio.h>
#include <sys/stat.h>
#include "esp_spiffs.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "jt_client.h"
#include "jt_business.h"

static const char *mipi_cam_tag = "mipi_cam";

#define FPS_FUNC_ON   1
#define ALIGN_UP_BY(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
static TaskHandle_t lcd_cam_task_handle = NULL;
bool camera_running;
volatile bool camera_stream_ready;
static uint8_t camera_init_flag = 0;
static char photo_status_msg[64] = {0};   /* 拍照状态提示 */
static int photo_msg_timer = 0;           /* 提示持续帧数 */
static bool photo_request = false;        /* BOOT 键/启动验收拍照请求 */
static bool storage_mounted = false;
static volatile bool jt_solve_busy = false;
static volatile bool preview_frame_ready = false;
static void *preview_buffers[2] = {NULL, NULL};
static void *preview_allocs[2] = {NULL, NULL};
static uint8_t preview_write_index = 0;
static portMUX_TYPE preview_mux = portMUX_INITIALIZER_UNLOCKED;
static void (*frame_callback)(void *buffer, uint32_t width, uint32_t height, void *ctx) = NULL;
static void *frame_callback_ctx = NULL;

static void jt_solve_done_cb(const jt_solve_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result) {
        return;
    }
    ESP_LOGI(mipi_cam_tag, "solve done: result=%d questions=%d",
             (int)result->result, result->question_count);
    jt_business_on_solve_result(result);
    if (result->result == JT_RESULT_OK && result->question_count > 0) {
        const jt_question_t *q = &result->questions[0];
        snprintf(photo_status_msg, sizeof(photo_status_msg), "ANS:%.50s",
                 q->answer[0] ? q->answer : "OK");
    } else {
        snprintf(photo_status_msg, sizeof(photo_status_msg), "SOLVE:%s",
                 jt_result_to_string(result->result));
    }
    photo_msg_timer = 120;
    jt_solve_busy = false;
}

static void jt_submit_photo(const uint8_t *jpeg, uint32_t jpeg_size)
{
    ESP_LOGI(mipi_cam_tag, "submit photo: jpeg_size=%u", (unsigned)jpeg_size);
    if (jpeg_size > 8 * 1024 * 1024) {
        snprintf(photo_status_msg, sizeof(photo_status_msg), "JPG Too Big");
        photo_msg_timer = 60;
        return;
    }

    jt_solve_busy = true;
    snprintf(photo_status_msg, sizeof(photo_status_msg), "SOLVING...");
    photo_msg_timer = 120;
    ESP_LOGI(mipi_cam_tag, "solve async start");
    if (!jt_client_solve_async(jpeg, jpeg_size, jt_solve_done_cb, NULL)) {
        jt_solve_busy = false;
        snprintf(photo_status_msg, sizeof(photo_status_msg), "Task Fail");
        photo_msg_timer = 60;
        ESP_LOGE(mipi_cam_tag, "solve async failed to start");
    }
}

static void lcd_cam_task(void *arg);

#define PHOTO_DIR        "/storage/PICTURE"
#define PHOTO_BASE       PHOTO_DIR "/PHOTO%03d.jpg"
#define JPEG_QUALITY     80

static esp_err_t photo_storage_init(void)
{
    if (storage_mounted) {
        return ESP_OK;
    }
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(mipi_cam_tag, "spiffs mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    storage_mounted = true;
    mkdir(PHOTO_DIR, 0777);

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(mipi_cam_tag, "storage total=%u used=%u bytes",
                 (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

int app_video_close(int fd) {
    return close(fd);
}

/**
 * @brief       文件名生成(非覆盖)
 * @param       pname : 输出路径缓冲区
 * @param       mode  : 保留参数，当前固定生成 JPEG
 * @retval      无
 */
void camera_new_pathname(uint8_t *pname, uint8_t mode)
{
    uint16_t index = 0;

    while (index < 0xFFFF)
    {
        sprintf((char *)pname, PHOTO_BASE, index);
        struct stat st;
        if (stat((char *)pname, &st) != 0) break;
        index++;
    }
}

/**
 * @brief       图像镜像翻转(垂直翻转,BMP自底向上存储)
 */
void mirror_image(uint16_t *buffer, uint16_t width, uint16_t height)
{
    uint16_t temp;
    uint32_t i, j;

    for (i = 0; i < height / 2; i++) {
        for (j = 0; j < width; j++) {
            temp = buffer[i * width + j];
            buffer[i * width + j] = buffer[(height - 1 - i) * width + j];
            buffer[(height - 1 - i) * width + j] = temp;
        }
    }
}

static void save_jpeg_photo(uint16_t *src, uint16_t width, uint16_t height)
{
    static jpeg_encoder_handle_t encoder = NULL;
    static uint8_t *raw_buf = NULL;
    static size_t raw_buf_size = 0;
    static uint8_t *jpg_buf = NULL;
    static size_t jpg_buf_size = 0;
    uint8_t path[64];
    uint32_t jpg_size = 0;

    if (photo_storage_init() != ESP_OK) {
        snprintf(photo_status_msg, sizeof(photo_status_msg), "Storage Fail");
        photo_msg_timer = 60;
        return;
    }

    size_t raw_size = (size_t)width * height * sizeof(uint16_t);
    if (!encoder) {
        jpeg_encode_engine_cfg_t enc_eng_cfg = {
            .timeout_ms = 1000,
        };
        if (jpeg_new_encoder_engine(&enc_eng_cfg, &encoder) != ESP_OK || !encoder) {
            encoder = NULL;
            snprintf(photo_status_msg, sizeof(photo_status_msg), "JPEG Init Fail");
            photo_msg_timer = 60;
            return;
        }
        jpeg_encode_memory_alloc_cfg_t tx_mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
        };
        jpeg_encode_memory_alloc_cfg_t rx_mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        raw_buf = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &tx_mem_cfg, &raw_buf_size);
        jpg_buf = (uint8_t *)jpeg_alloc_encoder_mem(raw_size / 3, &rx_mem_cfg, &jpg_buf_size);
        if (!raw_buf || !jpg_buf) {
            if (raw_buf) { free(raw_buf); raw_buf = NULL; }
            if (jpg_buf) { free(jpg_buf); jpg_buf = NULL; }
            jpeg_del_encoder_engine(encoder);
            encoder = NULL;
            snprintf(photo_status_msg, sizeof(photo_status_msg), "JPEG No Mem");
            photo_msg_timer = 60;
            return;
        }
    }

    memcpy(raw_buf, src, raw_size);
    jpeg_encode_cfg_t enc_cfg = {
        .height = height,
        .width = width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
        .pixel_reverse = false,
    };
    esp_err_t ret = jpeg_encoder_process(encoder, &enc_cfg, raw_buf, raw_size,
                                         jpg_buf, jpg_buf_size, &jpg_size);
    if (ret != ESP_OK || jpg_size == 0) {
        ESP_LOGE(mipi_cam_tag, "jpeg encode failed: %s size=%u",
                 esp_err_to_name(ret), (unsigned)jpg_size);
        snprintf(photo_status_msg, sizeof(photo_status_msg), "JPEG Fail");
        photo_msg_timer = 60;
        return;
    }

    camera_new_pathname(path, 0);
    FILE *fp = fopen((char *)path, "wb");
    if (!fp) {
        ESP_LOGE(mipi_cam_tag, "fopen failed: %s", path);
        snprintf(photo_status_msg, sizeof(photo_status_msg), "Save Failed!");
        photo_msg_timer = 60;
        return;
    }
    size_t written = fwrite(jpg_buf, 1, jpg_size, fp);
    fclose(fp);
    if (written == jpg_size) {
        ESP_LOGI(mipi_cam_tag, "PHOTO SAVED %s size=%u", path, (unsigned)jpg_size);
        snprintf(photo_status_msg, sizeof(photo_status_msg), "JPG OK %uK",
                 (unsigned)(jpg_size / 1024));
        jt_submit_photo(jpg_buf, jpg_size);
    } else {
        snprintf(photo_status_msg, sizeof(photo_status_msg), "Write Failed!");
    }
    photo_msg_timer = 60;
}

/**
 * @brief       mipi_cam初始化
 */
esp_err_t mipi_cam_init(void)
{
    esp_err_t ret = ESP_OK;
    if (camera_running && lcd_cam_task_handle != NULL &&
        eTaskGetState(lcd_cam_task_handle) != eDeleted) {
        ESP_LOGI(mipi_cam_tag, "camera already running");
        return ESP_OK;
    }
    mipi_dev_bsp_enable_dsi_phy_power();

    ret = photo_storage_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!preview_buffers[0]) {
        size_t preview_size = (size_t)1024 * 600 * sizeof(uint16_t);
        size_t cache_line = 0;
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line);
        if (cache_line == 0) {
            cache_line = 64;
        }
        size_t alloc_size = preview_size + cache_line - 1;
        preview_allocs[0] = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        preview_allocs[1] = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!preview_allocs[0] || !preview_allocs[1]) {
            ESP_LOGE(mipi_cam_tag, "preview buffer alloc failed");
            if (preview_allocs[0]) heap_caps_free(preview_allocs[0]);
            if (preview_allocs[1]) heap_caps_free(preview_allocs[1]);
            preview_allocs[0] = NULL;
            preview_allocs[1] = NULL;
            return ESP_ERR_NO_MEM;
        }
        preview_buffers[0] = (void *)(((uintptr_t)preview_allocs[0] + cache_line - 1) & ~(uintptr_t)(cache_line - 1));
        preview_buffers[1] = (void *)(((uintptr_t)preview_allocs[1] + cache_line - 1) & ~(uintptr_t)(cache_line - 1));
        ESP_LOGI(mipi_cam_tag, "preview buffers aligned: line=%u size=%u",
                 (unsigned)cache_line, (unsigned)preview_size);
    }

    if(camera_init_flag == 0)
    {
        camera_init_flag = 1;
        ret = app_video_main(i2c_bus_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(mipi_cam_tag, "video main init failed with error 0x%x", ret);
            return ESP_FAIL;
        }
    }

    int video_cam_fd0 = app_video_open(0);
    if (video_cam_fd0 < 0)
    {
        ESP_LOGE(mipi_cam_tag, "video cam open failed");
        return ESP_FAIL;
    }

    ret = app_video_init(video_cam_fd0, APP_VIDEO_FMT_RGB565);
    if (ret != ESP_OK)
    {
        ESP_LOGE(mipi_cam_tag, "Video cam init failed with error 0x%x", ret);
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(200)); /* 等待V4L2 CSI/ISP硬件就绪 */

    xTaskCreatePinnedToCore(lcd_cam_task, "lcd cam display", 4096, &video_cam_fd0, 4, &lcd_cam_task_handle, 0);
    camera_stream_ready = false;
    camera_running = true;
    return ESP_OK;
}

/**
 * @brief       摄像头显示任务
 */
static void lcd_cam_task(void *arg)
{
    int video_fd = *((int *)arg);
    int res = 0;
    struct v4l2_buffer buf;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format format = {0};
    format.type = type;

    void *draw_buffer = NULL;
    size_t data_cache_line_size = 0;
    uint8_t *pname = NULL;

    esp_err_t err;
    draw_buffer = preview_buffers[0];
    err = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (err != ESP_OK) { ESP_LOGE(mipi_cam_tag, "cache align failed"); goto cleanup_task; }

    pname = piclib_mem_malloc(64);  /* "/storage/PICTURE/PHOTOxxx.jpg" */

    void *camera_outbuf[2];
    /* esp-video 在 S_FMT 后需要短暂完成 CSI/ISP 缓冲重配，REQBUFS 偶发失败时重建 V4L2 会话 */
    for (int attempt = 0; attempt < 3; attempt++) {
        err = camera_set_bufs(video_fd, 2, NULL);
        if (err == 0) {
            break;
        }
        ESP_LOGW(mipi_cam_tag, "req bufs failed (err=0x%x), retry %d/3", err, attempt + 1);
        app_video_close(video_fd);
        video_fd = -1;
        vTaskDelay(pdMS_TO_TICKS(300));
        video_fd = app_video_open(0);
        if (video_fd < 0) { ESP_LOGE(mipi_cam_tag, "reopen failed"); goto cleanup_task; }
        err = app_video_init(video_fd, APP_VIDEO_FMT_RGB565);
        if (err != ESP_OK) { ESP_LOGE(mipi_cam_tag, "reinit failed (err=0x%x)", err); goto cleanup_task; }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err != 0) {
        ESP_LOGE(mipi_cam_tag, "req bufs retry exhausted (err=0x%x)", err);
        goto cleanup_task;
    }
    err = camera_get_bufs(2, &camera_outbuf[0]);
    if (err != 0) { ESP_LOGE(mipi_cam_tag, "get bufs failed"); goto cleanup_task; }

    ppa_client_handle_t ppa_srm_handle = NULL;
    ppa_client_config_t ppa_srm_config = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    err = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (err != ESP_OK) { ESP_LOGE(mipi_cam_tag, "ppa register failed"); goto cleanup_task; }

    err = camera_stream_start(video_fd);
    if (err != 0) { ESP_LOGE(mipi_cam_tag, "stream start failed"); ppa_unregister_client(ppa_srm_handle); goto cleanup_task; }
    camera_stream_ready = true;

#if FPS_FUNC_ON == 1
    int fps_count = 0;
    int64_t start_time = esp_timer_get_time();
#endif

    if (app_video_v4l2_ioctl(video_fd, VIDIOC_G_FMT, &format) != 0)
    {
        ESP_LOGE(mipi_cam_tag, "get fmt failed");
    }

    /* 首帧稳定后抓一张，作为 OV5647 取图上屏的验收照片 */
    vTaskDelay(pdMS_TO_TICKS(500));
    photo_request = true;

    while (camera_running)
    {
#if FPS_FUNC_ON == 1
        fps_count++;
        if (fps_count == 50) {
            int64_t end_time = esp_timer_get_time();
            ESP_LOGI(mipi_cam_tag, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
            start_time = end_time;
            fps_count = 0;
        }
#endif
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        res = app_video_v4l2_ioctl(video_fd, VIDIOC_DQBUF, &buf);
        if (res != 0) {
            ESP_LOGE(mipi_cam_tag, "failed to receive video frame");
        }

        /* PPA处理 - 7寸 1024x600 EK79007 */
        {
            ppa_srm_oper_config_t oper_config = {
                .in.buffer          = camera_outbuf[buf.index],
                .in.pic_w           = format.fmt.pix.width, 
                .in.pic_h           = format.fmt.pix.height,
                .in.block_w         = 1024, 
                .in.block_h         = 600,
                .in.block_offset_x  = 0, 
                .in.block_offset_y  = 0,
                .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,

                .out.buffer         = draw_buffer,
                .out.buffer_size    = ALIGN_UP_BY((size_t)lcddev.height * lcddev.width * 16 / 8, data_cache_line_size),
                .out.pic_w          = lcddev.width, 
                .out.pic_h          = lcddev.height,
                .out.block_offset_x = 0, 
                .out.block_offset_y = 0,
                .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
                .rotation_angle     = PPA_SRM_ROTATION_ANGLE_0,
                .scale_x            = 1.0f, 
                .scale_y            = 1.0f,
                .mirror_x           = true, /* 前置摄像头：水平镜像，修正左右反向 */
                .mode               = PPA_TRANS_MODE_BLOCKING,
            };
            ppa_do_scale_rotate_mirror(ppa_srm_handle, &oper_config);
            portENTER_CRITICAL(&preview_mux);
            preview_write_index = preview_write_index ? 0 : 1;
            preview_buffers[preview_write_index] = draw_buffer;
            draw_buffer = preview_buffers[preview_write_index ? 0 : 1];
            portEXIT_CRITICAL(&preview_mux);
            preview_frame_ready = true;
            if (frame_callback) {
                frame_callback(draw_buffer, lcddev.width, lcddev.height, frame_callback_ctx);
            }
        }

        /* === 按键处理: BOOT 拍照 === */
        if (key_scan(0) == BOOT_PRES) {
            photo_request = true;
        }

        if (photo_request && !jt_solve_busy) {
            uint16_t pic_width  = lcddev.width;
            uint16_t pic_height = lcddev.height;
            uint16_t *photo_buffer = NULL;

            if (!camera_stream_ready || !preview_frame_ready ||
                !preview_buffers[0] || !preview_buffers[1]) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            photo_request = false;
            photo_buffer = (uint16_t *)piclib_mem_malloc(pic_width * pic_height * sizeof(uint16_t));
            if (photo_buffer != NULL) {
                void *latest_frame = NULL;
                portENTER_CRITICAL(&preview_mux);
                latest_frame = preview_buffers[preview_write_index];
                portEXIT_CRITICAL(&preview_mux);
                memcpy(photo_buffer, latest_frame, pic_width * pic_height * sizeof(uint16_t));
                mirror_image(photo_buffer, pic_width, pic_height);
                save_jpeg_photo(photo_buffer, pic_width, pic_height);
                piclib_mem_free(photo_buffer);
            } else {
                ESP_LOGE(mipi_cam_tag, "Failed to allocate photo buffer");
            }
        }

        /* 绘制拍照状态提示 */
        if (photo_msg_timer > 0) {
            uint16_t msg_color = (photo_msg_timer > 30) ? GREEN : YELLOW;
            lcd_show_string(10, 10, lcddev.width - 20, 24, 24, photo_status_msg, msg_color);
            photo_msg_timer--;
        }

        if (app_video_v4l2_ioctl(video_fd, VIDIOC_QBUF, &buf) != 0)
        {
            ESP_LOGE(mipi_cam_tag, "failed to free video frame");
        }
    }

    camera_stream_stop(video_fd);
    if (ppa_srm_handle) ppa_unregister_client(ppa_srm_handle);
cleanup_task:
    app_video_close(video_fd);
    if (pname) piclib_mem_free(pname);

    ESP_LOGI(mipi_cam_tag, "Camera task stopped");
    vTaskDelete(NULL);
}

/**
 * @brief 停止摄像头任务
 */
void mipi_cam_stop(void)
{
    camera_running = false;

    /* 等待任务自行退出(清理PPA/V4L2/camera资源),最多等2秒 */
    int wait = 0;
    while (lcd_cam_task_handle != NULL && eTaskGetState(lcd_cam_task_handle) != eDeleted && wait < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait++;
    }

    if (lcd_cam_task_handle != NULL && eTaskGetState(lcd_cam_task_handle) != eDeleted) {
        vTaskDelete(lcd_cam_task_handle);
    }
    lcd_cam_task_handle = NULL;
    /* camera_init_flag 保持为1, app_video_main() 只注册一次,重复注册会失败 */
}

void mipi_cam_request_photo(void)
{
    if (!camera_stream_ready || !preview_frame_ready ||
        !preview_buffers[0] || !preview_buffers[1]) {
        ESP_LOGW(mipi_cam_tag,
                 "photo request ignored: stream_ready=%d frame_ready=%d preview0=%p preview1=%p",
                 (int)camera_stream_ready, (int)preview_frame_ready,
                 preview_buffers[0], preview_buffers[1]);
        return;
    }

    photo_request = true;
}

bool mipi_cam_get_preview(void **buffer, uint32_t width, uint32_t height)
{
    if (!buffer || width != 1024 || height != 600) {
        return false;
    }
    portENTER_CRITICAL(&preview_mux);
    *buffer = preview_buffers[preview_write_index];
    portEXIT_CRITICAL(&preview_mux);
    return *buffer != NULL;
}

void mipi_cam_release_preview(void)
{
}

void mipi_cam_set_frame_callback(void (*cb)(void *buffer, uint32_t width,
                                             uint32_t height, void *ctx),
                                 void *ctx)
{
    frame_callback = cb;
    frame_callback_ctx = ctx;
}
