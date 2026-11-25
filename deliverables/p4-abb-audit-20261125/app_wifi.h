/**
 ******************************************************************************
 * @file        app_wifi.h
 * @version     V1.0
 * @brief       Wifi APP
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#ifndef _APP_WIFI_H
#define _APP_WIFI_H

#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "app_main_ui.h"
#include "lvgl_demo.h"
#include "lcd.h"

/* 结构体声明 */
typedef struct {
    lv_obj_t *wifi_main_ui;
    lv_obj_t *list;
    lv_obj_t *scan_btn;
    lv_obj_t *back_btn;
    lv_obj_t *status_bar;
    lv_obj_t *status_label;
    lv_obj_t *status_icon;
    uint8_t initialized;
} wifi_ui_t;

extern wifi_ui_t wifi_ui;

/* 函数声明 */
void wifi_app_init(void);
void wifi_start_sta(void);
void wifi_module_init(void);
void wifi_app_del(void);
const char *wifi_get_ip_text(void);

#endif
