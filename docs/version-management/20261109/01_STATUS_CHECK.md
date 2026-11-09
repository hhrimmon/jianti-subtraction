# 第 48 批 · 现状核查表（GitHub ↔ 本地）

- 批次：第 48 批 — WBC：GitHub 版本管理（兼职）
- 核查时间：2026-11-09 23:07–23:16（北京时间）
- 核查人：WBC（风控／兼职版本管理）
- 口径：**全只读**。GitHub 走公开 REST（`api.github.com` / `raw.githubusercontent.com`）；本地走文件系统只读扫描与 SHA-256 复算。核查阶段未改仓库、未改代码、未改产品文档。
- 任务文档真件：`F:\AIsoft\data\简题2.0\简题2.0_开发任务_第48批_WBC_GitHub版本管理.md`，2,889 B，SHA-256 `2c56d727c37d723c9bfdd0bb82b2aaeff641b33309074003e45429b179d73190`（与 Nyx 23:05 简报的 2,889B／`2c56d727…` 一致）。

---

## 一、GitHub 仓库现状（只读实测）

| 项 | 实测值 | 来源 |
| --- | --- | --- |
| 仓库 | `hhrimmon/jianti-subtraction`（参赛仓库） | `GET /repos/{owner}/{repo}` 200 |
| 可见性 / 归档 | public，未归档 | 同上 |
| 许可证 | Apache-2.0（`LICENSE` 10,771 B） | `license.spdx_id` + raw 下载 |
| 默认分支 | `main` | 同上 |
| 仓库大小 | 20,123 KB | `size` |
| 创建 / 最后 push | 2026-10-25T04:17:17Z / **2026-10-30T18:24:42Z** | `created_at` / `pushed_at` |
| 主语言 | C（62,366,617 B）、C++（1,803,770）、Python（98,160）、CMake、PowerShell、TypeScript | `GET /languages` |
| 分支 | 2 个：`main` @ `5a56d74479ebf8378fec080b5e5f52f32d9b5625`；`taskC-20261030` @ `6f520926f845cb21844f29b7ca0c2d74ad30f271` | `GET /branches` |
| Tag | **仅 1 个**：`v3.23-card-preborrow-floor-8000-taskC-20261030` @ `5f8823d04a70ccae7ebb0a95022395cf249248ff` | `GET /tags` |
| Release | 1 个，同名 `v3.23 · 卡预支下限（Task C 封版）`，2026-10-30 发布，assets 0 | `GET /releases` |
| 根条目 | 7 项：`.gitignore`(329)、`LICENSE`(10,771)、`README.md`(4,508)、`app/`、`docs/`、`firmware-brookesia/`、`server/` | `GET /contents` |
| 完整树 | 924 条目，`truncated=false`（未截断） | `GET /git/trees/main?recursive=1` |
| 树分布 | `firmware-brookesia/` 874、`app/` 36、`server/` 9、`docs/` 2、根 3 | 树统计 |
| `docs/` 内容 | **仅 `docs/architecture.md` (2,622 B)** | 树 + raw 下载 |
| `main` 最新提交 | `5a56d744` 「feat(firmware): 归档 v1.0 交付固件（Brookesia + LVGL，v10b）」 2026-10-30T18:24:24Z | `GET /commits` |
| 提交总数 | 3（`5a56d744` ← `4bc73854` ← `2e65269d`） | `GET /commits?per_page=10` |
| 分支保护 | 两个分支均 `protected=false` | `GET /branches` |

**上游关键文件逐件 SHA-256**（raw 下载后本地复算，字节数与 GitHub 树 `size` 逐一吻合）：

| 上游路径 | 字节 | SHA-256 |
| --- | ---: | --- |
| `server/gateway.py` | 35,953 | `826530d258f1d11cb9674b0aa58a2311a9d32b0b76f1596147ddff331cedbaf8` |
| `server/qw_bridge.py` | 54,869 | `464b2d1f53e5b0629a1b20f1d6499955eca03a31f6b5019dbcc43a6d1ab0b57c` |
| `app/entry/src/main/ets/pages/Index.ets` | 52,397 | `fb4ba80aedca5bb42ffed6deedaaae41e7974a29566b74285e5197999460b876` |
| `docs/architecture.md` | 2,622 | `4422e71f52c0a40eb27aa018cf32208538f2f8c6f885fb6723301a7e7a8ffa05` |
| `README.md` | 4,508 | `8e77cb3fc92c4eec20a5e00f2d5daf16bdcfa6cb76fb959c0fe0b57fe762da35` |

> 可复算证据：`13_EVIDENCE_UPSTREAM_SHA.json`（含 5 件逐件字节＋SHA）、`10_EVIDENCE_GITHUB_API.json`（原始 API 响应摘要）、`11_EVIDENCE_TREE.json`（树统计）。

---

## 二、本地现状（只读扫描）

| 根目录 | 存在 | 深度≤2 文件数 | 目录数 | 已统计字节 | 说明 |
| --- | --- | ---: | ---: | ---: | --- |
| `F:\AI\jianti\` | 是 | 53 | 65 | 30,410,974 | 任务文档称「约 30 GB」，实测为**深度≤2 的浅层统计 30.4 MB**；深层（固件源码、模型素材、视频、`.venv`、构建产物）未全量递归，不在本表口径内 |
| `F:\b25src\` | 是 | 76 | 15 | 8,261,251 | Android/Kotlin 线源码与构建产物，含 `app-debug.apk` 6,501,555 B |
| `F:\AIsoft\data\简题2.0\` | 是 | 30 | 17 | 2,897,937 | 任务文档、需求分析／UIUE／PRD、各批交付物 |

- 三个根目录**均无 `.git`**（`F:\AI\jianti`、`.git`=False；`F:\b25src`=False；`F:\AIsoft\data\简题2.0`=False）→ 本地当前不受 Git 版本控制，这正是本批要补的洞。
- 本地鸿蒙壳线：`F:\AI\jianti\app\shell\entry\src\main\ets\pages\Index.ets` 75,976 B，SHA `d4ccc2fed6dde74c86da248bb8fdc6817dc7793309eee5e45febf8cc5e31b746`；`app\snapshots\` 下另存多份历史快照（如 `JianTiShell/.../Index.ets` 79,944 B `0fa618fb…`、`20261028_phone_before_edit/.../Index.ets` 62,269 B `db7dba11…`）。
- 本地网关线：`F:\AI\jianti\gateway_v3.23\gateway.py` 118,209 B，SHA `6e615702169e9ed027a804e26c0e80cfd5cd368ca2b69f20d3924d3ea26f1167`；`F:\AIsoft\data\简题2.0\DSH\_work\isolated_env\gateway\gateway.py` 同字节同 SHA（同一真件的两份副本）。与任务文档「网关 v3.23＋入口修复＋8 MB 修复，SHA 6E615702…」完全对上。
- 产品文档（任务文档点名的基线）逐件复算：

| 文档 | 路径 | 字节 | SHA-256 | 与任务文档对照 |
| --- | --- | ---: | --- | --- |
| 需求分析 v1.1 | `F:\AIsoft\data\简题2.0\codex\简题App2.0_需求分析_20261109.md` | 11,834 | `dfe62d5c8ec4fc6e1299ada4c4e9ce4cf69673b3bba8458c142068e0cf7bf02d` | `dfe62d5c…` ✅ |
| UI/UE 规范 v1.1 | `F:\AIsoft\data\简题2.0\codex\简题App2.0_UIUE交互规范_20261109.md` | 12,060 | `bea7b3879b73efde03337f05e1dd7617dc66d5af049b286a26f8ef4a1052f88f` | `bea7b387…` ✅ |
| PRD v1.0 | `F:\AIsoft\data\简题2.0\WBC\deliverables\20261109\batch42\docs\PRD_V1.0.md` | 6,976 | `50e53beceeaa4ba46ffab69aa80b05fa047df865c3ac49efc0f7629bfb9bf285` | `50e53bec…` ✅ |

> 完整逐文件清单（网关 95 件／隔离网关 30 件／Android 源码 24 件／鸿蒙壳 51 件／产品文档 7 件）见 `03_BASELINE_INVENTORY.md` 与 `12_EVIDENCE_LOCAL_BASELINE.json`。

---

## 三、两边差距（核心结论）

| 编号 | 差距 | 上游（GitHub） | 本地 | 判定 |
| --- | --- | --- | --- | --- |
| D1 | **网关版本严重滞后** | `server/gateway.py` 35,953 B / `826530d2…` | v3.23 基线 118,209 B / `6e615702…` | 上游落后一个完整代际（约 3.3 倍体量），入口修复与 8 MB 修复均未进仓库 |
| D2 | **鸿蒙壳版本不一致** | `Index.ets` 52,397 B / `fb4ba80a…` | 本地 `app\shell` 75,976 B / `d4ccc2fe…` | 上游不是当前交付版；Release 说明写的是 75,914 B `60C9C20B…`，与仓库实际 52,397 B **也对不上**，属上游文档自相矛盾，需单独裁定 |
| D3 | **Android 线完全缺席** | 仓库只有 HarmonyOS ArkTS 壳 | `F:\b25src\` 整套 Android/Kotlin 源码 + `app-debug.apk` 6,501,555 B / `ea5d1f14…` | 一条完整的产品线未纳入任何版本管理 |
| D4 | **产品文档未入仓** | `docs/` 只有 `architecture.md` 2,622 B | 需求分析 v1.1／UIUE v1.1／PRD v1.0 三份均在本地 | 2.0 的产品基线在 GitHub 上无留痕 |
| D5 | **tag / release 只覆盖 10-30 封版** | 唯一 tag `v3.23-card-preborrow-floor-8000-taskC-20261030` | 11-09 之后的所有批次（35/36/37/40/41/42/44/47/48）无版本标识 | 版本号不连续、无 2.0 语义版本、无法按版本回滚 |
| D6 | **分支策略缺失** | `main` + 一次性 `taskC-20261030`，无 `dev`，两分支均未保护 | 无 `.git` | 无稳定／开发分离，无合并与回滚路径 |
| D7 | **本地无版本控制** | — | 三个主目录均无 `.git` | 本地改动无提交、无 diff、无历史，出事只能靠文件夹快照 |
| D8 | **仓库 11 天未更新** | 最后 push 2026-10-30T18:24:42Z | 本地持续迭代到 2026-11-09 | 备份与同步机制事实上停摆 |

**补充观察（如实记录，不擅自改）**：
- `main` 上 3 个提交里，第 2、3 个提交信息存在 GBK/UTF-8 乱码（`server 鍗曠洰褰曞寲锛...`），是上游历史遗留，本批只记录、不改历史（不 rewrite、不 force push）。
- `firmware-brookesia/` 占 874/924 条目，是 10-30 归档的固件源码；Release 明确写「不含已烧录 bin、未做硬件在环验证」。本批不动固件。

---

## 四、写入通道实测（决定第 3 步「能推的先推」怎么执行）

| 检查项 | 实测结果 | 结论 |
| --- | --- | --- |
| `gh` CLI | 存在于 `C:\Program Files\GitHub CLI\gh.exe`（winget 记录 GitHub.cli 2.103.3），但**不在 PATH**（Bash `gh: command not found`） | 需用全路径调用 |
| `gh auth status` | **未登录任何 GitHub host**；`%APPDATA%\GitHub CLI\hosts.yml` 不存在；`gh api user` 报 `To get started ... gh auth login` | gh CLI 通道**不可用**，不拿它去要 token |
| Git 凭证 | 系统级 `git config --system` 有 `credential.helper=manager`；Windows 凭据管理器存在 `LegacyGeneric:target=git:https://github.com` 用户 `hhrimmon`，另有 `GitHub - https://api.github.com/hhrimmon` 用户 `hhrimmon` | **已有本机登录态**（与 Nyx 23:08 补充令「账号已登录」方向一致），但能否写入须以真实 push 结果为准，不以「有凭证」冒充「已推送」 |
| `git ls-remote` | 返回 `5a56d744…HEAD`，200 通 | 只读通道正常 |
| GitHub Desktop | `C:\Users\Administrator\AppData\Local\GitHubDesktop\GitHubDesktop.exe` 存在（401,184 B，23:09）；`%APPDATA%\GitHub Desktop` 存在 | 图形通道可用，本批不启动 GUI，用 git CLI |
| 本地仓库 | 三个主目录均无 `.git` | 需先 `clone` 到独立工作副本再提交，不在 30 GB 根目录直接 `git init` |

> **口径**：任务文档第 24 行禁「自己去要 token／绕过授权」；Nyx 23:08 转述浩爷补充令「已登录，推的时候直接推，不用等授权」。两者不冲突——使用**本机既有的登录凭证**不等于索取 token 或绕过授权。本批以真实 `git push` 结果为准：成功即把分支／tag／链接写进 `05_PUSH_RECEIPT.md`；若返回 401/403，则如实记为「卡在写入授权」，停下报回，**不重试绕过、不虚报已推**。

---

## 五、本表未覆盖 / 待裁定

1. `F:\AI\jianti\` 任务文档称「约 30 GB」，本表只做深度≤2 浅层扫描（30,410,974 B）；全量递归盘点与「哪些该进仓、哪些不该」需浩爷/Nyx 定范围，本批不擅自把 30 GB 推上 GitHub。
2. Gitee 主账本 `https://gitee.com/threeandhalfmans/jianti` 本批**未读取**（任务只要求列 GitHub↔本地差距），按边界「冲突以 Gitee 为准」原样保留，不核、不改。
3. Release 里 `Index.ets 75,914 B / 60C9C20B…` 与仓库实际 52,397 B 不符，本批只列为 D2 风险，不代 Nyx 裁定哪个是真件。
4. 上游提交信息乱码、未做分支保护，均属既有状态，本批只记录不改。
