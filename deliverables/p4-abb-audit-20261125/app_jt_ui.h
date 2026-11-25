#ifndef __APP_JT_UI_H__
#define __APP_JT_UI_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_jt_ui_init(void);
void app_jt_ui_del(void);

/** Ask the background worker to fetch /lockscreen immediately. */
void app_jt_ui_set_lock_screen_enter_callback(void (*cb)(void *ctx), void *ctx);

/** Notify the UI that the user returned to the lock screen; fetch throttle
 *  is handled outside the GUI task. */
void app_jt_ui_on_lock_screen_entered(void);

/** Update lock-screen dynamic values. Strings are copied. */
void app_jt_ui_set_environment(const char *temperature, const char *memo);
void app_jt_ui_set_clock(const char *hhmm, const char *date_md, const char *week);

/** Capture/debug only: show one of the eight v113 screens immediately. */
void app_jt_ui_debug_show_page(int page);

/** Capture/debug only: freeze the processing page so it does not auto-advance. */
void app_jt_ui_debug_freeze_process(bool freeze);

/** Capture/debug only: exercise SoftAP start/stop from the UART console. */
void app_jt_ui_debug_softap(bool start);

/** Capture/debug only: run the three-stage connectivity test from the UART console. */
void app_jt_ui_debug_chain_test(void);

/** Capture/debug only: report the live SoftAP state from the UART console. */
void app_jt_ui_debug_ap_state(void);

/** Flow/debug only: invoke the same handler as the finder shutter/confirm buttons. */
void app_jt_ui_debug_finder_shutter(void);
void app_jt_ui_debug_finder_confirm(void);

#ifdef __cplusplus
}
#endif

#endif
