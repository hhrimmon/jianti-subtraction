/**
 ******************************************************************************
 * @file        main.c
 * @version     V1.0
 * @brief       LVGL综合实验
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#include "led.h"
#include "lcd.h"
#include "myiic.h"
#include "ledc.h"
#include "myes8311.h"
#include "esp_rtc.h"
#include "lvgl_demo.h"
#include "sdmmc.h"
#include <stdio.h>
#include "key.h"
#include "spilcd.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "splash_image.h"
#include "app_jt_ui.h"
#include "esp_lvgl_port.h"
#include "driver/uart_vfs.h"
#include "driver/uart.h"
#include <string.h>

const char* main_twai_tag = "main_twai";

/** 
 * @brief       LED闪烁任务
 * @param       无
 * @retval      无
*/

static void led_toggel_task(void *pvParameters)
{
    while(1)
    {
        LED1_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/** Minimal debug CLI for deterministic board-page capture. */
static int jt_page_cmd(int argc, char **argv)
{
    int page;
    char *end = NULL;
    if (argc != 2) {
        printf("usage: page <1..8>\n");
        return 1;
    }
    page = (int)strtol(argv[1], &end, 10);
    if (!end || *end != '\0' || page < 1 || page > 8) {
        printf("bad page: %s\n", argv[1]);
        return 1;
    }
    if (lvgl_port_lock(0)) {
        app_jt_ui_debug_show_page(page - 1);
        lvgl_port_unlock();
    }
    printf("page=%d\n", page);
    return 0;
}

static void jt_disable_uart_nonblocking(void)
{
    /* The console driver may leave VFS in non-blocking mode. The page task
     * reads the UART directly, but stdout still shares this port. */
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_CRLF);
}

static void jt_uart_page_task(void *arg)
{
    static const char *commands[] = {
        "page 1", "page 2", "page 3", "page 4",
        "page 5", "page 6", "page 7", "page 8",
    };
    static const char *const pages[] = {
        "1", "2", "3", "4", "5", "6", "7", "8",
    };
    char line[64];
    size_t len = 0;
    (void)arg;
    while (1) {
        uint8_t ch;
        int got = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1,
                                  pdMS_TO_TICKS(20));
        if (got <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
        if (!strcmp(line, "softap on")) {
            app_jt_ui_debug_softap(true);
            printf("softap=1\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "softap off")) {
            app_jt_ui_debug_softap(false);
            printf("softap=0\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "chain")) {
            app_jt_ui_debug_chain_test();
            printf("chain=done\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "ap state")) {
            app_jt_ui_debug_ap_state();
            printf("ap-state=done\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "freeze 1")) {
            app_jt_ui_debug_freeze_process(true);
            printf("freeze=1\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "freeze 0")) {
            app_jt_ui_debug_freeze_process(false);
            printf("freeze=0\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "shutter")) {
            app_jt_ui_debug_finder_shutter();
            printf("shutter=done\n");
            len = 0;
            continue;
        }
        if (!strcmp(line, "confirm")) {
            app_jt_ui_debug_finder_confirm();
            printf("confirm=done\n");
            len = 0;
            continue;
        }
            for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                if (!strcmp(line, commands[i])) {
                    char *argv[] = { "page", (char *)pages[i] };
                    jt_page_cmd(2, argv);
                    break;
                }
            }
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
    }
}

static void jt_debug_console_init(void)
{
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW("JT_CONSOLE", "uart console init failed");
        return;
    }
    jt_disable_uart_nonblocking();
    xTaskCreate(jt_uart_page_task, "jt_uart_page", 16384, NULL, 5, NULL);
}

/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    uint8_t x = 0;
	uint8_t key = 10;
	uint8_t lcd_id[12];                     /* 存放LCD ID字符串 */
	uint8_t *tx_buf = malloc(8);
    uint8_t *rx_buf = malloc(8);

	usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
										.rx_buffer_size = 1024,
										.tx_buffer_size = 1024,
									};

    jt_debug_console_init();

    led_init();     /* LED初始化 */
	xTaskCreate(led_toggel_task, "led_toggel_task", 2048, NULL, 5, NULL); /* 创建LED闪烁任务 */
    key_init();     /* 按键初始化 */

   

    key = key_scan(0);

	if(key != KEY0_PRES && key != BOOT_PRES)        /* MIPI屏幕*/
	{
		/*********************************** USB JTAG Test ************************************/
		ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));

		if (usb_serial_jtag_is_connected())
		{
			lv_general.usb_jtag_check_en = 1;
		}
		else
		{
			lv_general.usb_jtag_check_en = 0;
		}

		/************************************** SD Check ***************************************/
		esp_err_t sd_result = sdmmc_init();         /* 初始化SD卡并保持挂载 */
		if (sd_result != ESP_OK)
		{
			lv_general.sd_check_en = SD_DISCONNECT;
		}
		else if (sd_result == ESP_OK)
		{
			lv_general.sd_check_en = SD_CONNET;
		}

        /************************************** Twai Test ***************************************/
		for (uint8_t i = 0; i < 8; i++)
		{
			tx_buf[i] = i;
		}

		twai_init(TWAI_MODE_NO_ACK);                           /* TWAI初始化 */
		ESP_ERROR_CHECK(twai_send_data(MSG_ID, tx_buf, 8));    /* ID = 0x12, 发送8个字节 */
        vTaskDelay(pdMS_TO_TICKS(20));
		twai_receive_data(MSG_ID, rx_buf);                     /* CAN ID = 0x12, 接收数据查询 */

        if (memcmp(tx_buf, rx_buf, 8) == 0)
        {
			for (uint8_t i = 0; i < 3; i++)
			{
				ESP_LOGI(main_twai_tag, "twai ok");
			}
        }
		else
		{
			for (uint8_t i = 0; i < 3; i++)
			{
				ESP_LOGI(main_twai_tag, "twai error");
			}
		}

		free(tx_buf);
        free(rx_buf);

		lcd_init();                                                     /* MIPI LCD初始化 */
		lcd_show_splash(splash_image_data);                             /* 显示开机启动图 */
		lcd_backlight_on();                                             /* 打开背光 */
		vTaskDelay(pdMS_TO_TICKS(2000));                                /* 启动图持续显示2秒 */

		if (myi2s_init() != ESP_OK)                                      /* i2s初始化 */
		{
			lcd_show_string(30, 110, 200, 16, 16, "I2S Error", RED);
			while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
		}
		
		while (myes8311_init())                                          /* ES8311初始化 */
		{
			lcd_show_string(30, 110, 200, 16, 16, "ES8311 Error", RED);
			vTaskDelay(pdMS_TO_TICKS(200));
			lcd_fill(30, 110, 239, 126, WHITE);
			vTaskDelay(pdMS_TO_TICKS(200));
		}

		gpio_set_level(GPIO_NUM_11, 1);   /* 打开喇叭 */

		if (mipidev.id == 0x79007)        /* 7寸 1024*600 MIPI屏幕 */
		{
			rtc_set_time(2026, 1, 1, 8, 8, 00);   /* 设置RTC时间 */
			ledc_config_t *ledc_config   = malloc(sizeof(ledc_config_t));
			ledc_config->clk_cfg         = LEDC_USE_PLL_DIV_CLK;    /* 启动定时器时，根据给出的分辨率和占空率参数自动选择ledc源时钟 */
			ledc_config->timer_num       = LEDC_TIMER_0;            /* 选择哪个定时器计数（LEDC_TIMER_0~LEDC_TIMER_3） */
			ledc_config->freq_hz         = 20000;                    /* 1KHz（系统自动计算分配系数，并提供freq_hz频率给到定时器） */
			ledc_config->duty_resolution = LEDC_TIMER_10_BIT;       /* 设置定时器最大计数值（请看技术手册表32.4.1） */
			ledc_config->channel         = LEDC_CHANNEL_0;          /* 设置输出通道（LEDC_CHANNEL_0 ~ LEDC_CHANNEL_7） */
			ledc_config->duty            = 100;                     /* 一个周期内占高电平时间(占空比) */
			ledc_config->gpio_num        = GPIO_NUM_23;             /* PWM信号输出那个管脚 */
			ledc_init(ledc_config);

			lvgl_demo();
		}
	}
	else if(key == KEY0_PRES)                                   /* SPI接口屏幕 */
	{
		spilcd_init();                                          /* SPILCD初始化 */
	    sprintf((char *)lcd_id, "LCD ID:%04X", spilcddev.id);   /* 将LCD ID打印到lcd_id数组 */
		
		while(1)
		{
			switch (x)
			{
				case 0:
				{
					spilcd_clear(WHITE);
					break;
				}
				case 1:
				{
					spilcd_clear(BLACK);
					break;
				}
				case 2:
				{
					spilcd_clear(BLUE);
					break;
				}
				case 3:
				{
					spilcd_clear(RED);
					break;
				}
				case 4:
				{
					spilcd_clear(MAGENTA);
					break;
				}
				case 5:
				{
					spilcd_clear(GREEN);
					break;
				}
				case 6:
				{
					spilcd_clear(CYAN);
					break;
				}
				case 7:
				{
					spilcd_clear(YELLOW);
					break;
				}
				case 8:
				{
					spilcd_clear(BRRED);
					break;
				}
				case 9:
				{
					spilcd_clear(GRAY);
					break;
				}
				case 10:
				{
					spilcd_clear(LGRAY);
					break;
				}
				case 11:
				{
					spilcd_clear(BROWN);
					break;
				}
			}

			spilcd_show_string(10, 40,  200, 32, 32, "ESP32-P4", RED);
			spilcd_show_string(10, 80,  200, 24, 24, "SPILCD TEST", RED);
			spilcd_show_string(10, 110, 200, 16, 16, "WKS SMART", RED);
			spilcd_show_string(10, 130, 200, 16, 16, (char *)lcd_id, RED);       /* 显示LCD ID */
			x++;
			if (x == 12)
			{
				x = 0;
			}

			LED0_TOGGLE();
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}
	else if(key == BOOT_PRES)     /*wifi APP*/
	{
		wifi_app_flag = 1;

		lcd_init();               /* MIPI LCD初始化 */
		lcd_show_splash(splash_image_data);                             /* 显示开机启动图 */
		lcd_backlight_on();                                             /* 打开背光 */
		vTaskDelay(pdMS_TO_TICKS(2000));                   /* 启动图持续显示 */

		if (mipidev.id == 0x79007)         /* 7寸 1024*600 MIPI屏幕 */
		{
			lvgl_demo();
		}
	}
}
