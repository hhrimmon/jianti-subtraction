P4 ABB audit evidence bundle
Source (desktop host): D:\AItools\projects\jt2.0\_work
Progress section: p4_development_progress.md (ABB 572/NYX 580 rework and gear fix sections)

Changed sources:
esp32p4/p4b/main/APP/MAIN_UI/app_jt_ui.c
esp32p4/p4b/main/APP/MAIN_UI/app_jt_ui.h
esp32p4/p4b/main/APP/MAIN_UI/app_wifi.h
esp32p4/p4b/main/APP/MIPI_CAM/mipi_cam.c
esp32p4/p4b/main/APP/MIPI_CAM/mipi_cam.h
esp32p4/p4b/main/APP/JT/jt_client.c
esp32p4/p4b/main/APP/JT/jt_client.h
esp32p4/p4b/main/main.c
esp32p4/p4b/main/APP/IMAGE/jt_v113_icons.c
esp32p4/p4b/main/APP/IMAGE/jt_v113_icons.h
make_jt_icons.py

Source fixes in this pushed copy:
- jt_show_softap(false) always stops AP/DHCP and sets WIFI_MODE_STA, regardless of STA connection state.
- jt_client_internet_reachable() performs an independent public HTTP GET probe for chain stage 2; server stage uses jt_client_server_reachable().
- jt_client_terminal_id_source() marks the active terminal ID as compile-time-default or runtime-config.
- Gear button image is regenerated from v113/screen1.png crop (936,28)-(976,68).

Build/flash evidence (full text, .log.txt to avoid Git LFS pointer files):
build_abb_camera_final.log.txt + flash_abb_camera_final.log.txt
build_abb_settings_log.log.txt + flash_abb_settings_log.log.txt
build_abb_softap_chain.log.txt + flash_abb_softap_chain.log.txt

Reaudit evidence (full text):
abb_audit_boot_id.log.txt
abb_audit_camera_serial_evidence.log.txt
abb_audit_chain_only.log.txt
abb_audit_chain_serial_evidence.log.txt
abb_audit_nyx_rework_serial.log.txt
build_abb_reaudit_console.log.txt
build_abb_reaudit_console2.log.txt
flash_abb_reaudit_console2.log.txt

No new source files in this update; the listed changed sources are unchanged.

2026-11-25 rework rebuild/flash:
- build_abb_reaudit_console2.log.txt / flash_abb_reaudit_console2.log.txt (ABB 572 fixes)
- build_gear_fix.log.txt / flash_gear_fix.log.txt / monitor_gear_fix.log.txt (gear icon fix)
- comprehensive_routine_70inch_mipilcd.bin (final gear-fix firmware)
- gearfix_01_gear.png (PC-camera board photo showing corrected gear)
- SHA256SUMS.txt includes every file in this bundle; SHA256SUMS_SELF.txt records SHA256SUMS.txt itself.
