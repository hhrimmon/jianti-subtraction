# 自测记录（2026-11-17）

环境：Windows / Node 22 / Playwright（Chromium 内核，Edge 通道）headless，视口 1280×720；被测文件 `deliverables/app-h5/index.html`，对照原型 `shouji-app-v155-full.html`。

## 1. 结构检查

| 项 | 结果 |
|---|---|
| 屏数 `.screen` | 16（与原型一致） |
| 评审 UI 残留 `#indexBar` / `#screenDesc` | 无 |
| 残留 JS 变量 `indexBtns` / `#indexBar` 引用 | 无 |
| 旧演示码 `104005983277` / `scanDeviceTail` | 无 |
| 新演示码 `2864 1907 5238 6419` / `JianTi-2864190752386419` | 已在 |
| 写死的演示手机号 / 演示设备号 / 演示用量（`13800138000`、`DEMO_TERMINAL_CODES`、`usageData`、`simulateInviteJoin`） | 无（本轮已清） |
| 外部依赖（CDN/字体/图片外链） | 无，单文件 |

## 2. 运行期检查

- 加载 16 屏逐屏截图，`pageerror` 与 `console.error`：**0 条**（仅剩 `file://` 下必然出现的跨域网络失败，属预期）。
- 交互点击（force 点击，每步等 350–1500ms）：首页拍照入口、快门、取景器返回、解题卡、预支、发申请、卡管理、终端管理、输入设备号、服务端管理、官方模型、登录 MCP —— **均无 JS 异常**；接口不通时均停留在当前屏并给出提示，未跳转到结果页谎称成功。

## 3. 逐屏视觉比对

16 屏截图与原型逐屏核对：版面、字号层级、黑白灰配色、边框圆角、按钮位置一致。截图中可见的差异只有数据状态，不是设计差异：

| 屏 | 差异 | 原因 |
|---|---|---|
| 1 锁屏 | 温度 `---°C`、MEMO `---` | 原型是演示值；按 PRD 温度/MEMO 未查到不编数字 |
| 2 home | 状态胶囊显示“服务器未连接” | 原型写死“已连接”；未连通时如实显示，连上后变“已连接” |
| 13 卡管理 | 余额/每日张数/公式显示 `---`，输入框留空 | 未拿到 A3 服务端数据；按裁决不显示 5/5 演示值 |

截图目录：`_work/jianti-app-src/shots-1117/`（`proto-screen-*.png` 对照 `impl-screen-*.png`）。

## 4. 本轮修掉的问题

1. 删评审 UI 时连带删掉了 `.full-layout` 的 CSS，舞台宽度塌成 0（首屏全黑）——去掉只用于装评审侧栏的外壳，`.stage` 恢复 1152×648。
2. `#prevBtn` / `#nextBtn`（评审翻屏箭头，已随评审 UI 删除）取元素为空导致 `addEventListener` 报错——改为存在才绑定。
3. 状态胶囊写死“服务器已连接”，未连通也这么显示——改为按 A8 结果切换。
4. 接口无超时，DNS 挂起时界面状态一直不定——加 8 秒 AbortController 超时与统一提示。
5. 注册接口路径/字段与接口文档不符（`/users/register`、`login_password`、`sms_code`）——改为 `/auth/register`、`password`、`code`。
6. 预支倍率为 1（演示值）——按裁决改 0，且倍率为 0 时不本地拦截、交给服务端判定上限。
7. 第 13 屏两个输入框仍是静态初值（倍率框显示 1，与算出来的 5 张矛盾）——`renderCardBalance()` 里把 `dailyInput` / `ratioInput` 同步为 `dailyTotal` / `advanceRatio`，正在输入时不覆盖；本轮进一步改为未拿到 A3 时显示 `---`、输入框留空。
8. 官方基地址缺端口（代码默认 `http://solve.mhq-ai.xyz`，文档写 `solve.mhq-ai.xyz:8000`）——`DEFAULT_BASE_URL` 改为 `http://solve.mhq-ai.xyz:8000`。
9. 扫码绑定用写死演示码、按序号顺延编号、请求字段与 B5 不符（审查 H1）——改为 `getUserMedia` + `BarcodeDetector` 实读二维码，`api.bindScan({qr_payload})`；读不到载荷只提示、不编设备号，成功后按服务端列表重画。
10. 两个 `renderUsage` 重名、后者覆盖前者（审查 H2）——拆为 `renderDailyUsage`（每日四项）与 `renderUsageReport`（用量页），调用点按参数依赖分别指向。
11. 预支上限被写进 `dailyTotal`（审查 H3）——新增 `advanceLimit` 单独保存，余额不再被预支数覆盖。
12. 401 不分终端/账号（审查 M3）——终端请求回设置屏并清终端 token，账号请求回登录屏。
13. 余额可为负（M4）、`advanced=0` 被当无值（M5）——统一 `Math.max(0, …)` 与 `!= null` 判定。
14. 用量页写死演示用量/费用、充值本地加钱（M6）——用量只渲染 `GET /records` 实数，无数据显示 `---`；官方余额无接口显示 `¥--`，充值只提示“未接入”，不改余额。
15. 家庭组写死成员、本地假加入（M7）——进屏 `GET /family` 渲染，通过/拒绝/转让/删除都走接口后重拉；删除 `simulateInviteJoin`、`famJoinRequest` 与写死成员行；未接服务端或没有成员时显示 `---`，不拿演示手机号顶。
16. 已绑定终端只有 5 个静态行、设备号写死——改为按服务端列表动态建行（多少台建多少行），解绑成功后重拉。
17. 设置屏账号手机号静态写死 `13800138000`——未登录显示 `—`，登录成功后填真实号码。
18. L1 端点表被整体删掉——按《简题平台接口文档》2026-11-16 版补回 `api.ENDPOINTS` 对照表（A1–A8、A9、A10、A12、A13、B1–B19、B20 六子端点、C3；A11 作废不列），只作对照、不参与运行。
19. 审批未按 B1 传 `grant_type`/`grant_value`（P0）——按张 `cards` 1–9、按倍 `times` 1–3；余额优先取服务端 `balance`/`granted_cards`，不本地硬加。
20. 改密码/重置管理密码/验证码 purpose/MCP 字段与文档不符（P0）——改登录密码 `{type:'login',old_password,new_password,new_password_confirm}`、改管理密码 `{type:'admin',new_admin_password}`、重置 `{phone,code,new_admin_password}` + purpose `reset_admin_password`；MCP 登录字段 `captcha_code`，删掉无对应端点的“获取验证码”按钮。
21. 服务器配置保存字段（P0）——改 `server_mode:'custom'`，读取兼容 `official`/`pub`。
22. 拍题未先判绑定码（P0）——扫到 `JianTi-` + 16 位只调 `bindScan({qr_payload})` 绑定并刷新终端列表，本次不解题；没扫到才走 A1。
23. 余额/每日张数/预支倍率/已预支/公式写死演示值（P1）——全部改为 A3 `cardsCheck` 返回值；未拿到时 `---`、输入框留空；总可用按 每日张数×(1+倍率)、预支上限按 每日张数×倍率。
24. 设备号编造（P1）——`makeDeviceId()` 生成 16 位纯数字，首位类型码（1 嵌入式/2 安卓/3 苹果/4 鸿蒙/0 其他），后 15 位由本机种子稳定哈希；`localStorage` 只存种子，不存 token。
25. 官方模型列表/选中态未接 B8/B9（P1）——按 `items[]` + `active_model_id` 渲染，点行调 `selectModel({model_id,line:'official'})` 成功才高亮。
26. 待批申请只在本地出现（P1）——`loadApplications()` 拉 B2 `status=pending`，每 30 秒轮询，有单弹消息条；没登录不发请求。
27. 卡规则未回显（P1）——`loadCardRules()` 读 B13 `daily_total`/`advance_ratio`；每日张数 1–9、倍率 0–3 夹紧。
28. 用量/使用记录未按文档取数（P1）——用量改 A9 `GET /usage?range=7d|30d|90d`，五个官方模型名映射，合计取 `total_cost`；三组统计取 B11 `summary`，日志用服务端 `date`+`time`，来源默认“终端 1”。
29. 家庭组 ID 写死 `12345678`（P1）——改为读 `GET /family` 的 `family_group_id`，拿不到显示 `---`。
30. 家庭组管理员判定靠猜（P2）——改为缓存登录（B16）返回的 `role`／`member_no`：有 `role` 按“管理员/admin”判，没 `role` 就用缓存的 `member_no` 与服务端 `GET /family` 的 `admin_member_no` 比对，两者都没有才判非管理员（`GET /family` 顶层本来就不给 `role`／`member_no`）。
31. 手动绑定成功后终端列表不刷新（P2）——成功后调 `loadTerminals()`；扫绑定码路径同样刷新。
32. 拍题上传缺 8MB 预检/413/429/40001 分支（P2）——超 8MB 不发；413 连续两次提示“照片太大，请靠近一点拍”；429 读 `Retry-After` 提示 N 秒后手动重试；`40001` 跳设置屏提示“设备未绑定，请先绑定终端”。
33. 同一张照片重发换新幂等键（P2）——按照片内容哈希复用同一个 `Idempotency-Key`。
34. 没选照片时拿 96×96 假图顶上去发网关（P2）——删除，改为提示“还没有照片，请先拍一张题目的照片。”
35. 空白题文案与文档不一致（P2）——`no_question` 提示“这张里没有题目”，`recognition_failed` 仍提示“没看清题目，请重拍”。
36. 终端 401 只给一句话（P2）——按码区分：`40101` 提示“登录已失效”，未绑定提示“设备未绑定”，都回设置屏并清终端 token。
37. 启动时未登录也会发管理接口、可能被 401 顶回登录/设置屏（本轮顺带）——`loadRecords`/`loadCardBalance`/`configServer`/`loadApplications` 在无 token 时跳过，界面留 `---`。
38. 家庭组管理员判定读错数据源（WBC 复审打回，P2 回归）——原写法在 `GET /family` 顶层找 `role`／`member_no`，而顶层只有 `family_group_id`／`status`／`admin_member_no`／`members`，导致 `famIsAdmin` 恒 false、管理员按钮不渲染。改为登录/注册成功时缓存 `role`／`member_no`，退出登录、管理口 401、手机验证码重置管理密码（重置成功即退出登录）三处清空缓存；进家庭组时按“有 role 看 role，无 role 用 member_no 比对 admin_member_no”判定。
39. 预支上限 fallback 成 0（复审 Optional）——`advanceLimit` 与 `dailyTotal` 都为 null 时不再按 0 本地拦，`limit` 置 null、跳过本地上限判断，交给服务端 A5 裁决（服务端返回真实上限/错误码）。
40. `loadCardRules()` 未登录也发 B13（复审 Optional）——补 `if (!authToken)` 守卫，与同批其他 loader 一致，无 token 直接 `renderCardBalance()` 返回。
41. 服务端骨架信封与文档不一致（本地联调发现）——骨架报错是 `{"error":{"code","message"}}`、成功时直接返回数据对象（无 `code`），原 `request()` 只判 `code===0`，会把骨架的成功响应一律当失败。改为先取 `error.code/message`，成功响应无 `code` 也按成功取数据（有 `data` 取 `data`，否则整包当数据）。
42. 注册后没 token 就进管理口（本地联调发现）——骨架 v3 注册响应带 token；若换到不带 token 的实现，`api.register` 会自动用同一手机号/密码补一次登录再返回，避免注册后立刻 401。
43. 登录/注册后 home 余额一直 `---`（本地联调发现）——登录成功、注册成功点「下一步」时各补一次 `loadCardBalance()` + `loadApplications()`，进管理前余额已是服务端实数。
44. 进卡管理只拉 B13/B2、不拉 B11（本地联调发现）——`goTo(12)` 补 `loadRecords()`，使用记录三项进屏即按服务端 7 日汇总渲染，不再停在 `---`。
45. A1 上传成功后卡在取景器、不进处理屏也不轮询（本轮冒烟发现）——`handleSolveBlob` 里残留一句 `resetSolve();`，全文没有这个函数，`.then` 里抛 `ReferenceError` 被 `.catch` 当接口错误吞掉。删掉该调用，改为直接 `goTo(3)` + `startWaiting()`。
46. 取景器提示走 `alert`（本轮冒烟发现）——`showCaptureFeedback` 找 `#finderFeedback`，但第 3 屏没有这个元素，所有提示（没看清、设备未绑定、照片太大等）都落到 `window.alert`。补一个 `.pin-toast` 浮条 `#finderFeedback`，与其它屏一致，几秒自动消失、不占版面。
47. 手动绑定终端没验证过（本轮补测）——屏 14「输入设备号」填 App 自己算出的 16 位设备号，B15 返回 201 并刷新列表；随后 A1 的 `X-Terminal-Id` 正是这台已绑终端。
48. 信封归一误伤 2xx 的失败 job（abb 复审 P1）——`request()` 原先把“顶层带对象 error”一律当错误信封，而 v4 失败 job 的 200 轮询视图本身带 `error:{code:'no_question'|'model_not_connected'|...}`，于是被 THROW 成接口错误，`poll()` 里 `status:'failed'` + `failure_reason` 的分支永远走不到：空白题显示服务端长句而不是已定的「这张里没有题目」。改为**只有 `!resp.ok` 时**才把顶层 error 当错误信封，2xx 的 job 视图按数据交给调用方分流。复跑实测：`model_unavailable` 现在显示「判题服务暂时接不通」。

本轮复测（Playwright/Edge headless，1280×720）：16 屏 `pageerror` 0 条；解题卡三处余额、每日张数、公式结果均为 `---`，13 屏两个输入框为空；三组统计 `---`；家庭组标题显示“家庭组ID ---”、成员行显示 `---`，点「邀请成员」真实发出 `POST /family/invite`（`file://` 下跨域失败并如实提示）、不生成邀请码；用量页五行官方模型均 `---`、合计 `---`；已绑定终端 0 行。

复审回归复测（第 38–40 条，拦截接口伪造返回，走真实 UI 路径）：

| 场景 | 期望 | 实测 |
|---|---|---|
| 登录返回 `role:'管理员'` | 显示管理员操作 | 2 行、按钮 `删除(disabled)` / `转让` / `删除` |
| 登录返回 `role:'admin'` | 同上 | 同上 |
| 登录无 `role`、`member_no=5`、`admin_member_no=5` | 判为管理员 | 同上 |
| 登录无 `role`、`member_no=6` | 判非管理员 | 2 行、0 个操作按钮 |
| 登录 `role:'成员'`、`member_no=5` | 判非管理员 | 2 行、0 个操作按钮 |
| 登录既无 `role` 也无 `member_no` | 判非管理员 | 2 行、0 个操作按钮 |
| 未拿到 A3/B13 点「预支」 | 不被本地 0 上限拦住，真实发 A5 | 发出 `POST /cards/advance`，提示“解题卡已领取，请合理使用。” |
| 未登录进 13 屏 | 不发 B13 | 只有启动的 `GET /config/environment`，公式 `---` |

以上八个场景 `pageerror` 均为 0 条。

## 5. 本地网关联调（第 41–48 条）

环境：本机起服务端骨架 v4.0.0（`platform-server-v4.zip`，MD5 `b698b18c5e9a7a287c86d729986e658e`，`python app.py`，`127.0.0.1:8000`，数据目录独立临时目录），App 用 `python -m http.server 8080` 起在 `127.0.0.1:8080`；Playwright/Edge headless 加 `--disable-web-security`（骨架不带 CORS 头）。路径全走可达 UI：home → 服务端管理把网关切到自有服务器并填 `127.0.0.1` → 回 home 管理口 → 注册（真实手机号占位 + 六位管理密码）→ 管理密码 → 卡管理/账户管理/终端管理/服务端管理 → 终端管理手动绑定 → 回 home 拍题。

| 步骤 | 请求 | 实测 |
|---|---|---|
| 启动 | `GET /config/environment` | 官方地址本机不可达（503），属环境限制，不影响后面走自有网关 |
| 切网关（未登录） | `POST /config/server` | 401 `unauthorized`，界面不报错，地址已切到本机 |
| 注册 | `POST /auth/register` | 201，返回 `family_id`、`member_no:1`、`role:"管理员"`、`token` |
| 注册后立即刷新 | `POST /cards/check` | 200 `daily_cards:5, advance_ratio:0, balance:5, total_available:5`；home 显示 `5 / 5 张` |
| 注册后立即刷新 | `GET /applications?status=pending` | 403 `admin_password_required`（未验管理密码，符合预期） |
| 管理密码 | `POST /auth/admin-password/verify` | 200 `verified:true` |
| 卡管理 | `GET /cards/rules` | 200 同 A3 口径，13 屏每日张数 `5`、预支倍率 `0`、公式 `= 5 张` |
| 卡管理 | `GET /applications?status=pending` | 200 `items:[]`，界面「暂无申请」 |
| 卡管理 | `GET /records?range=7d` | 200 `summary` 全 0，使用记录三项 `0（次）/0（道）/0（张）` |
| 账户管理 | `GET /family` | 200 `family_group_id:33417426`、`admin_member_no:1`、成员 1 为管理员，删除按钮可见 |
| 终端管理 | `GET /terminals` | 200 `items:[]`，界面 0 行（空列表不编设备号） |
| 手动绑定终端 | `POST /terminals/bind` | 201 `terminal_no:1`、`bind_method:"manual"`、设备号回显 `1188 7825 8423 7543`；浮条“设备 … 已绑定。”，列表随即刷新出 1 行 |
| 拍题上传 A1 | `POST /solve`（真实 1×1 JPEG，`X-Terminal-Id` 为上面已绑设备） | 202 `job_id`、`stages.uploading/gateway=done`、`model=processing`，App 进第 4 屏并开始轮询 |
| 拍题轮询 A2 | `GET /solve/{job_id}` | 200 `status:"failed"`、`model:"failed"`、`failure_reason:"model_unavailable"`（本机未配 `QWEN_API_KEY`）；App 回第 3 屏并浮条显示已定文案「判题服务暂时接不通」，不伪造成功、不显示服务端长句（第 48 条修完复跑） |
| 服务端管理 | — | 网关设置/模型管理正常渲染 |

整条链路 `pageerror` 0 条；`console.error` 只有上述 503/401/403 三条，均为预期状态码，不是脚本异常。

联调中发现的**服务端（v3 / v4.0.0 同样）问题**（不是 App 问题，App 未改服务端）：报错早退路径（401/403）没读请求体，keep-alive 连接上残留 body 会把下一个请求顶掉，服务端回 `501 Unsupported method ('{"custom_ip":...}POST')`，浏览器复用连接时注册必然失败。v4 包复现脚本同样得到 `501`。本地测试副本已临时补“发响应前把未读 body 读掉”一行洞（仅测试目录，不在交付内）；正式服务端需确认是否已修。

## 6. 未覆盖

- 线上官方服务器：本机出站被挡（`solve.mhq-ai.xyz:8000` 连不上），未联调；已用本地 v4 网关跑通上述链路。
- 判题成功态（A2 返回 `done` 与题目/思路/结果）：本机网关未配 `QWEN_API_KEY`，只跑到 `failed`；成功态的渲染与原型比对见第 3 节，未接真实模型。
- 真机（vivo X60）：未上，按约定先在模拟环境比对。
- 原生相机与安装包：本版为 H5，不含原生壳。
