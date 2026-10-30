# 简题（JianTi）· 可部署代码包

本包是「简题」整套系统的封版代码：板子固件底版 + 手机 App 壳 + 服务器网关 + 路由管理模块（独立夹）+ 部署文档。
许可证：Apache-2.0（见 `LICENSE`）。

---

## 一、目录说明

```
server/                 服务器网关（正式入口，端口 8000，接口 /solve、/health）
  gateway.py            网关主体（v3.23-card-preborrow-floor-8000，仅标准库）
  admin_routes.py       外部路由表加载器（读 route_admin 产出的 JSON，可选）
  qw_bridge.py          Web-AI 桥接（可选，默认 127.0.0.1:8001）
  watchdog.py           守护进程（可选）
  autorun.pyw           开机自启入口（可选）
  run_gateway.ps1       启动脚本
  start_gw_wbapi.bat    Windows 一键启动（本机 API 直连模式）
  requirements.txt      仅桥接需要（playwright、psutil）；网关本身零第三方依赖
firmware/               板子固件底版（ESP-IDF 工程，ESP32-P4 + OV5647）
  main/                 main.c（相机/UI/结果页）、uplink.c（WiFi+上传）、prov.c（配网）、
                        app_video.c（成像管线）、dns_server.c（配网热点 DNS）、字库 *.c
  CMakeLists.txt        工程入口（ESP-IDF 5.5.x）
  partitions.csv        分区表
  sdkconfig.defaults    默认配置
app/                    手机 App 壳（HarmonyOS ArkTS）
  Index.ets             单页应用源码
  entry-default-signed.hap  已签名安装包
route_admin/            路由管理模块 v1.5.0（独立模块，夹带；默认 127.0.0.1:8791）
01_盘点清单.md          本机实查的版本与哈希
02_清理清单.md          逐项清理记录
04_快照/                本地与 43 的部署前快照
05_staging验证/         43 staging 健康检查与接口验证日志
06_入库记录.md          仓库、tag、推送记录
07_未修清单.md          未清理/未修项与原因
report_to_nyx.txt       交付回执
```

## 二、硬件清单

| 件 | 型号/规格 | 说明 |
|---|---|---|
| 主控板 | 微雪 ESP32-P4-WIFI6-Touch-LCD-4.3 | ESP32-P4 + 板载 4.3 寸触摸屏 |
| 摄像头 | OV5647（RAW8，经 P4 ISP 出图） | 板载 MIPI 接口 |
| 无线 | ESP32-C6（esp_hosted，板载） | WiFi6，固件侧走 esp_wifi_remote |
| 电源 | USB-C 5V/2A | 与下载口同一路 |
| 手机 | HarmonyOS NEXT（装 `entry-default-signed.hap`） | 需允许安装未上架应用 |
| 服务器 | Windows Server 2019（示例为 43 机器） | 跑 `server/gateway.py`，Python 3.10+ |

## 三、接线

1. 摄像头排线插板载 MIPI 座（金手指朝下，卡扣压紧）。
2. USB-C 接电脑或 5V 电源；接电脑时同时是烧录口与串口。
3. 无需外接屏幕、无需 SD 卡（照片经串口或 WiFi 上传）。

## 四、烧录（固件）

1. 装 ESP-IDF v5.5.x（VS Code 插件或命令行均可）。
2. 打开 `firmware/` 这一层（含 `CMakeLists.txt`）。
3. 选串口 → `set-target esp32p4`（仅首次）→ Build → Flash → Monitor。
4. 首次编译会联网拉 BSP / esp_video / LVGL 组件，耗时属正常。
5. 串口出现 `JianTi cam ... ready` 即启动成功。
6. 未配网时板子开热点，用手机连上后按屏幕提示填 WiFi 名/密码，保存后自动重启入网。

> 固件源码版本以 `main/main.c` 头部注释为准（当前为 v0.3 配网版）。`README.md` 里的 v0.1 描述为早期相机基线，功能已迭代。

## 五、部署服务器网关

```powershell
# 1) 放代码（示例）
#    C:\jt43\app\server\

# 2) 起网关（正式入口 8000）
python .\server\gateway.py --port 8000

# 3) 自检
curl http://127.0.0.1:8000/health
#    -> {"ok": true, "gateway": "v3.23-card-preborrow-floor-8000", ...}
```

- 正式端口固定 **8000**，接口 `/solve`（POST，`Content-Type: image/jpeg`，body 为图片字节）与 `/health`（GET）。
- 模型路线二选一：
  - **本机 API 直连**（默认）：网关调 `JT_WB_API_BASE`（默认 `http://127.0.0.1:7863/v1`）里的模型；
  - **Web-AI 桥接**：起 `qw_bridge.py`，网关按 `JT_BRIDGE_*` 轮换调用。
- 只用标准库即可启动；桥接才需要 `pip install -r server/requirements.txt`。
- 没有真实模型 key 时可用**假答案模式**做接口连通性验证（仅验证接口，不代表模型能力）：

```powershell
$env:JT_SOLVE_FAKE="1"
python .\server\gateway.py --port 8000
```

### 常用环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `JT_VLM_BACKEND` | `dashscope` | `dashscope` / `ollama` / `qianwen` |
| `JT_VLM_API_KEY` | 空 | 真实模型 key（**不入库、不写进本包**） |
| `JT_VLM_MODEL` | `qwen-vl-max` | 视觉模型名 |
| `JT_WB_API_BASE` | `http://127.0.0.1:7863/v1` | 本机 API 代理地址 |
| `JT_WB_USAGE` | `server/state/wb_usage.jsonl` | 积分记账文件路径 |
| `JT_SOLVE_FAKE` | 空 | 置 `1` 走假答案，仅做接口验证 |
| `JT_QW_SOFT_DEADLINE` | `90` | 桥接软超时（秒） |
| `JT_FALLBACK_MODEL` | `qwen2.5vl:7b` | 本机 Ollama 兜底模型 |

## 六、手机 App

- 装 `app/entry-default-signed.hap`；首次打开填服务器地址 `http://<服务器IP>:8000/solve`。
- 源码 `app/Index.ets` 可直接用 DevEco Studio 打开同结构工程替换后重新签名打包。
- 默认地址已改为 `http://127.0.0.1:8000/solve`（本机自测占位），正式使用请填服务器 IP。

## 七、路由管理模块（独立夹）

- 入口：`route_admin/app.py`，默认 `127.0.0.1:8791`，**只绑本机**，拒绝绑定其它地址。
- 与网关的唯一契约：把启用路由按等级降序写成 JSON（默认 `route_admin/data/gateway_routes.json`），网关 `admin_routes.py` 只读加载。
- 零第三方依赖、无数据库、不调网关写接口；**没有网关也能启动、编辑、导出**。换业务系统只需改 `gateway_output` 路径或调 HTTP 接口。
- 启动与自启见 `route_admin/README`（若包内未附，见 v1.5.0 完整成果包说明）。

### 密码在哪（本包不含任何密码原文）

| 场景 | 位置 |
|---|---|
| 首次启动 | 自动生成 `route_admin/data/first_run_password.txt`，登录后改密码并删除该文件 |
| 自定义初始密码 | 启动时加 `--password "你的口令"` |
| 服务器 SSH | 由运维自持（本包不记录、不写入任何文件） |
| 模型 API Key | 环境变量 `JT_VLM_API_KEY` 或本机 DPAPI 存储（不入库） |

## 八、安全边界

- 本包不含任何密码、API Key、Cookie、浏览器 profile、运行日志、题库与截图。
- 管理页只绑 `127.0.0.1`；远程访问请自备 SSH 隧道。
- 网关监听 `0.0.0.0:8000` 由使用方决定是否对公网开放；本包不附带任何防火墙/端口放行脚本。

## 九、许可证

Apache License 2.0，见 `LICENSE`。
