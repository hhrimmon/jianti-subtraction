# 简题 App（手机端 H5）

手机 App 从零开发的第一版实现。界面逐屏照 v155 原型（`shouji-app-v155-full.html` / `app-v155.html`）复刻，交互、翻屏、弹窗、输入格式化与原型一致；数据层从原型的内置演示变量改成调用服务器接口，接口不通时如实提示、不伪造结果。

## 交付物

| 文件 | 说明 |
|---|---|
| `index.html` | 全部实现，单文件（HTML + CSS + JS），无外部依赖、无 CDN |
| `README.md` | 本文件 |
| `SELFTEST.md` | 自测记录（方法、结果、限制） |
| `manifest.sha256` | 每个交付文件的 SHA256 |

## 怎么跑

1. 直接用浏览器打开 `index.html` 可看界面与交互。
2. 要真正调接口，建议用静态服务器起本地 http（`file://` 打开时浏览器会拦跨域请求，接口一律失败）：
   ```
   npx serve .
   ```
3. 打开后先在设置（第 6 屏）里确认网关：默认官方地址 `solve.mhq-ai.xyz:8000`（界面不显示该地址），也可填自有 IP（端口 8000）。

## 接口对接

- 基地址：默认官方 `solve.mhq-ai.xyz:8000`；自有地址在设置屏填写后生效，仅存内存。
- 统一信封 `{code,message,data}`，程序只判 `code`；`message` 仅用于展示。
- 鉴权：登录/注册成功后把 token 放内存变量，**不落盘、不硬编码**；退出登录即清空。
- 幂等：扣卡、预支、申请、审批每次操作运行时用 `crypto.randomUUID()` 生成 `Idempotency-Key`；拍题按**照片内容哈希**复用同一个 key，同一张照片重发不会重复建 job。
- 请求超时 8 秒；超时或连不上统一提示，停留在当前屏，不自动重试。429 读 `Retry-After`，提示用户等 N 秒后手动按快门重试，不自动循环。
- 设备号：按文档 16 位纯数字，第 1 位是类型码（1 嵌入式 / 2 安卓 / 3 苹果 / 4 鸿蒙 / 0 其他），后 15 位由本机种子稳定哈希得到，存 `localStorage` 的种子只为保证同一台机号不变，不存任何 token。
- 待批申请（B2）每 30 秒轮询一次，有单就弹消息条；没登录时不发请求。

已接端点（路径按 `平台接口-20261116.md`）：

| 组 | 端点 | 用途 |
|---|---|---|
| A1 | `POST /solve` | 拍题上传（raw JPEG，`X-Terminal-Id` + Bearer + Idempotency-Key） |
| A2 | `GET /solve/{job_id}` | 轮询进度，1 秒 × 最多 60 次，按 `stage_index` 点亮五阶段 |
| A3 | `POST /cards/check` | 查余额/解锁态 |
| A4 | `POST /cards/deduct` | 扣 1 张取答案 |
| A5 | `POST /cards/advance` | 预支 1 张 |
| A6 | `POST /applications` | 发申请（成功后停在领取页只显示“申请已发送。”） |
| A7 | `POST /applications/{id}/withdraw` | 撤回申请 |
| A8/A10 | `GET /config/server`、`GET /config/environment` | 服务器配置、温度/MEMO |
| A9 | `GET /usage?range=7d\|30d\|90d` | 官方模型用量（`items[model,calls,cost]`、`total_cost`） |
| A12 | `POST /solve/{job_id}/cancel` | 处理页没出结果就离开时撤掉本次任务（不弹窗、不提示） |
| A13 | `POST /solve/{job_id}/skip` | 已实现方法 `api.solveSkip`，但 v155 原型没有「跳过」入口，界面不调用 |
| B1–B13 | applications / terminals / models / config / records / cards rules | 管理端各屏 |
| B14–B20 | `/auth/*`、`/family*` | 登录注册、管理密码、家庭组 |
| C3 | `POST /models/mcp/login` | 登录 MCP |

失败分支按接口文档处理：管理口 401 回登录屏；终端 401 回设置屏，`40101` 提示“登录已失效”、未绑定提示“设备未绑定”，`40001` 也跳设置屏提示绑定；`failed/recognition_failed` 回取景器提示重拍，`no_question` 提示“这张里没有题目”；轮询超时回 home；余额 0 转领取页；已解锁直接进结果页；413 连续两次提示“照片太大，请靠近一点拍”；429 按 `Retry-After` 提示手动重试。

## 本轮（P0/P1/P2）已改

- 审批按 B1 传 `grant_type`（按张 `cards` / 按倍 `times`）+ `grant_value`，张数 1–9、倍率 1–3；余额以服务端 `balance` 为准。
- 改密码按 B14/B15 分别传 `type:'login'|'admin'`；重置管理密码验证码 purpose 用 `reset_admin_password`；MCP 登录字段 `captcha_code`，去掉无对应的验证码按钮。
- 服务器配置保存 `server_mode:'custom'`；拍题先判绑定码（`JianTi-` + 16 位），扫到只绑定不解题。
- 余额、每日张数、预支倍率、已预支、家庭组 ID、家庭组成员、三组统计、用量全部取服务端；拿不到显示 `---`，不再写死 5/5，也不按本地计数顶。
- 官方模型列表按 B8 `items` + `active_model_id` 渲染，选中调 B9；用量按 A9 五个官方模型名映射，缺项 `---`。
- 家庭组管理员判定：登录/注册成功时缓存服务端返回的 `role`／`member_no`，进家庭组优先按 `role`（管理员/admin）判，没有 `role` 就用 `member_no` 与 `GET /family` 的 `admin_member_no` 比对；`GET /family` 顶层不给 `role`／`member_no`，不再在顶层猜，也不会恒判非管理员。
- 手动绑定成功、扫绑定码成功后都刷新终端列表；拍题没选照片时提示重拍，不再拿 96×96 假图顶上去发网关。
- 预支上限在没拿到 `advance_limit`／每日张数时不做本地判断，交给服务端 A5 裁决；`loadCardRules()` 无 token 时不发 B12。

## 演示值说明（非验收依据）

以下数值是原型走流程用的演示值，接口返回真实值后覆盖：

- 思路页预置题目文本（未拍照时的视觉占位，真实题目来自 A2 `questions`）
- 充值档位 ¥10/30/50/100（按 v155 原型保留；接口文档无对应端点，按钮只提示“未接入”）

按裁决：预支倍率默认 **0**（原型的 1 是演示值），每日张数默认 5。解题卡余额、每日张数、已预支、公式结果在拿到 A3 之前一律显示 `---`，不显示 5/5。

不编数字：温度读不到显示 `---°C`，MEMO 读不到显示 `---`，服务器连不上时状态如实显示“服务器未连接”（连通后变“服务器已连接”）。家庭组成员、已绑定终端、使用记录、用量、官方模型余额一律取服务端实数；没有数据就留空或显示 `---`／`¥--`，不用演示手机号、演示设备号或演示用量顶。官方模型余额与充值在接口文档里没有对应端点，充值按钮只提示“未接入”，不在本地加钱。

## 已知限制

- 本版是 H5 实现，拍照走浏览器 `capture` 能力，不是原生相机封装；安装包（安卓原生壳）未包含在本次交付。
- 扫码绑定用 `getUserMedia` + `BarcodeDetector` 实读二维码：浏览器不支持 `BarcodeDetector`（如部分 iOS）时提示改用「输入设备号」，不生成演示码。
- 接口只按文档接通并做了降级，未在真实服务上联调（`file://` 下跨域被拦，且不碰线上配置）。
- 判题链路（A1/A2）需真实终端 token 与已绑定设备才能跑通，本版未内置任何 token。

## 安全

不包含任何 token、密码、密钥；不写入本地存储；不改服务器端代码与线上配置。
