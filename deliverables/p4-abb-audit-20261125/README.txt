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

2026-11-25 pushed-firmware evidence closure:
- p4_3fcb898_hotspot_chain_serial_ready.log.txt captures hotspot on/off, independent Internet probe,
  server probe with host=..., and final chain test on pushed commit 3fcb898.
- gear_image_button_preview.png and gearfix_01_gear.png are force-added despite the repository
  *.png ignore rule, so every referenced evidence file is present in this commit.
- p4_development_progress.md records the exact serial lines and the non-blocking softap netif warning.

2026-11-25 NYX 639/640 evidence:
- build_prd_flow_readyguard.log.txt and flash_prd_flow_readyguard.log.txt cover the camera readiness fix.
- p4_prd_flow_wait_ready2.log.txt is the pushed-firmware-style serial capture: camera started,
  PHOTO008.jpg saved, JPEG submitted, solve callback returned result=2 questions=0, no reboot.
  Correction: result=2 is JT_RESULT_HTTP_ERROR, not server_error (jt_client.h enum: 0/1/2/3/4).
- p4_prd_flow_02_finder_20261125.png shows the live MIPI preview with a real paper question.
- app_jt_ui.c/.h, main.c, mipi_cam.c and jt_business.c/.h are refreshed from the current firmware tree.
- cardflowfix_05_after.png, ideacardfix_05_dupfix2.png and the finder photo are force-added despite *.png ignore.

2026-11-25 /solve auth correction:
- Bare POST /solve reproduced 401 unauthorized; firmware JT_AUTH_TOKEN was empty.
- Injected current family session token and added HTTP error status/body logs; built build_auth_token_fix.log and flashed flash_auth_token_fix.log.
- p4_auth_fix_shutter.log: PHOTO011.jpg 70336 bytes, solve async start, then result=3 questions=0.
- Server job-00591dc7dc31 received image_bytes=70336 and failed with no_question; upload/auth path PASS, recognition/content remains.
