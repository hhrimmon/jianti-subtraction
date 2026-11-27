# 1149 三项绑定证据 + 1201 同页隔离测试 + 连续实拍（2026-11-27）

真机：vivo V2046A / 1578472099005SJ，2376x1080；官方服务端 http://solve.mhq-ai.xyz:8000。
本组证据全部为真机连续实拍，未使用任何 JS 钩子/注入（运行时插桩仅用于采集下述字段，已在最终版全部移除）。

## 1149 要的三项（登录接口响应，Debug Server HTTP 采集后移除）
1. **App 发的 deviceId**：`2568298890326209`（App 内 makeDeviceId() 按 UA+本地随机种子生成，首位 2=安卓；
   与设置页二维码展示号、板子真号 2040 属不同用途的号，登录头 `X-Terminal-Id` 用此号）。
2. **绑到哪个 family**：`familyId = 65478612`（测试账号 13800000003 所属家庭组）。
3. **自动绑定有没有发生**：**发生了且成功**。全新安装（pm clear 后重新生成 deviceId）登录响应
   `terminal_auto_bind.status = bound`、`terminal_no = 2`、`message = 已将终端绑定到该家庭组`。
   （对照组：旧测试号 2864190752386419 返回 `bound_elsewhere`——该号被别的家庭组占用，属环境数据问题；
   用 App 自己新生成的 deviceId 即自动绑定成功，App 侧无需改绑定逻辑。）

## 1137 尾巴（1197 要求换干净号跑通整条）
全新 deviceId 自动绑定成功后，整条判题链路真机跑通：
- 未登录点对号 → 登录页（设计行为，"请先登录，登录后再发送"）；
- 登录成功 → 自动绑定 bound → **当帧进第4屏处理页**（点对号即 goTo(3)，不等扫码/网络）；
- `/solve` 返回 job（`job=True`）→ 五阶段动画正常走完（stage 1→5，无卡死）；
- 轮询 `processing` → `done` → **第5屏结果页**：真实 AI 思路（"共2题第X题 · 思路"、971"无对错·只给思路"口径、上一题/下一题）。
运行时事件证据（已采集后移除插桩）：`processJpegBytes.entry` → `handleSolveBlob.then job=True` →
`poll.status processing×N` → `poll.status done`；全程无 `stageTimer.blocked`、无 401、无 catch 错误。

## 1201 同页隔离测试（浩爷令：手机拍板子拍过的同一页教材）
- 提交图：`c3_frozen.png`（拍摄定格帧，"已拍好，点对号发送"）；
- 处理页：`c4_processing.png`（上传✓网关✓大模型/NLP/下发 五阶段）；
- 结果页原样：`c5a.png`（第2题：代数式化简求值思路）、`c6_q1.png`（第3题：定义新运算 a*b=a(a-b)+1 思路）、
  `h7.png`（首跑第2题：代数式化简 x=1/2,y=1 思路）。
- **结论：模型返回的是真题（识别出多题并逐题给思路），不是假题、不是 no_question。按 1201 判据——
  手机对了，问题在板子侧（摄像头/提交链路），不在模型/网关。**

## 连续实拍序列（无 hook）
`c1_home.png`（home，服务器已连接、已登录）→ `c2_finder.png`（取景器实时画面）→
`c3_frozen.png`（快门定格）→ `c4_processing.png`（点对号当帧进处理页）→
`c5a.png`/`c6_q1.png`（结果页题目切换）。

## SHA256
- 9b757dd9996cae8023c7f2808995b062f884f2ecbe4300bebc8acb1e41e43471  c1_home.png
- 91b5090b5b5a5263ea33bec2f4ae37d5330c1dff7b6c26dd1f1f2aa580c98059  c2_finder.png
- 30ea3084a44769cfbec8705392d64a5eb33c3e3a01c00eb0526b10a903e1729c  c3_frozen.png
- 49d2f680ab152020f20ce1675ab012647fc795fbbd9dbf887c522572932151a0  c4_processing.png
- b79746da6cc331d124cecd6280878653a3f1041349b81838751b5eca0d9a61dc  c5a.png
- 13cc25c453d3f85971f9da434bb6337fae488fab2006612eb7db4ce153cc4af6  c6_q1.png
- ba661be3368bd1ad269588fca698a602ab4283a355f819d649b0fa944051ff17  h7.png
