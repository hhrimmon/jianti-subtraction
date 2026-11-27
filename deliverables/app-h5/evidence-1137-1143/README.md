# 1137 / 1143 真机证据（2026-11-27）

## 1137：第3屏跳第4屏卡死
- 真机：vivo V2046A / 1578472099005SJ，2376x1080，APK 为本目录上一级 `jianti-2.0.0-debug.apk`。
- 修复：点对号当帧 `goTo(3)` 进第4屏处理页，不再等 BarcodeDetector 与 `/solve` 返回；失败由 `toShooterFromSolve()` 回第3屏并给可见错误；登录后续发同样先跳第4屏。
- 运行时证据（Debug Server HTTP 插桩，已在最终版移除）：
  - 未登录：confirmSend -> `hasAuth=false` -> 第9屏登录页，符合设计（非卡死）。
  - 已登录：processJpegBytes -> image decoded -> proceed -> `/solve` catch `code=terminal_not_bound status=403`；服务端 `terminal_auto_bind.status=bound_elsewhere`，该测试终端号已绑到别家，属测试环境绑定态，非 App 动画 bug。
- `v_after_login.png`：登录后自动续发，落入“简题 处理中...”第4屏处理页（上传/网关/大模型/NLP/下发五阶段）。
- `vf_1.png`/`vf_2.png`/`vf_3.png`：修复版拍摄→点对号后 2/4/6 秒抓帧，进第4屏处理页；未完成整条判题是测试终端 `terminal_not_bound` 所致，2 秒内明确回退取景器并显示错误，不再无反馈卡死。
- `v_unauth_land.png`：未登录点对号，正确落登录页。

## 1143：v156 二维码页
- `f_settings3.png`：设置页只显“网络状态：网络已连接”，无 SSID/信号；含“显示设备二维码”入口。
- `f_qr.png`：二维码页只留码 + 关闭按钮，底色+黑码（无白色方块底），无标题/说明/设备号行，符合 1143 对 v156 的纠正。

## SHA256
- df514aa707c2a32e83a54f880b40b6bfd5f4d8bbb1fec0e0ce063facdafbea73  f_settings3.png
- d1173a9001c48ff22d42474962907dd985820b4242e5a07f2bb04ec951d87f22  f_qr.png
- a8130226eb6cb66d8745bd0f239ad79b9d2f58f7de1601938f58bdb36ae49a06  v_after_login.png
- 56f507dfb35c23a64c91affab19ce1c9525dbd92b0dcd4e657fb76e82992837a  vf_1.png
- 2e2ed9dc3759c929c5fee533f53812731b3e28ba45b4c998301e00d0d46a6ac2  vf_2.png
- dac324263edf9de980cfcf50818a8cb34492ac82d7f3c3a42daa15f94ea5a30a  vf_3.png
- 006c0f1752e1568aa7ba269f688c7c07b310391266c30045eb4d7f82a129c0b5  v_unauth_land.png
