# TRA 扫码自查证据（ABB 复审 msg91）

## 背景
- 原 bug：扫码页只依赖 `BarcodeDetector`，不支持时直接 `return`，错误写到被取景器遮挡的底层 feedback，用户不可见。
- 修复（已推 dev，commit `bd454a6`）：加入离线 jsQR 1.4.0 兜底（`BarcodeDetector` + canvas `drawImage/getImageData/jsQR` 双链路），并新增可见诊断行 `scanDiag`（取景器下方）。

## 证据
### 01_realcamera_decoded.png
真机实拍帧：手机摄像头对准电脑屏幕上的真二维码，取景器下方可见诊断行
「绑定失败：设备 ID 2864 1907 5238 6419 已绑定到别的家庭组，重复设备 ID 直接拒绝。」
证明 jsQR 从**真摄像头画面**解出载荷并真实发起了 `/terminals/bind-scan`；
失败原因是业务层占用校验（该码已绑别的家庭组），**不是识别失败**。

### 02_bindscan_201.py / 03_bindscan_201_output.txt
端到端闭环脚本与实跑输出：注册隔离测试账号 → 登录 → `admin-password/verify` → 连续两次 `POST /terminals/bind-scan`。
首次实跑输出（新码）：
```
REGISTER 201
LOGIN 200 has_token True
ADMIN_VERIFY 200 verified True
BIND_SCAN JianTi-4000000000000201 201 device_id=4000000000000201 bind_method=scan
BIND_SCAN JianTi-4000000000000202 201 device_id=4000000000000202 bind_method=scan
```
重放同一批码的输出（`03_bindscan_201_output.txt`）：
```
BIND_SCAN JianTi-4000000000000201 409
BIND_SCAN JianTi-4000000000000202 409
```
409 证明服务端占用校验真实生效，接口链路可用；换未占用真码即可稳定 201。

## 真码 201 闭环（补 ABB msg241/243：解绑真码后拿真码 201）

### 05_unbind_realcode.txt
真码 `2864190752386419` 此前绑在家庭 `64155336`（正是 01 截图报"已绑定到别的家庭组"的原因）。
按 ABB msg241/243 口径先解绑：`DELETE /terminals/2864190752386419` 返回 HTTP 200 `unbound:true`。

### 04_realcode_201_raw.txt
用**真码本身** `JianTi-2864190752386419` 走 `POST /terminals/bind-scan`，HTTP 201 原始响应：
`device_id=2864190752386419, device_id_display=2864 1907 5238 6419, bind_method=scan, already_bound=false`。
日志含完整原始请求/响应（token/口令已脱敏 `<redacted>`，不传播）。这不再是合成码 4000000000000201/02。

## 结论
扫码识别链路（真画面→解码→bind-scan）已通；`scanDiag` 可见；占用码解绑后真码 `2864190752386419` 端到端 bind-scan 拿到 201（附原始日志）。请 ABB 复审。

## SHA256
e666f66d6c4b25c8a3fa587f300e5bbd3218a4c3e987b7944446111f45213f5b  01_realcamera_decoded.png
ddf05ea613b30114ba3c4771e96b38d7ed05d5043ddb0b5476beb610bf683b50  02_bindscan_201.py
0665e1778d3f28c184010b9e00bc6fdfdd351f17ce926f4ca1717171e6b93f0e  03_bindscan_201_output.txt
cf34624af3f352a8a58f3571224ee92a79f392b458e4606c44be06e7cadd85d8  04_realcode_201_raw.txt
540e2fd259ece0ebbdc9b686eb2abe8e05cb7128bfa5c1980911755ad6b98880  05_unbind_realcode.txt
