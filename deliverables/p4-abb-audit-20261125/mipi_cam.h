/**
 ******************************************************************************
 * @file        mipi_cam.h
 * @version     V1.0
 * @brief       mipicamera驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#ifndef __MIPI_CAM_H
#define __MIPI_CAM_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_video.h"
#include "myiic.h"
#include "driver/ppa.h"
#include "esp_timer.h"
#include "lcd.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "myes8311.h"

extern bool camera_running;
extern volatile bool camera_stream_ready;

/* 函数声明 */
esp_err_t mipi_cam_init(void);          /* mipi_cam初始化 */
void mipi_cam_stop(void);
void mipi_cam_request_photo(void);
bool mipi_cam_get_preview(void **buffer, uint32_t width, uint32_t height);
void mipi_cam_release_preview(void);
void mipi_cam_set_frame_callback(void (*cb)(void *buffer, uint32_t width,
                                             uint32_t height, void *ctx),
                                 void *ctx);

#endif
