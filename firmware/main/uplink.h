#pragma once
/* JianTi v0.2 uplink interface.
 * uplink.c owns: WiFi station, HTTP POST of the gray JPEG, gateway reply parse.
 * main.c owns: the display + dummy-draw flag, and implements jt_show_result /
 * jt_back_to_preview (declared here, defined in main.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void jt_uplink_init(void);
/* Copy a freshly encoded JPEG for the upload task (called from capture task). */
void jt_uplink_offer_jpeg(const uint8_t *data, size_t len);
/* Wake the upload task (called from capture task when an upload was requested). */
void jt_uplink_kick(void);

/* Implemented in main.c. 壳上显示「完成 + 上传秒数+作答秒数」，
 * 完整答案文本始终打到串口日志。 */
void jt_show_result(bool ok, const char *text, int latency_ms,
                    int upload_ms, int answer_ms);
void jt_back_to_preview(void);

#ifdef __cplusplus
}
#endif
