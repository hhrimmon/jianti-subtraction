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

## v5.8 / v5.9 真机证据（补 ABB msg284：dev 上缺证据文件）

### 06_frozen_shutter_check.png
快门后真机截图：画面定格、快门置灰、对号黑底白勾，toast「已拍好，点对号发送」。
证明 msg259 冻结态和 v5.8「拍好了」状态可见。

### 07_gate_login.png
未登录时点对号，真机弹出登录页并提示「请先登录，登录后再发送」。
证明 v5.9 登录门槛生效。

### 08_login_resume_processing.png
在上述门槛登录后，App 自动回到发送流程，直接进入处理页（上传/网关已完成，大模型阶段）。
证明「登录后继续发送刚才那张」生效；登录请求已带 `X-Terminal-Id`，按浩爷定的方案 A 由服务端自动绑终端，App 不再直调 `/terminals/bind`。

### 09_frozen_back_refinder.png
冻结态点返回，真机回到取景重拍，toast「已回到取景，请重新拍」。

### 10_finder_back_home.png
取景态再点返回，真机回到第 2 屏 home。

## v5.10 / msg325 真机证据

### 11_lockscreen_server_memo.png
锁屏真机截图：App 切到自建服务 `127.0.0.1:8000`（adb reverse）后，锁屏显示服务端 `/lockscreen` 下发的 MEMO，温度服务端返回 null 时显示 `---°C`（正确行为，不是丢数据）。补 ABB msg318 的 v5.10 锁屏证据。

### 12_server_persist_after_restart.png
`adb shell am force-stop com.jianti.app` 后重开 App，设置页仍选中「局域网/自建服务」、`服务器已连接`，回到锁屏仍显示服务端 MEMO 与 `---°C`。证明 msg325② 的自建服务器地址已持久化（localStorage `jt2-server-address`），重开客户端不再回官方服务器导致锁屏数据丢失。

### msg325① 答案对不上拍的题：根因复现
真机拍「白纸/黑条」发 `/solve`，本地服务端 `/health` 明示 `test_mode:true`，返回的却是固定数学题解析——即服务端在测试模式用 mock 答案，与上传照片无关，不是 App 发错图。已在 App 侧加显式红底横幅「测试模拟数据，非真实判题结果」，结果页不再把 mock 冒充真实判题；服务端侧同步 `mock`/`result_source` 标记（待 DSH 部署）后双保险。轮询严格按 `activeJob.jobId` 丢弃旧 job 结果，防止串题。

## SHA256
e666f66d6c4b25c8a3fa587f300e5bbd3218a4c3e987b7944446111f45213f5b  01_realcamera_decoded.png
ddf05ea613b30114ba3c4771e96b38d7ed05d5043ddb0b5476beb610bf683b50  02_bindscan_201.py
0665e1778d3f28c184010b9e00bc6fdfdd351f17ce926f4ca1717171e6b93f0e  03_bindscan_201_output.txt
cf34624af3f352a8a58f3571224ee92a79f392b458e4606c44be06e7cadd85d8  04_realcode_201_raw.txt
540e2fd259ece0ebbdc9b686eb2abe8e05cb7128bfa5c1980911755ad6b98880  05_unbind_realcode.txt
082cdf5806babd3a0ecf95daa246fbe105cd9050d00cbebca67863590bd19638  06_frozen_shutter_check.png
d8741f5afb92f9f32dc1ac59e388290700d03f5334aad37542c326a61bc3d6ea  07_gate_login.png
167a694b35358ec9d2c3a1147ba8073b2aa3c7ac0eed01a304e4fea09fd36293  08_login_resume_processing.png
8778293229b626f3463b150acd02b0d5857e33ff893ef44cb931c981930fe8c7  09_frozen_back_refinder.png
e699235428047421e7b016cef929cf5fcfaa89d1d74ec42843224c1cff88a968  10_finder_back_home.png
b3159c9c5e4d19825ea342a611c49b22567d1ac068de9fdb13606b37e1243e51  11_lockscreen_server_memo.png
0e4732a921e1d75c762a8c58a198d0a0d4b426693dae3bf205c67815d0b3f74a  12_server_persist_after_restart.png
