# 板子 SoftAP 配网 API 契约（草稿 v0.1，等 NYX 拍板口径后定稿）

依据：PRD v5.10 §5.4、开发手册 v3 §5（配网页**只连 Wi-Fi**，不放服务器二选一）。
页面：`ap_config_page_draft.html`（由板子固件在 `192.168.4.1` 提供，captive portal 自动弹出）。
归属：DSH 出页面＋契约＋本机模拟验证；COD 接入 ESP-IDF `esp_http_server`（欢迎分工）。

## 行为约定（PRD/手册硬口径）

1. SoftAP 只在第 7 屏点「配置 Wi-Fi」后启动；关页热点随之关（固件侧控制，与页面无关）。
2. 页面不放服务器二选一、不填服务器地址；服务器默认官方，手机 App 上改、由服务端下推（/config/server 已实现）。
3. Wi-Fi 连上后**绑定二维码显示在板子屏幕上**（屏逻辑），页面只提示"看板子屏幕"，全程一个二维码。
4. 温度/MEMO 不在配网里配。

## API 契约

### GET /ap/scan
扫描周边 Wi-Fi。响应（同源 JSON，无需鉴权）：
```json
{"ok": true, "items": [{"ssid": "MyHome", "rssi": -52, "auth": true}]}
```
- `rssi`：整数 dBm；`auth`：true=加密网。隐藏网不走扫描（用户可用"其他网络"入口，暂缓）。
- 扫描在 SoftAP 信道切换期间约 2–4 秒，页面提示"正在扫描…"即可，接口允许超时重试。

### POST /ap/connect
```json
请求：{"ssid": "MyHome", "password": "…"}
响应：{"ok": true}                    // 已受理，结果走 /ap/status
      {"ok": false, "msg": "请先选择一个 Wi-Fi"}
```
- 固件收到后启动 STA 连接；页面 1.5 秒一次轮询状态。
- 密码错误也走 status=failed（不在本接口判）。

### GET /ap/status
```json
{"ok": true, "stage": "connecting"}                       // idle|connecting|connected|failed
{"ok": true, "stage": "connected", "ssid": "MyHome", "ip": "192.168.1.23"}
{"ok": true, "stage": "failed", "msg": "密码错误或信号不佳"}
```
- `connected` 后固件保持 SoftAP + STA 共存 60 秒供页面收尾，随后可按 PRD 关热点；
  绑定二维码在板子屏上显示（屏逻辑），页面只提示。
- `failed` 后可再次 POST /ap/connect 重试。

## 验收对照（手册 F5/§5）
- 页面无服务器二选一、无服务器地址输入 ✅
- 只连 Wi-Fi；联上提示看板子屏二维码 ✅
- 配网成功后终端拉取服务端值（温度/MEMO）不在配网内 ✅（服务端已实现）
