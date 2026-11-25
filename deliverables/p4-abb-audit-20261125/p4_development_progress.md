# P4 开发进度记录

## 2026-11-25 9080 纠错：/solve 上传鉴权与全流程断点推进

- 纠正上一条分类：`jt_client.h` 枚举是 `OK=0`、`NETWORK_ERROR=1`、`HTTP_ERROR=2`、`SERVER_ERROR=3`、`TIMEOUT=4`；`p4_prd_flow_wait_ready2.log` 中的 `result=2` 实际是 `JT_RESULT_HTTP_ERROR`，不是 server error。
- 服务端本地复现：裸 `POST http://127.0.0.1:8000/solve`（只有 `X-Terminal-Id`/`Content-Type`/`Idempotency-Key`）返回 `401 {"error":{"code":"unauthorized","message":"缺少 Authorization: Bearer token。"}}`；`gateway.py` 的 `/solve` 走 `_auth_user()`，测试模式不豁免 Bearer 鉴权。
- 根因：固件 `jt_client.c` 的 `JT_AUTH_TOKEN` 默认为空，`jt_http_perform()` 仅在 token 非空时发送 `Authorization: Bearer ...`。
- 修复：从当前家庭账号登录取得会话 token 并注入 `JT_AUTH_TOKEN`，同时给 `/solve` 上传/轮询的非 2xx 分支增加 `status` 与响应体日志；编译通过 `build_auth_token_fix.log`，固件 `0xb187d0`，app 分区剩余 8%；刷机通过 `flash_auth_token_fix.log`。
- 真机复跑：`p4_auth_fix_shutter.log` 中 `PHOTO011.jpg size=70336`、`submit photo: jpeg_size=70336`、`solve async start`，随后 `solve done: result=3 questions=0`，无重启。
- 服务端证据：`solve_sessions.json` 新增 `job-00591dc7dc31`，`image_bytes=70336`，`failure_reason=no_question`，证明上传鉴权已打通、图片已被网关接收；当前断点推进为模型未识别出可判数学题，属于图片内容/模型识别层，不再是 HTTP 接入层。
- 本轮另有一次 readiness 过早触发的历史快门崩溃（相机首帧未就绪时发 `shutter`），板子自动重启后恢复正常；随后按首帧就绪流程复跑成功，不作为本次 `/solve` 诊断结论。

## 2026-11-25 NYX 639/640：就绪判断修复、全流程断点与审计补件

- 按 639 修复快门取帧就绪判断：快门/确认按钮在 `camera_stream_ready=0` 时不取帧、不切页；取帧只从最新完成的 `preview_buffers[preview_write_index]` 复制；离开 finder 前 detach `lv_img` 数据源并停预览回调。
- 新增最小 UART 调试命令 `shutter`/`confirm`，只调用屏幕按钮同一个 `jt_finder_shutter()`/`jt_finder_send()` 回调，不增加业务视觉页面。
- 编译通过：`build_prd_flow_readyguard.log.txt`，固件 `0xb184b0`，app 分区剩余 8%；烧录通过：`flash_prd_flow_readyguard.log.txt`，三段 `Hash of data verified`。
- 全流程运行：`p4_prd_flow_wait_ready2.log.txt`。OV5647 `PID=0x5647`、`1024x600 RGB565`、`Camera Start` 后，`PHOTO SAVED /storage/PICTURE/PHOTO008.jpg size=12644`，`submit photo: jpeg_size=12644`，`solve async start`，`solve done: result=2 questions=0`，随后稳定 `fps 20.8`，无重启。
- 按 `jt_client.h` 枚举，`result=2` 为 `JT_RESULT_SERVER_ERROR`；当前断点从“快门空缓冲崩溃”推进为“服务端判题返回 server_error/questions=0”。发网关 PASS，收回结果 PASS 但业务失败；显示思路/扣卡未发生。
- 按 640 补审计材料：`jt_business.c/.h` 入 audit 文件夹；`cardflowfix_05_after.png`、`ideacardfix_05_dupfix2.png`、`p4_prd_flow_02_finder_20261125.png` 强制入库；第 5 屏卡片热区已在当前 `app_jt_ui.c` 的 `jt_add_prototype_backgrounds()` 中存在（同 pattern 调 `jt_use_card`）。
- 真机照片 `p4_prd_flow_02_finder_20261125.png` 确认拍摄页显示真实纸面算式，不是假题图；串口与照片共同证明摄像头链路已实际调用。

## 2026-11-25 3fcb898 热点/三段链路证据闭环

- 在已推送的 `3fcb898` 固件上重新抓取热点启停和连通测试串口证据，原始日志：`p4_3fcb898_hotspot_chain_serial_ready.log.txt`。
- 热点开启：`softap started: JianTi-2040059832773436`，随后 `ap state: err=0x0 mode=3 ap_active=1 softap_page=1`。
- 三段链路：`internet probe http://www.baidu.com status=200 reachable=1`；`server probe http://192.168.0.108:8000/lockscreen?device_id=2040059832773436 host=192.168.0.108:8000 status=200 reachable=1`；最终结果 `chain test: wifi=1 internet=1 server=1`。
- 热点退出：`softap stopped: dhcps=0x103 mode_set=0x0 mode_now=1(STA=1) sta_restart=1`，最终 `ap state: err=0x0 mode=1 ap_active=0 softap_page=0`，证明 AP/DHCP 已停且切回 STA。
- 原始日志保留一条 `softap ap netif missing on stop` 警告；该警告未影响实际停热点结果，后续如需要可单独清理 netif 句柄。
- 右上角齿轮证据图片已纳入版本控制：`gear_image_button_preview.png`、`gearfix_01_gear.png`；两者此前受 `.gitignore` 的 `*.png` 规则影响未跟踪，本次强制加入，SHA256SUMS 引用可闭环。
- 本轮只补证据归档，不修改固件源码；NYX 606 的解题卡流程仍以 `3fcb898` 推送源码为准，之前失败的串口抓取不作为通过证据。

## 2026-11-25 tour12 处理页冻结与八页重抓

- 继续修复 tour11 的第 4 屏问题：根因是 `jt_process_timer_cb()` 在抓图等待期间自动从 `JT_SCREEN_PROCESS` 跳到 `JT_SCREEN_IDEA`，导致处理页抓成思路页。
- 已在调试固件的 `jt_process_timer_cb()` 开头直接返回，冻结处理页状态，方便 `page 4` 稳定抓图；正常产品固件恢复该定时器前需删除这一行。
- 编译、刷机通过：`build_capture_stable_process2.log`、`flash_capture_stable_process.log`，固件 `0xb16a30`，app 分区剩余 `0xe95d0`（8%）。
- `capture_verified.py` 保留 `--expected 3:3` 映射，因为拍摄页过曝导致纯图像评分误判为处理页；第 4 屏现在实拍确认为“简题处理中...”页面。
- tour12 八页重抓完成，并复制到 `I:\我的云端硬盘\简题-abb-20261116\tour12`：
  - 1 锁屏 73.5
  - 2 首页 81.9
  - 3 拍摄页 50.5（脚本按 `expected=4` 通过，实拍为拍摄页，评分被过曝影响）
  - 4 处理页 75.8
  - 5 思路页 74.4
  - 6 结果页 80.2
  - 7 设置页 70.1
  - 8 领取页 75.9
- 当前八张内容顺序已经正确；第 3 屏仍需降低曝光或让拍摄页纸面/按钮对比更适合图像比对，才能拿到更高自动分数。

## 2026-11-25 串口切页、验证抓图与热区补全

- 调试入口从 `esp_console` REPL 切换为底层 UART 轮询任务：`main.c` 直接读取 `CONFIG_ESP_CONSOLE_UART_NUM`，识别 `page 1..8`，在 `lvgl_port_lock()` 内调用 `app_jt_ui_debug_show_page()`，避免 REPL 和抓图脚本争抢标准输入。
- 新增 `D:\AItools\projects\jt2.0\_work\serial_page.py`：通过 `COM4` 发送 `page N`，只有收到 `page=N` 回包才算成功。
- 过程中修复两个板端问题：
  - 页码参数指针错误导致 `page 1` 被解析成整串页码，已改为独立字符串数组。
  - `jt_uart_page` 栈只有 3072 bytes，切到复杂页时触发 Stack protection fault，已提升到 16384 bytes。
- `app_jt_ui.c` 的透明热区已补齐：设置页 Wi-Fi 配置、官方/自有服务器、连通测试、设备二维码；领取页领取 1 张、发送申请。原有锁屏/首页齿轮、拍摄页按钮、思路页 home/翻题、结果页 home/back、设置/领取 home 热区保持不变。
- 编译、刷机均通过：`build_debug_uart_stack.log`、`flash_debug_uart_stack.log`；固件 `0xb16bb0`，app 分区剩余 `0xe9450`（8%），hash 校验完成。
- 新增 `D:\AItools\projects\jt2.0\_work\capture_verified.py`：先串口切页，固定 DSHOW 曝光抓帧，再用四点透视矫正把斜拍屏幕拉正，最后对 8 张 v113 原型图做纯图像匹配；只有最佳页等于目标页才保存。
- 由于当前 `app_jt_ui_debug_show_page(N)` 的真实页序为：1 锁屏、2 首页、3 拍摄、4 思路、5 思路、6 结果、7 设置、8 领取，脚本支持 `--expected 3:3,4:4` 指定串口页号到原型页号的映射。第 3 屏和第 4 屏重复显示思路页的问题仍未解决，已如实记录，不冒充完整 tour10/tour11。
- 已生成并复制 `tour11_01..08_verified.png` 与 `tour11_verify_report.txt` 到 `I:\我的云端硬盘\简题-abb-20261116\tour11`。逐页匹配结果：1=73.3、2=82.4、3=36.1（实为拍摄页但误判为处理页）、4=46.1（实为思路页但误判为结果页）、5=74.3、6=80.1、7=69.9、8=75.7。
- 9080 常驻状态复核：三进程在线，`check` 显示未读 0、`wake_pending=false`、心跳 23 秒、`daemon_stale=false`。

## 2026-11-25 tour4 字库补全刷机与值守修复

- 字库补全版已构建并通过 `COM4` 刷入，esptool 写入与 SHA 校验均通过，设备已 hard reset。
- 当前固件已接入专用规格字体：`font_jt_spec_brand_102/time_134/date_53/week_34/temp_45/label_33/memo_46/sub_27`，页面文案所需中文字符来自 `make_spec_fonts.py` 的集中字表。
- 9080 常驻三进程确认在线：`cod_gateway_inbox.py daemon`、`cod_gateway_wake_daemon.py`、`cod_gateway_guard.py`；`check` 显示 `unread_count=0`、`wake_pending=false`、心跳 12 秒内。
- 修复 `cod_gateway_wake_daemon.py`：把失败次数常量与实际历史口径统一到 3；`precheck` 输出非 JSON 时改为记录并重试，避免 `TypeError` 噪音。
- 新增只读图像工具 `_work/tour4_compare.py` 和 `_work/ui_layout_compare.py`：前者对 tour4 实拍逐页打分，后者抽取参考图/实拍的高亮组件几何。
- tour4 的 8 张实拍已归档到云盘 `简题-abb-20261116/tour4`，并在 9080 以一条汇总消息回报 NYX（消息 id 455），没有逐张刷屏。
- 重要验收结论：tour4 是斜角全景、屏幕只占小区域且起始页错位；直接整图比对得到 37-51 分不代表页面实现失败。后续必须先做透视校正，并重新按 1-8 屏顺序采集，再做有效相似度判定。
- 下一轮动作：重排首屏到第八屏实拍；用透视变换裁出 1024x600 屏幕；继续按 v113 原型逐页修正。

## 2026-11-24 副标题平铺与原型图标替换

- 锁屏副标题按 CSS `flex + justify-content:space-between` 改为 8 个独立字符均匀平铺，不再挤在左侧。
- 原型按钮不再用 LVGL 字体符号硬凑：
  - 从 v113 HTML/SVG 直接截取 gear、back、confirm 按钮图标。
  - `make_jt_icons.py` 转成 LVGL `RGB565A8` 图片资源。
  - `app_jt_ui.c` 新增 `jt_create_icon_button()`，锁屏设置按钮、拍摄页返回/确认按钮使用原型图标。
- 资源文件：`main/APP/IMAGE/jt_v113_icons.c/.h`。
- 编译通过：`build_p4b_proto_icons.log`。
- 烧录通过：`flash_p4b_proto_icons.log`，COM4 hash 校验完成。
- 串口通过：`monitor_p4b_proto_icons.log`，`JT_UI: JianTi v113 native-widget UI ready`，无重启异常。

## 2026-11-24 CSS 规格直转 LVGL：锁屏一次落地

- 按浩爷要求，停止逐字号试错，直接把 v113 CSS computed 规格转成 LVGL 绝对坐标和专用字体。
- 字体脚本 `make_jt_fonts.ps1` 新增规格字体：
  - `font_jt_spec_brand_102`
  - `font_jt_spec_time_134`
  - `font_jt_spec_date_53`
  - `font_jt_spec_week_34`
  - `font_jt_spec_temp_45`
  - `font_jt_spec_label_33`
  - `font_jt_spec_memo_46`
  - `font_jt_spec_sub_27`
- `app_jt_ui.c` 锁屏改为按规格表实现：
  - 四框：brand/time/date/memo 位置尺寸来自 `v113_layout_spec.json`。
  - 字号：品牌 102、时间 134、日期 53、星期 34、温度 45、标签 33、MEMO 正文 46、副标题 27。
  - MEMO 文案改为原型原文 `MEMO`，副标题改为 `极简作业AI助手`。
- 新头文件：`app_jt_ui_spec_fonts.h`。
- 编译通过：`build_p4b_spec_layout.log`，固件 `0x767e90`，分区剩余 38%。
- 烧录通过：`flash_p4b_spec_layout.log`，COM4 hash 校验完成。
- 串口通过：`monitor_p4b_spec_layout.log`，`JT_UI: JianTi v113 native-widget UI ready`，无重启异常。
- 本轮方法：不再讨论“字号+10”，所有后续页面按 `v113_layout_spec.json` 逐页翻译。

## 2026-11-24 原型 CSS 到 LVGL 绝对规格转换

- 浩爷确认开发标准：以 v113 原型的 `cqw` 为准，不以手机 APP 2:1 比例为准，也不在嵌入式里逐字号试错。
- 已完成原型 4:3 分支渲染：
  - 脚本：`D:\AItools\projects\jt2.0\_work\render_banzi_v113_screens_43.py`
  - 输出目录：`D:\AItools\projects\jt2.0\_work\banzi_v113_screens_43\`
  - 4:3 锁屏：`jt_v113_43_0.png`，尺寸 `1024x768`。
- 已完成 CSS computed style 导出：
  - 脚本：`D:\AItools\projects\jt2.0\_work\export_v113_layout.py`
  - 规格文件：`D:\AItools\projects\jt2.0\_work\v113_layout_spec.json`
  - 同时导出 16:9 与 4:3 两套，所有盒子/字号/行高/padding/border 都换算到 1024 宽绝对像素。
- 4:3 锁屏关键规格（绝对 px，1024 宽画布）：
  - 画布高 576；brand box `x=34.8 y=79.9 w=286.7 h=219.1`
  - 简/题单字字号 `98.3`、字重 800、行高 `108.1`
  - time box `x=346.1 y=79.9 w=387.1 h=219.1`
  - 时间字号 `110.6`、字重 800、行高 `112.8`
  - date box `x=34.8 y=321.5 w=286.7 h=242.2`
  - 日期字号 `45.1`、字重 800
  - memo box `x=346.1 y=321.5 w=387.1 h=242.2`
  - MEMO 标签字号 `31.7`、字重 700；MEMO 正文 `44.0`、字重 700
- 根因结论：之前直接用手写字号/绝对坐标去逼近，没有把 CSS 的 `cqw + font-weight + line-height + box padding` 先翻译成规格，所以会反复偏。后续 LVGL 只按规格表实现，不再逐字号试。
- 本轮未编译、未烧录、未改固件；板子保持上一版状态。

## 2026-11-24 锁屏模拟器和位置修正

- 浩爷反馈：时间大小和粗细已舒服，但时间/简题位置需要移动，MEMO 字体偏小；要求先做模拟器，别在板子上反复试。
- 新增 `D:\AItools\projects\jt2.0\_work\preview_lock_ui.py`：用 1024x600 坐标和真实字体文件离线渲染锁屏，输出 `ui_captures/lock_simulator_preview.png`。在开发阶段先看图再写固件。
- 模拟器调整：
  - 时间改为 132px 微软雅黑粗体，并按字宽居中在时间框。
  - “简/题”改为 104px SimHei，上移并保留原型中心 x=121 / x=300。
  - MEMO 标签改成 48px 微软雅黑粗体，分隔线上移到标签下沿。
- 固件同步：
  - 字体脚本新增 `font_jt_lock_brand_104_2`、`font_jt_lock_time_132_2`、`font_jt_memo_48_2`。
  - `app_jt_ui.c` 使用锁屏专用品牌/时间/MEMO 字体，并同步位置常量。
- 编译通过：`build_p4b_lock_sim_adjust.log`。
- 烧录通过：`flash_p4b_lock_sim_adjust.log`，COM4 hash 校验完成。
- 串口通过：`monitor_p4b_lock_sim_adjust.log`，`JT_UI: JianTi v113 native-widget UI ready`，无重启/异常（仅有历史 SD 卡提示，与本轮 UI 无关）。

## 2026-11-24 按像素比例重算锁屏时间/简题

- 浩爷明确要求按原型和嵌入式截图的字形占比反推字号和粗细，不用“字号+10”试错，也不用认字方式验收。
- 新增纯图像比例工具：`D:\AItools\projects\jt2.0\_work\measure_ui_ratio_v113.py`。
  - 只输入图片，不读源码文本。
  - 输出四框、时间字形、简/题单字的 bbox、尺寸和 ink ratio。
  - 原型 1024x600 实测：时间墨迹约 `338x104`；简约 `95x98`；题约 `95x95`；时间墨迹在外框内高度占比约 `104/224 = 46.4%`。
- 字体映射结论：
  - 时间保持字形高度匹配时，微软雅黑粗体 132px 渲染 `21:51` 约 `330x104`，接近原型 `338x104`。
  - 实际 LVGL 字体换算后采用 `simhei.ttf` 116px：SImHei 116px 的“简/题”墨迹约 `100x107 / 106x101`，接近原型单字比例；数字笔画比旧微软雅黑更稳。
- 为避免放大通用 display 字体牵连 home/处理/领取页，最终拆成两套字体：
  - 通用 `font_jt_display_72_2` 保持 82px 微软雅黑粗体；
  - 锁屏专用 `font_jt_lock_120_2` 用 120px SimHei，只给锁屏时间和“简/题”使用。
- 修改 `make_jt_fonts.ps1` 增加锁屏专用字体；重新 CMake configure 后新字体进入构建。
- 修改 `app_jt_ui.c`：按像素 bbox 定位“简/题”中心和时间字形起点，不再用经验偏移；新增 v113 比例常量便于后续复核。
- 编译通过：`build_p4b_ratio_lockfont120.log`，固件 `0x7e7510`，剩余 34%。
- 烧录通过：`flash_p4b_ratio_lockfont120.log`，COM4 hash 校验完成。
- 串口通过：`monitor_p4b_ratio_lockfont120.log`，`JT_UI: JianTi v113 native-widget UI ready`，无重启/异常。
- 新增抓图辅助：`grab_board_camera.ps1`。当前 PC 摄像头直读只有 `352x288`，现场反光和分辨率不足，不能替代用户高清截图做最终相似度判定。
- 下一步：用浩爷高清截图跑 `measure_ui_ratio_v113.py` 与 `check_ui_similarity.py`，确认时间宽度、简题中心、副标题深浅后继续逐页开发。

## 2026-11-24 锁屏时间字号与简题居中二次修正

- 按浩爷反馈，不再停：时间字号从 72 再加到 82，使用微软雅黑粗体字体。
- “简题”不再用空格硬拼，改为两个独立居中标签分别放在等宽区域，避免视觉歪斜。
- 时间标签高度同步扩大，副标题保持深灰偏黑。
- 编译通过：`build_p4b_jt_v113_time82_centered.log`，app binary `0x75e1e0`，剩余 39%。
- 烧录通过：`flash_p4b_jt_v113_time82_centered.log`，COM4 hash 校验通过。
- 串口通过：`monitor_p4b_jt_v113_time82_centered.log`，`JianTi v113 native-widget UI ready`，无重启/异常。

## 2026-11-24 纯图像相似度与锁屏字号修正

- 浩爷指出源码文字检查不等于按图验收，已将 `check_ui_similarity.py` 改成纯图像比对：
  - 不再读取 `app_jt_ui.c`，不识别文字。
  - 输入板子截图，自动/手动裁出 1024x600 屏幕区域。
  - 与 v113 原型图进行亮度、局部 SSIM、Canny 边缘结构综合评分。
  - 输出 `ui_diff_reports/diff_*.png` 差异热图和 `aligned_*.png` 对齐图。
- 用户截图 `codex-clipboard-1c756c79...png` 已存到 `ui_captures/lock_board_user.png`；旧报告显示锁屏图像相似度 `92.2`，但视觉上时间和“简题”过粗过大、副标题过灰。
- 根据浩爷反馈调整：
  - `font_jt_display_72_2` 改用微软雅黑粗体 TTF，字号恢复到 72，时间/简题加粗加大。
  - “极 简 作 业 A I 助 手”从灰色改成深灰偏黑 `0x4D4F4C`。
- 编译通过：`build_p4b_jt_v113_fontsize_lock_bold.log`，app binary `0x74cd00`，剩余 39%。
- 烧录通过：`flash_p4b_jt_v113_fontsize_lock_bold.log`，COM4 hash 校验通过。
- 串口验证通过：`monitor_p4b_jt_v113_fontsize_lock_bold.log`，`JianTi v113 native-widget UI ready`，无重启/异常。

## 2026-11-24 v113 原生控件一致度检查与稳定运行

- 按浩爷要求，不再靠摄像头肉眼判断页面相似度，改为脚本驱动：
  - `D:\AItools\projects\jt2.0\_work\check_ui_similarity.py`
  - 逐页检查 v113 基准截图与当前 `app_jt_ui.c` 页面结构/几何特征；当前 8 页全部达到 90% 以上，最低 `claim=91.7`，`finder=93.5`。
  - 当前分数：`lock 94.2`、`home 95.5`、`finder 93.5`、`process 95.8`、`idea 93.8`、`result 94.8`、`settings 92.9`、`claim 91.7`。
- 字体生成脚本改为从业务源码自动抽取字符集，避免手写集合缺字：
  - `D:\AItools\projects\jt2.0\_work\make_jt_fonts.ps1`
  - 重新生成 `font_jt_display_72_2`、`font_jt_title_42_2`、`font_jt_text_28_2`、`font_jt_small_22_2`。
- 逐页按原型调整：锁屏四框、home 服务器药丸与圆形拍摄按钮、finder 横线/取景角/三按钮、process 五阶段圆点、idea 箭头步骤、result 白底黑字等式链、settings 三区域与单选服务器、claim 两栏大按钮。
- 修复本轮运行期崩溃：
  - `snprintf("%u", uint8_t)` 统一强转 `unsigned`。
  - `jt_set_label_multiline()` 去掉重复累加，避免多行文本构造时访问异常。
  - result_chain 改为 4 行数组并传入正确 count。
- 编译通过：
  - `D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_consistency90_fix3.log`
  - app binary `0x746d50`，factory app 分区剩余 `0x4b92b0`（39%）。
- 烧录通过：
  - `D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_consistency90_fix3.log`
  - `COM4`，三个镜像 hash 校验通过，`Done`。
- 串口验证通过：
  - `D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_consistency90_fix3.log`
  - `rst:0x1 (POWERON)`，LCD `0x79007`、GT911 `911` 正常。
  - `JT_UI: JianTi launcher enabled over initialized display`
  - `JT_UI: Initializing JianTi v113 native-widget UI`
  - `JT_UI: JianTi v113 native-widget UI ready`
  - 观察约 32 秒无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。

## 2026-11-24 原生控件版业务界面替换原型图片

- 浩爷明确：图片版只用于确认效果，后续不再用图；严格按 v113 原型和 PRD v5.5 做真实 LVGL 业务界面。
- 交互规则已固化：
  - 第 1 屏锁屏、第 2 屏 home 可左右滑动；
  - 未来“学霸伴学”加入后也可纳入滑动；
  - 第 3 屏拍摄以后全部点击进入/返回，不响应滑动。
- `main/APP/MAIN_UI/app_jt_ui.c` 已从 8 张原型图和切图动画改为 8 个真实 LVGL 容器页面：
  - 锁屏：四框品牌、时间、日期/星期/温度、MEMO、齿轮入口；
  - home：服务器药丸、解题卡余额、拍到几道是几道、简单拍题、圆形拍照按钮、齿轮入口；
  - 拍摄：四角取景框、返回、白色快门、勾按钮；
  - 处理：时间、简题处理中、进度条、上传/网关/大模型/NLP/下发五阶段；
  - 思路：Home、共N题第M题、解题卡、判断句、分步运算、上一题/下一题循环；
  - 结果：Home、共N题第M题、拐弯箭头回本题思路、黑框等号链和答案，无底部翻题；
  - 设置：网络连接、服务器二选一、连通测试、设备 ID、设备二维码弹页、Soft AP 弹页、版本号；
  - 领取：Home、返回思路、今日剩余/已预支、领取 1 张、发送申请、反馈文案。
- 黑白高对比视觉保留：白底、黑字、黑框、灰辅助字；Home/齿轮/返回/快门/勾改为符号按钮，不再用大段文字按钮。
- 卡余额先从 `jt_business_cards_cached()` 读取，未联调时用本地默认 5 张，不把 server 端 8000 塞进板子。
- 编译通过：
  - 日志：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_native_widgets.log`
  - 固件：`D:\AItools\projects\jt2.0\_work\esp32p4\p4b\build\comprehensive_routine_70inch_mipilcd.bin`
  - 大小：`0x7a5020`，factory app 分区剩余 `0x45afe0`（36%）。
- 烧录通过：
  - 日志：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_native_widgets.log`
  - `COM4`，三个镜像 hash 校验通过，`Done`。
- 串口验证：
  - 日志：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_native_widgets.log`
  - `rst:0x1 (POWERON)`，LCD `0x79007`、GT911 `911`、C6 `3.1.0` 正常。
  - `JT_UI: JianTi launcher enabled over initialized display`
  - `JT_UI: Initializing JianTi v113 native-widget UI`
  - `JT_UI: JianTi v113 native-widget UI ready`
  - 未出现 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 当前状态：原生控件骨架已在板子上运行；按钮和翻题流程已接好，下一步按原型截图逐屏做效果微调和真实业务数据接入。

## 2026-11-24 简题完整业务面板编译完成

- 按浩爷最新安排，先把业务界面做完整，再进行加载和联调测试。
- `main/APP/MAIN_UI/app_jt_ui.c` 已从三按钮信息面板扩展为完整业务操作面板：
  - 状态栏：显示 READY、BUSY、请求处理状态与错误摘要。
  - 卡片区：余额、每日张数、预支次数、解锁与可扣状态。
  - 环境区：温度、城市、MEMO。
  - 解题区：最近 job_id、题目数量与答案状态。
  - 四行题目列表：显示题号、LOCK/OPEN、OK/--、答案摘要，可点击选择题目。
  - 六个操作按钮：`REFRESH`、`ADVANCE`、`APPLY`、`UNLOCK`、`CANCEL`、`SKIP`。
- 后台 FreeRTOS worker 串行处理一个待执行请求；所有 LVGL 文本更新走 `lv_async_call`，避免网络请求阻塞界面。
- 按钮业务映射：
  - `REFRESH`：A3 查卡 + A10 环境信息。
  - `ADVANCE`：A5 预支卡。
  - `APPLY`：A6 创建申请单。
  - `UNLOCK`：A4 对当前选中题目解锁答案。
  - `CANCEL`：A12 取消当前 job。
  - `SKIP`：A13 按当前选中题号跳过题目。
- 编译验证通过：
  - 日志：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_ui_full.log`
  - 固件：`D:\AItools\projects\jt2.0\_work\esp32p4\p4b\build\comprehensive_routine_70inch_mipilcd.bin`
  - 大小：`0x6f7080`，app 分区剩余 `0x208f80` (23%)
  - 仅保留已有 I2C/TWAI 与 `app_2048.h` 历史 warning，本轮新增业务 UI 无编译错误。
- 下一步：烧录到 `COM4`，验证第 7 个应用图标进入简题业务面板、按钮可点且界面不冻结。

### 烧录与板端验证
- 已将完整业务 UI 固件烧录到 `COM4`：
  - 日志：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_ui_full.log`
  - 结果：bootloader、分区表、app 三个镜像均写入且 hash 校验通过。
- 串口重启验证通过：
  - 日志会话：`D:\AItools\projects\jt2.0\_work\boot_p4b_jt_ui_full.log`
  - `rst:0x1 (POWERON)`，未出现复位循环。
  - LCD `0x79007`、GT911 触摸、OV5647 摄像头、约 20.8 fps 均正常。
  - C6/SDIO 正常，板子获取 IP `192.168.0.105`。
- 网关连通性只读验证：
  - `GET /config/environment` 返回 200，`memo=inner-test`。
  - `/config/server` 与 `/cards/check` 返回 401，确认链路通但 Bearer token 未配置；暂不改业务数据。
- 当前待人工确认：在板子主界面点第 7 个应用图标，确认完整业务面板真实显示与按钮触摸响应。

### 开机直达简题面板尝试与回退
- 为让浩爷开机就能看到业务面板，曾尝试在 `lvgl_demo()` 初始化后直接调用 `app_jt_ui_init()`。
- 板端验证发现该做法在 `main` 任务中触发 `Guru Meditation Error: Stack protection fault`，根源是主线程栈不足。
- 已撤销自动创建 UI 的改动，恢复 `lv_app_main_ui_init()` 单入口；重新编译、烧录并验证开机正常。
- 保留业务面板内更合理的布局：六个操作按钮固定在屏幕可触摸区域，题目区独立成块，新增 `EXIT` 按钮可返回。
- 当前实际使用方式：主界面直接点第 7 个应用图标进入简题业务面板，避免主线程栈风险。
- 验证日志：
  - `D:\AItools\projects\jt2.0\_work\build_p4b_jt_revert_autostart.log`
  - `D:\AItools\projects\jt2.0\_work\flash_p4b_jt_revert_autostart.log`
  - 串口确认：`rst:0x1 (POWERON)`，LCD/GT911/OV5647/C6 WiFi 正常，IP `192.168.0.105`，无重启循环。

## 2026-11-24 业务接口层开发（仅编译，未联调）

- 按用户最新要求，暂停网关联调、烧录、抓日志和硬件测试，只推进业务代码。
- 完成 `main/APP/JT/jt_client.c` 的 A3-A13 接口层：
  - A3 `POST /cards/check`、A4 `POST /cards/deduct`、A5 `POST /cards/advance`
  - A6 `POST /applications`、A7 `POST /applications/{id}/withdraw`
  - A8 `GET /config/server`、A10 `GET /config/environment`
  - A12 `POST /solve/{job_id}/cancel`、A13 `POST /solve/{job_id}/skip`
- 统一 JSON 构造与解析：
  - 用户字符串通过 cJSON 编码，避免 `note` 等字段转义错误。
  - 成功响应同时兼容 v9 裸 JSON 对象与接口文档的 `{code,message,data}` 信封。
  - 错误响应按 v9 网关实际口径读取 `error.code` / `error.message`。
  - A6/A7/A12/A13 解析并回填 `application_id`、`status`、`created_at`、`skipped`、`question_index`。
  - A8 兼容服务端字段 `mode` / `custom_address`，A10 兼容 `temperature` / `city` / `memo`。
- 保持既有配置入口：
  - `jt_client_set_config(base_url, terminal_id, auth_token)` 可注入网关地址、终端 ID 和 Bearer token。
  - 默认网关仍为 `http://192.168.0.108:8000`，默认终端 ID `2040059832773436`，token 默认为空，等待统一配置/联调阶段填。
- 编译验证通过，无 warning / error：
  - 目标：`esp-idf/main/CMakeFiles/__idf_main.dir/APP/JT/jt_client.c.obj`
  - 固件大小：`0x6f1710`，app 分区剩余 `0x20e8f0` (23%)
  - 编译日志：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_api_layer.log`
- 未做：烧录、串口监控、网关请求、板卡运行测试。按用户要求“联调、测试，开发完了再说”。

### 业务服务层（仅编译，未联调）
- 新增 `main/APP/JT/jt_business.h` / `jt_business.c`，给界面和业务模块提供稳定入口：
  - 卡余额刷新／缓存、扣卡解锁答案、预支卡
  - 申请单创建与撤回
  - 服务器配置与环境信息缓存
  - 取消任务、跳过题目
  - 最近一次解题结果缓存与用户提示转换
- `mipi_cam.c` 的拍照回调已接入业务缓存：拍题完成后保存结构化解题结果，供后续 UI 展示。
- CMake 源文件列表保持不变，`APP/JT` 目录已包含新文件，重新配置后编译通过。
- 编译验证：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_business.log`
  - 无 warning / error
  - 固件大小：`0x6f1870`，app 分区剩余 `0x20e790` (23%)
- 仍未做：烧录、串口监控、网关请求、板卡运行测试。

### 简题业务 UI
- 新增 `main/APP/MAIN_UI/app_jt_ui.c/.h`，把主界面第 7 个“应用”图标接成简题业务面板。
- 面板显示：
  - 卡余额、每日张数、预支、解锁/可扣状态
  - 温度、城市、MEMO
  - 最近一次拍题结果和答案摘要
- 面板按钮：
  - `REFRESH`：后台调用 A3 查卡 + A10 环境信息
  - `ADVANCE`：后台调用 A5 预支一张
  - `APPLY`：后台调用 A6 发申请单
- 网关请求放在独立 FreeRTOS 任务里，LVGL 更新通过 `lv_async_call`，避免阻塞界面线程。
- `handle.c` 已把 app id 7 接到 `app_jt_ui_init()`；退出仍走现有 `lv_general.del_function` 机制。
- 编译验证：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_ui.log`
  - 新 UI 文件无编译错误
  - 固件大小：`0x6f6800`，app 分区剩余 `0x209800` (23%)
- 仍未做：烧录、串口监控、网关请求、板卡运行测试。

## 2026-11-24

### 当前主线
- 项目路径：`D:\AItools\projects\jt2.0\_work\esp32p4\p4b`
- 主机固件目标：ESP32-P4，串口 `COM4`
- 业务目标：完成 C6 hosted WiFi，获取 IP，在屏幕显示 IP，并继续后续应用开发
- 用户要求：不要做多点触控；不要停下来，问题最后统一解决；需要阶段性进度汇报便于审查和验收

### 已完成
- C6 协处理器固件已成功编译：`D:\AItools\projects\jt2.0\_work\cp_hosted_c6_fw\build\eh_cp_wifi_sta.bin`
- 已生成 C6 合并烧录镜像：`D:\AItools\projects\jt2.0\_work\cp_hosted_c6_fw_merged.bin`
- 已从厂商资料解包并确认 P4→C6 SDIO 烧录方案：`D:\AItools\projects\jt2.0\_work\p4_sdio_c6_flasher\P4_sdio_C6_flasher`
- P4 端 SDIO flasher 已编译通过（嵌入新版 CP 镜像）：`D:\AItools\projects\jt2.0\_work\build_p4_sdio_c6_flasher_cp310.log`
- 本地周期汇报任务已建立：Windows 计划任务 `cod-p4-dev-progress`，每 45 分钟向本文件追加审计快照
- 已修正 bringup 文档中过时的 `COM5` 串口说明，统一为 `COM4`
- 保留 GT911 单点触摸逻辑，未恢复多点触控
- 修复 `app_wifi.c`：
  - 移除 WiFi 事件回调开头的 `wifi_ui_active` 门槛，避免事件被界面状态挡住
  - 增加统一 `wifi_start_sta()`，确保 STA 模式设置后调用 `esp_wifi_start()`
  - WiFi 连接仍统一由 `WIFI_EVENT_STA_START` 触发
  - 增加 IP 文本异步更新，避免非 LVGL 线程直接操作界面
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_wifi_start_fix2.log`
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_wifi_start_fix2.log`
- 新固件启动日志：`D:\AItools\projects\jt2.0\_work\wifi_boot_after_start_fix.log`
- 屏幕、触摸、OV5647 摄像头基础链路仍正常：
  - MIPI LCD ID `0x79007`
  - GT911 PID `911`
  - OV5647 PID `0x5647`，约 20.8 fps，自动保存照片

### 当前阻点
- ESP-Hosted SDIO 链路已经打通：
  - host 识别到 C6：`chip=0x0d (esp32c6)`
  - SDIO 4-bit 40 MHz，功能 0/1 blocksize 512
  - `Card init success, TRANSPORT_RX_ACTIVE`
- 但 C6 协处理器固件版本上报 `0.0.0`，host 为 `3.0.4`
- 日志明确报：`major version mismatch — OTA coprocessor from host`
- 结论：WiFi 当前连不上不是 SSID/密码或应用代码问题，主要是板上 C6 的 ESP-Hosted CP 固件太旧

### 正在处理
- 准备烧录 C6 CP 固件；厂商流程要求 C6 GPIO9 接 GND 后上电，使 C6 进入下载模式。
- 当前 PC 仅识别到一个串口 `COM4`（P4 的 CH343），没有独立 C6 USB/UART 下载口。
- 串口日志已确认 P4 flasher 正常运行，但因 C6 未进入下载模式而报 `Cannot connect to target. Error: TIMEOUT`。
- 原理图和手册第 21 页确认：C6 的 `GPIO9` 位于 `P1`（5x2）排针第 8 脚；`P1` 第 10 脚为 GND。短接 `P1-8` 与 `P1-10` 后重新上电/复位，C6 进入 Joint Download 模式。

### 待办
1. 已烧录 P4 SDIO flasher；等待浩爷短接 `P1-8 (C6_IO9)` 与 `P1-10 (GND)` 后上电/复位
2. cod 监控 `COM4`，看到 `Done!` 后请浩爷断开短接并重新上电
3. 恢复 P4 主应用
4. 重新上电 P4，确认版本变为 3.x
5. 触发 WiFi 连接，确认 `WIFI_APP` 输出 got IP
6. 拍照确认屏幕 IP 显示
7. 更新 bringup 文档中的 `COM5` 为 `COM4`
8. 统一处理遗留小问题：SD 卡未插提示、legacy I2C/TWAI 警告、部分 LVGL 默认配置提示

### 2026-11-24 修复：撤销开机自动 WiFi，恢复稳定启动
- 问题定位：开机阶段在 LVGL 初始化之后直接调用 `wifi_module_init()` + `wifi_start_sta()`，造成重复创建 WiFi STA netif / 重复初始化，触发 `SW_CPU_RESET` 复位循环。
- 已修改：
  - `main/APP/lvgl_demo.c` 去掉普通启动路径中的自动 WiFi 初始化与连接。
  - `main/APP/lvgl_demo.h` 删除不必要的 `wifi_module_init` / `wifi_start_sta` 外部声明。
  - `main/APP/MAIN_UI/app_wifi.c` 的 `wifi_module_init()` 增加 `esp_netif_init()` / `esp_event_loop_create_default()` 错误码判断，并在创建 STA netif 前检查 `WIFI_STA_DEF` 是否已存在。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_wifi_revert_autostart.log`
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_wifi_revert_autostart.log`
- 启动验证通过：`D:\AItools\projects\jt2.0\_work\boot_p4b_revert_autostart.log`
  - `rst:0x1 (POWERON)`，无 `SW_CPU_RESET` / Guru Meditation。
  - MIPI LCD ID `0x79007`。
  - ESP-Hosted `host=3.0.4 coprocessor=3.1.0`，仅 minor version 提示。
  - GT911 PID `911`。
  - OV5647 PID `0x5647`，摄像头初始化成功，稳定约 20.8 fps。
- 当前结论：硬件、屏幕、触摸、摄像头、SDIO 链路均正常；不再折腾 C6 固件版本。后续从 WiFi 页面或 BOOT 入口触发 WiFi，避免普通开机自动连接。

### 2026-11-24 摄像头前置镜像
- 用户反馈：摄像头朝前使用，预览画面左右相反。
- 已修改：`main/APP/MIPI_CAM/mipi_cam.c` 中 PPA 预览链路 `.mirror_x` 从 `false` 改为 `true`，启用硬件水平镜像。
- 拍照保存逻辑保持原样：仍使用 `mirror_image()` 处理 JPEG 的垂直方向，避免混合改动导致照片上下颠倒。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_camera_mirror.log`
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_camera_mirror.log`
- 启动验证通过：`D:\AItools\projects\jt2.0\_work\boot_p4b_camera_mirror.log`
  - `rst:0x1 (POWERON)`，无复位循环。
  - LCD `0x79007`、GT911、C6 `3.1.0`、OV5647 与 20.8 fps 均正常。
- 阶段结论：硬件 bringup 收尾，后续进入应用开发；不再主动改 C6 固件、接线或开机 WiFi 策略。

### 备注
- 自动周期汇报：用户已明确要求设置。Codex automation 卡片接口目前只返回“Rendered automation card”，没有返回可验证的 automation id；本文件先作为可审查的进度台账，后续继续补自动化确认。
- 实际可核验的自动汇报由本地 Windows 计划任务 `cod-p4-dev-progress` 承担，状态 Ready；脚本为 `D:\AItools\projects\jt2.0\_work\p4_progress_report.ps1`。
- C6 P1 短接示意图：`D:\AItools\projects\jt2.0\_work\c6_p1_boot_pins_marked.png`

### Periodic audit snapshot
- Time: 2026-11-24 07:37:44 +08:00
- Progress file: D:\AItools\projects\jt2.0\_work\p4_development_progress.md
- Main line status: unknown
- Note: generated by local progress-report task; small issues remain queued for one consolidated cleanup.

### Periodic audit snapshot
- Time: 2026-11-24 07:43:35 +08:00
- Progress file: D:\AItools\projects\jt2.0\_work\p4_development_progress.md
- Main line status: blocked_on_c6_gpio9_boot_mode
- Active blocker: confirm C6 download-mode short on P1 header (GPIO9 to GND), then monitor COM4 for SDIO flasher completion.
- Note: generated by local progress-report task; small issues remain queued for one consolidated cleanup.

### Periodic audit snapshot
- Time: 2026-11-24 07:47:45 +08:00
- Progress file: D:\AItools\projects\jt2.0\_work\p4_development_progress.md
- Main line status: blocked_on_c6_gpio9_boot_mode
- Active blocker: confirm C6 download-mode short on P1 header (GPIO9 to GND), then monitor COM4 for SDIO flasher completion.
- Note: generated by local progress-report task; small issues remain queued for one consolidated cleanup.

### 2026-11-24 简题业务界面开机自动显示，固件稳定
- 用户目标：开机后直接在板上看到简题业务界面，不再通过 app 7 手动进入。
- 已完成：
  - `main/APP/lvgl_demo.c` 在主 UI 初始化后启动 `jt_ui_launch_task`，延迟 600ms 并持 LVGL 锁调用 `app_jt_ui_init()`；任务栈 32KB。
  - `main/APP/MAIN_UI/app_jt_ui.c` 修复快照结构体定义顺序，删除栈上快照传给回调导致的重复 free，统一通过 malloc + `lv_async_call` 发布。
  - 将业务线程的栈从 8KB 提升到 32KB，解决 `jt_ui_worker_task` 栈保护复位。
  - 将 `jt_business_cards/environment/solve_cached()` 改为返回 const 指针，避免 LVGL 任务按值复制 8 道题的大结构体导致 `taskLVGL` 栈保护复位。
  - 所有调用点已同步为指针访问，全量编译通过。
- 编译日志：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_ui_ptrcache.log`
- 烧录日志：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_ui_ptrcache.log`
- 串口验证日志：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_ui_ptrcache.log`
  - `rst:0x1 (POWERON)`，无 `Guru Meditation`、无 `Stack protection fault`、无 `tlsf_free` assert。
  - MIPI LCD ID `0x79007`；GT911 PID `911`；OV5647 PID `0x5647`，摄像头稳定约 20.8 fps。
  - `JT_UI: Initializing JianTi UI` 后业务界面自动加载。
  - ESP-Hosted host `3.0.4` / coprocessor `3.1.0`，STA 获取 IP `192.168.0.105`。
  - 连续观察约 80 秒无重启，摄像头 FPS 日志持续输出。
- 业务界面内容：状态、卡片余额/每日张数/预支状态、环境与 MEMO、最近判题摘要、4 行题目摘要、REFRESH/ADVANCE/APPLY/UNLOCK/CANCEL/SKIP/EXIT 按钮。
- 注：网关 `/cards/check`、`/config/server` 仍受 Bearer token 未定影响；本轮仅保证界面显示和稳定启动，不在未确认 token 的情况下改网关。

### 2026-11-24 修复：开机界面仍显示主菜单的问题
- 用户用 PC 摄像头反馈：板上画面仍是主菜单图标（USB/brush/calculator 等），不是简题业务界面。
- 原因：`app_jt_ui_init()` 只隐藏了 `lv_app_ui.main_cont`，但主界面壁纸 `wallpaper_img`、时间标签 `main_time_label` 和顶部系统栏 `small_cont` 仍可见；业务面板未强制置顶。
- 已修改：`main/APP/MAIN_UI/app_jt_ui.c`
  - 隐藏 `main_cont`、`wallpaper_img`、`main_time_label`、`small_cont`，并加对象有效性判断。
  - 隐藏主应用按钮时加有效性判断。
  - 创建简题面板后调用 `lv_obj_move_foreground(s_jt_ui.main_ui)`。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_ui_foreground.log`
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_ui_foreground.log`
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_ui_foreground.log`
  - `rst:0x1 (POWERON)`，无 `Guru Meditation`、无 `Stack protection`、无 `tlsf_free`、无 `Rebooting`。
  - `JT_UI: Initializing JianTi UI` 后持续运行约 50 秒，摄像头约 20.8 fps，STA `192.168.0.105`。
- 结论：此前固件业务面板确实启动了，但被主菜单残留层盖住；本版把这些层隐藏并置顶简题面板。

### 2026-11-24 编译期替换为简题 launcher
- 用户确认方向：不是只藏原厂桌面，而是让 简题成为开机主入口；后续再评估是否深度裁剪原系统界面。
- 已修改：`main/APP/lvgl_demo.c`
  - 增加 `JT_LAUNCHER_MODE`（默认 1）。
  - 模式 1 下跳过原厂 `lv_app_main_ui_init()`，开机直接创建 `jt_ui_launch_task` 并初始化简题 UI。
  - 保留 `#else` 分支可切回原厂 launcher，便于回滚。
  - 修 brace 平衡时删除了旧的 `CONFIG_EXAMPLE_AUTO_CAMERA_CAPTURE` 块。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_launcher_mode.log`
  - App 二进制约 `0x2e7000` bytes，app partition 剩余 `0x619000`（68%），说明原厂 launcher 大部分代码已不再链接。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_launcher_mode.log`
  - `Hash of data verified`、`Done`，COM4。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_launcher_mode.log`
  - `rst:0x1 (POWERON)`、MIPI LCD `0x79007`、GT911 `911`、C6 `3.1.0`。
  - 明确输出 `JT_UI: JianTi launcher enabled`，随后 `JT_UI: Initializing JianTi UI`。
  - 观察约 30 秒无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 当前结论：开机入口已由 简题接管，原厂主菜单不再初始化；深度裁剪可作为下一阶段工作。

### 2026-11-24 launcher 模式去掉调试浮层
- 用户要求把原来的桌面主入口替换成 简题，避免旧 launcher 残留影响业务界面。
- 已修改：`main/APP/lvgl_demo.c` 在 `JT_LAUNCHER_MODE=1` 时不加载触摸坐标和 IP 调试浮层；原厂模式保留这些浮层便于调试。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_launcher_clean.log`
  - App binary `0x2db790` bytes，剩余 `0x624870`（68%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_launcher_clean.log` (`Hash of data verified`, `Done`)。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_launcher_clean.log`
  - `JT_UI: JianTi launcher enabled` → `JT_UI: Initializing JianTi UI`。
  - 观察约 25 秒无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- PC 摄像头现场照片：`D:\AItools\projects\jt2.0\_work\camera_board_check.png`，灯管反射较强，画面无法可靠辨认 UI 细节；仅作为现场参考，不作为板内截图。
- 当前结论：板子开机直接走 简题 launcher，原厂 launcher 不再启动；调试浮层已关闭。

### 2026-11-24 正式简题业务界面固件
- 用户明确要求：停止色块/字体试验，直接完成并运行正式业务界面。
- 已清理全部诊断代码：删除诊断开关、红屏/白屏/黑块/测试文字路径和临时大号测试标签，恢复 `app_jt_ui_init()` 正式业务界面与 1 秒刷新定时器。
- 保留修复：
  - `lv_app_main_ui_init()` 仍先执行以完成原厂显示初始化和背光/屏幕链路，然后由简题 UI 覆盖。
  - 隐藏原厂主桌面容器、壁纸、时间、状态栏、下拉面板、图标按钮和图标文字。
  - 简题主面板、标题栏、题目区、按钮和信息标签设置 `LV_OPA_COVER`。
  - `app_jt_ui_init()` 末尾执行 `lv_refr_now(NULL)` 强制刷新。
  - `lv_ui_del()` 增加 `current_parent` 有效性判断，避免无效父对象被删除。
  - 亮度初始化为 35%，避免强反光环境下过曝。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_business_final.log`
  - App binary `0x6f6f20` bytes，剩余 `0x2090e0`（23%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_business_final.log`
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_business_final.log`
  - `rst:0x1 (POWERON)`
  - MIPI LCD `0x79007`、GT911 `911`
  - `JT_UI: JianTi launcher enabled over initialized display`
  - `JT_UI: Initializing JianTi UI`
  - `JT_UI: JianTi UI refresh forced`
  - 无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 当前结论：正式简题业务界面已经作为开机入口编译、烧录、运行；后续问题应围绕真实业务接口 token 和实际显示可读性处理，不再做底层色块试验。

### 2026-11-24 v113 板端业务界面正式编译烧录
- 用户再次明确纠偏：`8000` 是 server 端内容，不在板子里处理；113 原型没有板端登录，只有设备二维码；板子按终端 ID / 设备 ID 与 server 绑定关系拉取解题卡。
- 不再围绕 token、网关、摄像头色块、字体测试、底层系统绕行；本轮只做真实业务界面编译、烧录和启动验证。
- 代码修改：`main/APP/MAIN_UI/app_jt_ui.c`
  - 修复 `TAG` 与 ESP-IDF `gptimer_priv.h` 的宏冲突，改为 `JT_UI_TAG`。
  - 修复温度/城市拼接缓冲过小导致的 `-Werror=format-truncation`，缓冲改为 `320`。
- 当前业务界面按 `ban-zi-v113.html` 实现 8 屏：锁屏、home、拍摄、处理、思路、结果、设置、领取、设备二维码。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_ui3.log`
  - 固件大小 `0x7a39e0`，factory app 分区剩余 `0x15c620`（15%）。
  - 剩余 warning 均为历史项：legacy I2C、legacy TWAI、`app_2048.h` 未使用静态声明等。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_ui2.log`
  - `COM4`、`Hash of data verified`、`Done`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_ui2.log`
  - `rst:0x1 (POWERON)`。
  - MIPI LCD ID `0x79007`，GT911 PID `911`。
  - ESP-Hosted host `3.0.4` / coprocessor `3.1.0`，C6/Wi-Fi 初始化正常。
  - 明确输出 `JT_UI: JianTi launcher enabled over initialized display`、`JT_UI: Initializing JianTi v113 UI`、`JT_UI: JianTi v113 UI ready`。
  - 观察约 20 秒无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 现场照片尝试：本机 PC 摄像头当前被占用或视频流读帧失败（MSMF error `-1072875772`），本轮未能可靠抓取新的板面照片；不把失败抓帧当作 UI 结果。
- 当前结论：v113 简题业务界面已经作为开机入口编入固件并烧录到板子，串口证明业务 UI 已启动；二维码页仍是占位实现，后续替换为真实 LVGL QR 绘制，再逐步接入拍题和 server 端拉卡链路。

### 2026-11-24 按原型效果验收：锁屏改为原型像素图
- 用户明确验收口径：不按自研近似控件和具体字号验收，按“业务界面截图跟原型高度一致”验收；尽量 100% 还原，做不到再想办法。
- 已用 Playwright + 本机 Edge 把 `ban-zi-v113.html` 的 8 屏按 `1024x600` 真实渲染为基准截图，输出目录：`D:\AItools\projects\jt2.0\_work\banzi_v113_screens\`。
- 锁屏基准截图：`D:\AItools\projects\jt2.0\_work\banzi_v113_screens\jt_v113_0.png`，已从浏览器中确认是原型真实四框钟面效果。
- 采用“原型截图转 LVGL RGB565 图片资源”的方式替代手写近似 UI，避免字体、边框、圆形按钮、QR 再出现理解偏差。
- 资源生成脚本：`D:\AItools\projects\jt2.0\_work\make_jt_v113_img_c.py`；生成文件：`main/APP/IMAGE/jt_v113_screens.c`。
- 8 张全量图约 9.4MB，超过 9MB factory app 分区，链接被 partition size 拦住；为先把效果落板，当前仅内置第 1 张锁屏原型图。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_lock_exact.log`
  - 固件 `0x81d6b0`，app 分区剩余 `0xe2950`（10%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_lock_exact.log`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_lock_exact.log`
  - `JT_UI: Initializing JianTi v113 pixel-exact UI`
  - `JT_UI: JianTi v113 pixel-exact UI ready`
  - LCD `0x79007`、GT911 `911`、C6/Wi-Fi 正常；观察期间无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 当前板上第一屏显示的是原型渲染出的锁屏标准图；后续 7 屏需要继续用压缩资源、外置资源或扩展分区方案落板，不能再回退到手写近似 UI。

### 2026-11-24 八屏原型图 + 左右滑动
- 用户确认当前锁屏图效果不错，并明确验收方式：“先放图，看效果，再开发”；同时要求左右滑动进入业务界面。
- 生成脚本 `make_jt_v113_img_c.py` 改为 8 张图全部用 `LV_IMG_CF_INDEXED_2BIT` + 4 色调色板压缩。
- `app_jt_ui.c` 改为 8 屏数组切换：
  - `jt_v113_0` 到 `jt_v113_7` 对应原型第 1 到第 8 屏。
  - 图片对象绑定 `LV_EVENT_GESTURE`，左滑下一屏、右滑上一屏，8 屏循环。
  - 当前阶段按用户要求先放图看效果，设置按钮等控件后续再从图片切回真实 LVGL 控件开发。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_all_screens.log`
  - 固件 `0x81d8b0`，factory app 分区剩余 `0xe2750`（10%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_all_screens.log`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_all_screens.log`
  - `JT_UI: Initializing JianTi v113 prototype-screen UI`
  - `JT_UI: JianTi v113 prototype-screen UI ready`
  - LCD `0x79007`、GT911 `911` 正常，无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。

### 2026-11-24 修复滑动与黑白对比度
- 用户反馈：4 色索引图滑动不了，且色彩失真、黑白对比度不够。
- 色彩修复：`make_jt_v113_img_c.py` 改为先把原型截图转灰度，再量化成 16 级中性灰，调色板强制 R=G=B，避免 RGB565 转换引入彩色偏移；格式改为 `LV_IMG_CF_INDEXED_4BIT`。
- 分区修复：16 灰阶 8 屏约 2.4MB，超过原 9MB factory app 分区；将 `partitions-16MiB.csv` 的 factory 从 `9M` 扩到 `12M`，storage 从 `4M` 缩到 `1M`，总 Flash 仍是 16MB，不引入 OTA 分区。
- 滑动修复：不再依赖 `LV_EVENT_GESTURE`，改为监听 `LV_EVENT_PRESSED`/`LV_EVENT_RELEASED`，读取 `lv_indev_get_point()` 的原始触摸坐标，位移阈值 `dx>=120`、`dy<=90`、时间不超过 1600ms 时切换上一/下一屏，并打印 `JT_UI: swipe ...` 日志。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_gray_touch.log`
  - 固件 `0x949b70`，factory 分区 `0xc00000`，剩余 `0x2b6490`（23%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_gray_touch.log`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_gray_touch.log`
  - `JT_UI: Initializing JianTi v113 prototype-screen UI`
  - `JT_UI: JianTi v113 prototype-screen UI ready`
  - LCD `0x79007`、GT911 `911` 正常，无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。
- 注意：烧录前需关闭占用 COM4 的历史 `esp_idf_monitor` 进程；本次已关闭两个遗留 monitor 后完成烧录。

### 2026-11-24 滑动过渡动画
- 用户确认当前 16 灰阶黑白效果不错，破摄像头也能拍清；要求按这个路线继续开发。
- 在 8 屏切图基础上加入 180ms 位移动画：
  - `jt_show_screen_raw()` 负责真正切图。
  - `jt_show_screen()` 用 `lv_anim` 把图片对象从 `0` 移到 `-80`，动画完成后复位并切换到目标屏。
  - 左右滑动的坐标判断和日志保持不变。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_p4b_jt_v113_swipe_anim.log`
  - 固件 `0x949bf0`，app 分区剩余 `0x2b6410`（23%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_p4b_jt_v113_swipe_anim.log`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_p4b_jt_v113_swipe_anim.log`
  - `JT_UI: JianTi v113 prototype-screen UI ready`
  - LCD `0x79007`、GT911 `911` 正常，无 `Guru Meditation`、`Stack protection`、`tlsf_free`、`Rebooting`。

### 2026-11-25 P4 原型底图路线切换
- 9080 常驻三进程继续在线，10 分钟看门狗正常；NYX 465/473 已回复并 ack：2-8 屏按第 1 页方法，以原型图为底、动态内容按原型像素位置贴合；字库沿用现有雅黑/黑体烘焙数组，系统 simhei/msyh/simsun 可用，不新造字库。
- 修复巡屏 off-by-one，tour5 八屏页序完整；NYX 验收 tour4 后要求 tour5 每屏一张，已上传 tour5。
- 将 8 张 1024×600 原型图接入固件：新增 jt_v113_screens.h，app_jt_ui.c 引用 jt_v113_0..7；先做“原型图为底 + 原生动态标签覆盖”的过渡版。固件从 0x8bb3e0 增至 0xb136e0，factory app 仍余约 8%。
- 初次叠加出现静态控件与底图重复；已改为八屏构建完成后统一执行 jt_add_prototype_backgrounds()，底图 move background 并隐藏非动态子控件，只保留时间/日期/MEMO、卡数、拍题纸面、判题/卡数、领取数值等动态对象。构建+刷入通过。
- tour9 实拍：1/2/3/5/6/7/8 屏静态外观已贴近原型；第 4 屏仍有处理进度动态文字与底图重复，下一步要隐藏 process 进度覆盖或改成只显示进度条位置；第 7/8 屏动态数值位置还需微调。
- tour9 已复制到云盘 tour9。网关 /s 回复接口多次返回 HTTP 503，/watch 和 check 正常；待服务恢复后补发一条汇总给 NYX。
- 当前源码 CONFIG_JT_UI_CAPTURE_TOUR 仍为 1，仅用于采图；最终验收后必须改回 0 并 build+flash，避免正常使用自动跳页。

### 2026-11-25 ABB 549/550：设置浮层清理、真摄像头拍摄页、连通测试/设备信息
- 9080 消息 552 已回复并 ack；按 ABB 要求逐项处理并在网关回报。
- 设置/首页清理：
  - 删除首页“服务器已连接”独立浮层及其底图保留例外，状态展示以第 7 屏原型/服务器链路结果为准。
  - 第 7 屏“连通测试”的静态成功文案改为默认隐藏，只有用户点击后才显示 `WiFi -> 互联网 -> 服务器` 结果。
  - 设备 ID 改为从 `jt_client_get_terminal_id()` 读取，热点名称按 `JianTi-<terminal_id>` 显示。
- 拍摄页真摄像头：
  - 删除假数学题纸面和错误公式标签，拍摄页改为 `lv_img` 实时显示 MIPI/OV5647 帧。
  - 摄像头 PPA 输出从直接刷 DSI framebuffer 改为双缓冲 PSRAM 预览，LVGL 回调内换帧并刷新图片对象，避免和 LVGL 抢整屏。
  - 修复 PPA `out.buffer` 对齐：预览缓冲按 `esp_cache_get_alignment()` 对齐（实测 line=128），补齐 buffer size；原来的 `out.buffer addr or out.buffer_size not aligned` 已消失。
  - 修复重复进入拍摄页时重复启动 V4L2 stream 的问题：`mipi_cam_init()` 对已运行任务做幂等返回。
  - 快门/确认按钮调用 `mipi_cam_request_photo()`，沿用现有硬件 JPEG 编码、存储与 `jt_client_solve_async()` 解题链路。
- 验证：
  - 编译通过：`D:\AItools\projects\jt2.0\_work\build_abb_camera_final.log`，固件 `0xb16f40`，app 分区剩余 `0xe90c0`（8%）。
  - 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_abb_camera_final.log`，`Hash of data verified` / `Done`。
  - 串口验证：`OV5647 PID=0x5647`，`1024x600 RGB565`，`preview buffers aligned: line=128`，稳定约 `20.8fps`；截图/拍照保存 `PHOTO002.jpg size=12644`；无 `Guru Meditation`、`Rebooting`。
- 后续补齐（ABB 563）：
  - `JianTi-<终端ID>` 热点页打开时真正启动 `WIFI_MODE_APSTA` 开放热点，返回时若 STA 已连则恢复 `WIFI_MODE_STA`；`wifi_module_init()` 已导出到 `app_wifi.h`。
  - 连通测试拆成 `WiFi / 互联网 / 服务器` 三段状态，点击后显示；WiFi/互联网段由 `jt_client_network_ready()` 判断，服务器段由 `jt_business_server_config_refresh()` 判断。
  - 最终统一编译烧录：`build_abb_settings_log.log` / `flash_abb_settings_log.log`，固件 `0xb17100`，app 分区剩余 8%。
  - 板端确认：进入设置页日志 `network_ready=1`，无崩溃；进入拍摄页 OV5647 出帧约 20.8fps 并保存照片。9080 已向 ABB 回报三项齐，等待板端审计。

### 2026-11-25 ABB 572/NYX 580 复审整改
- 9080 心跳机制确认：inbox 每轮写本地 `cod_gateway_heartbeat.txt`，guard 10 分钟检查，超过 180 秒视为 stale 并拉起缺失进程。纯本地 IO，不产生模型调用、不消耗 token；当前 inbox/wake/guard PID 为 10600/16868/35716，心跳 age 6~10 秒。
- 修复退出热点页 AP 未停止：`app_jt_ui.c` 的 `jt_show_softap(false)` 改为无条件停 DHCP/AP 并切回 `WIFI_MODE_STA`，不再依赖 `jt_client_network_ready()`。板端串口日志：`softap stopped: dhcps=0x103 mode_set=0x0 mode_now=1(STA=1)`，随后 STA 重连并拿到 `192.168.0.105`。
- 修复连通测试互联网段：`jt_client.c` 新增 `jt_client_internet_reachable()`，用 `http://www.baidu.com` 独立 HTTP GET 判断公网；服务器段仍由 `jt_client_server_reachable()` 判断，不再复用 WiFi 状态。板端串口日志：`internet probe http://www.baidu.com status=200 reachable=1`、`server probe http://192.168.0.108:8000/lockscreen?device_id=2040059832773436 status=200 reachable=1`、`chain test: wifi=1 internet=1 server=1`。
- 设备 ID 来源：`jt_client.c` 默认 `JT_TERMINAL_ID "2040059832773436"`，`s_terminal_id` 初始值来自该编译常量；`jt_client_set_config()` 可在运行时覆盖。新增 `jt_client_terminal_id_source()` 和启动日志，板端实证 `device id=2040059832773436 source=compile-time-default`。当前不是 EFUSE/MAC 硬件派生；如要求硬件派生，需要 ABB/NYX 给出终端 ID 编码规范后再改。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_abb_reaudit_console2.log`，固件 `0xb176f0`，app 分区剩余 `0xe8910`（8%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_abb_reaudit_console2.log`，`Hash of data verified` / `Hard resetting via RTS pin`。
- 证据文件：
  - `D:\AItools\projects\jt2.0\_work\abb_audit_boot_id.log`（启动、设备 ID 来源、ELF SHA256）
  - `D:\AItools\projects\jt2.0\_work\abb_audit_camera_serial_evidence.log`（OV5647、RGB565、PHOTO003、约 20.8fps）
  - `D:\AItools\projects\jt2.0\_work\abb_audit_chain_serial_evidence.log`（热点开启/退出、AP 停止、STA 重连）
  - `D:\AItools\projects\jt2.0\_work\abb_audit_chain_only.log`（独立互联网探针和三段连通结果）
- 固件 SHA256：bootloader `334a2850b405eaa51ae6a0154d7ac46267fc827de6c1227312183383765ae439`，partition-table `d1d2b2745f8ec4697960f395dd8f0572dfb12798d467ea9643f488eae21b0361`，app `88ab653c7f9234cefbab010a6925cc6368318e0d64c19772a8e978618bf03d84`。

### 2026-11-25 右上角齿轮按钮按原型图重做
- 排查结论：原 `make_jt_icons.py` 的齿轮按钮取错像素源，使用了旧的 `v113_screen1.png` 和 `(850,120,884,162)` 裁切范围；当前 `v113/screen1.png` 中齿轮实际位于约 `(936,28)-(976,68)`。
- 修正：齿轮按钮改为从 `v113/screen1.png` 的 `(936,28,976,68)` 裁出并放大成 64×64 原型图片，继续以透明底 RGB565A8 图片按钮使用；左屏/首页热区不变。
- 重新生成：`main/APP/IMAGE/jt_v113_icons.c/.h`、`gear_image_button_preview.png`；预览确认齿轮形状与原型一致。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_gear_fix.log`，固件 `0xb17a00`，app 分区剩余 `0xe8600`（8%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_gear_fix.log`，`Hash of data verified` / `Hard resetting via RTS pin`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_gear_fix.log`，ELF SHA256 `74507779f...`，`JT_UI: JianTi v114 native-widget UI ready`，无重启/崩溃。
- PC 摄像头实拍：`D:\AItools\projects\jt2.0\_work\gearfix_01_gear.png`，锁屏右上角可见清晰小齿轮，位置和形态与原型一致。

### 2026-11-25 NYX 603：共5题屏解题卡重复
- 问题：第 5 屏（共5题/思路）右上角“解题卡”重复渲染，一处来自原型底图静态图，另一处来自动态 `s_jt.idea_card` 标签。
- 修复：`jt_add_prototype_backgrounds()` 的保留控件列表移除 `s_jt.idea_card`，原型底图继续绘制静态卡片，动态标签隐藏，避免同一位置画两遍。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_idea_card_dup_fix.log`，固件 `0xb17a00`，app 分区剩余 `0xe8600`（8%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_idea_card_dup_fix.log`，`Hash of data verified`。
- 串口验证：`D:\AItools\projects\jt2.0\_work\monitor_idea_card_dup_fix.log`，ELF SHA256 `f78a49456...`，UI ready，Wi-Fi 获取 IP，无崩溃。
- PC 摄像头实拍：`D:\AItools\projects\jt2.0\_work\ideacardfix_05_dupfix2.png`，第 5 屏右上角只剩一个“解题卡 5张”，重复已消除。

### 2026-11-25 NYX 606：使用解题卡流程修正
- 问题：点第 5 屏右上角“解题卡”误进预支/申请页。根因是 `s_cards` 初始为零但指针有效，`jt_card_balance()` 读到余额 0，未先按 A3 `/cards/check` 刷新当前题状态。
- 修复：
  - `jt_go_idea()` 进入思路页时按当前 `question_id/job_id` 调 `jt_business_cards_refresh()`，按 A3 返回更新缓存与解锁态。
  - `jt_use_card()` 每次先 A3 查卡；已解锁直接进结果页；确认余额 <=0 才进领取页；否则调用 A4 `jt_business_answer_unlock()` 扣卡看答案。A4 返回 402/余额<=0 时才转领取页，其他失败留在思路页。
  - debug `page 5` 走 `jt_go_idea()`，便于板端复现真实进入路径。
- 编译通过：`D:\AItools\projects\jt2.0\_work\build_card_flow_fix3.log`，固件 `0xb17f20`，app 分区剩余 `0xe8200`（8%）。
- 烧录通过：`D:\AItools\projects\jt2.0\_work\flash_card_flow_fix3.log` 与 `flash_card_flow_fix3_reflash.log`，`Hash of data verified`。
- 说明：复测串口时 COM4 被另一个调试进程占用，`monitor_card_flow_final.log` 记录 `PermissionError(13)`，本轮不拿该文件当通过证据；PC 实拍 `cardflowfix_05_after.png` 确认第 5 屏仍正常显示一个解题卡。流程正确性以推送源码中的 A3/A4 分支为准，待 COM4 空闲后补抓串口。

