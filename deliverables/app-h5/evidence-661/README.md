# 661 装机全链路截图留档（Tra）

- 设备：1578472099005SJ（安卓真机）
- APK：jianti-2.0.0-debug.apk，SHA256 前缀 9a6a4ee9（dev commit 434d116 交付版）
- 内嵌 H5：assets/index.html SHA256 前缀 3ee20117，`LOCAL_MODE=true`（演示态）
- 网关：本地 v8 测试模式 192.168.0.108:8000（仅用于连通测试，演示态下业务 API 未经网关）
- 结论（Nyx 裁定 02:43）：装机 PASS、演示态全链路走通计 PASS；不作真后端 PASS 依据。
  真后端链路（扣卡余额变化、使用记录入库）等浩爷定 v9 上 43/现网 + 测试账号后再测。
- 截图均来自 `adb exec-out screencap -p` 真机实拍。

| 文件 | 内容 |
| --- | --- |
| 07_now.png | 设置屏：官方/自有服务器、SSID JT-DEMO、信号 -52 dBm |
| 10_after_confirm.png | 自有服务器 192.168.0.108 保存后，自有服务器选中 |
| 11_conn_test.png | 连通测试：WiFi/互联网/服务器三项全绿 |
| 12_home.png | 拍题首页（服务器未连接、卡数 ---，演示态特征） |
| 14_shoot.png | 取景器：预置计算题 + 学生作答，拍照/返回/确认 |
| 15_shutter.png | 快门拍照后暂存帧 |
| 16_confirm_send.png | 判题结果屏：第1题做错、解题思路、已解锁 |
| 17_last_q.png | 第5题结果屏：思路、已解锁（5题全通） |
| 18_home_after.png | 走完链路后首页 |
