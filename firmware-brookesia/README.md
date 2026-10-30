# 简题固件 v1.0（Brookesia 交付版）

这是板子上实际在跑的那套固件源码：**ESP32-P4 + Brookesia + LVGL**，2026-10-29 的 v1.0 交付批（内部批次号 **v10b**，端口统一到 8000 之后）。树莓派式的 4.3 英寸 480×800 屏、OV5647 定焦摄像头，UI 全部为自绘。

> 与仓库根目录 `firmware/` 的关系：那份是早期 **相机底版 v0.3**（`jt_cam_v01` 线，只做拍、传、显示结果，用来验证链路）。本目录才是整机交付固件。两者不是同一个工程，不要混用。

## 这一版里有什么

| 路径 | 内容 |
|---|---|
| `main/main.cpp` | 启动、板级初始化、存储挂载、任务编排 |
| `main/jianti_debug.cpp` | 联调页与 v10 素材批自测（`v10A` 真拍 / `v10B` 假拍兜底），仅 `CONFIG_JIANTI_DEBUG_AUTOTEST` 打开时编译进来 |
| `components/brookesia_app_jianti/` | 简题 App 本体：拍题、思路卡、用卡看过程与答案、设置、配网（`esp_brookesia_app_jianti.cpp` 约 178 KB） |
| `components/brookesia_core/` | Brookesia 框架与手机式 UI 系统 |
| `components/esp32_p4_wifi6_touch_lcd_4_3/` | 微雪 P4 4.3 寸板级配置 |
| `components/` 其余 | 上游示例 App（MusicPlayer、XiaozhiApp、Camera、VideoPlayer 等），随框架一起编译，简题不使用 |
| `spiffs/` | 打包进 SPIFFS 分区的资源；中文 30 px 字体在构建时由 `xiaozhi-fonts` 注入为 `xiaozhi/font.bin` |
| `partitions.csv` / `sdkconfig.defaults*` | 分区表与默认配置 |

## 版本与证据

- 交付构建目录（未归档）：`build-jianti-delivery/`，2026-10-29 19:31
- v10 素材批收尾打印：`===JT_AUTOTEST_DONE v10_material===`，产出 `v10A_01..07`（真拍）与 `v10B_01..07`（假拍兜底）
- 串口启动留底：`serial_boot_delivery_v10b_8000_20261029.txt`

## 怎么编译

ESP-IDF **5.5.x**（本批实测 5.5.6），`esp32p4` target。

```bash
cd firmware-brookesia
idf.py set-target esp32p4
idf.py reconfigure          # 首次会按 idf_component.yml 拉取 managed_components（约 780 MB，需联网）
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.jianti" build
idf.py -p <COM口> flash monitor
```

网关地址在 `sdkconfig.defaults.jianti` 里，默认是示例值 `http://192.168.1.100:8000/solve`，编译前改成你自己的服务器地址；Wi-Fi 凭据不进固件，走运行时 SoftAP 配网（`JianTi-XXXX` → `192.168.4.1`）写入 NVS。

## 没有归档的东西

| 项 | 原因 |
|---|---|
| `build-jianti/`、`build-jianti-delivery/` | 构建产物，合计约 1.5 GB |
| `managed_components/` | 第三方依赖（约 780 MB），由 `idf_component.yml` 拉取 |
| `dependencies.lock` | 内含开发机绝对路径，`idf.py reconfigure` 会重新生成 |
| `sdkconfig` | 本机构建生成，默认值已在 `sdkconfig.defaults*` 里 |
| `*.bin` / `*.elf` / `*.map` | 编译产物，按仓库约定不入库 |

## 边界

- 不含 Wi-Fi 密码、API Key、Cookie、运行日志与任何题库
- `sdkconfig.defaults.jianti` 中原开发机内网地址已替换为示例值
- 联调自测页在交付构建里默认关闭（`CONFIG_JIANTI_DEBUG_AUTOTEST` 未开启）
- 许可：Apache-2.0，与仓库一致

上游 Brookesia 说明见 `README.upstream.md` / `README.upstream.zh.md`。
