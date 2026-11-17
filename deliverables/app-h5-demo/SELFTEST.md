# 演示版自测记录（app-h5-demo）

- 日期：2026-11-18
- 来源：9090 指挥通道消息 515（Nyx）——「手头后半段测完，再做一个演示版（单独一版，别动正常版）」
- 范围：只新增本目录；`../app-h5/index.html`、`../app-h5/jianti-2.0.0-debug.apk` 未做任何修改（manifest 8/8 仍一致）。

## 1. 构建环境

| 项 | 值 |
|---|---|
| JDK | `tools\jdk17` |
| build-tools | `34.0.0`（aapt2 / d8 / zipalign / apksigner） |
| platform | `android-34` |
| 签名 | debug keystore（脚本内生成，不提交） |

## 2. 演示写死项（仅界面预填，不绕过接口）

| 屏 | 字段 | 值 | 方式 |
|---|---|---|---|
| 第9屏 登录 | 手机号 / 密码 | 18601109233 / 123456 | 进屏自动填（空时） |
| 第10屏 注册 | 手机号 / 验证码 / 密码 / 管理员密码 | 18601109233 / 123456 / 123456 / 123456 | 进屏自动填（空时） |
| 第11屏 管理密码 | 6 位 | 123456 | 进屏自动填（空时） |
| 重置密码 | 手机号 / 验证码 | 18601109233 / 123456 | 进屏自动填（空时） |

实现：演示副本 `index.html` 末尾新增 `DEMO` 常量与 `fillDemo()`，并在 `goTo()` 的 `render()` 后调用；正常版无此函数、无此调用。

## 3. 核对结果

| 检查 | 方法 | 结果 |
|---|---|---|
| 脚本语法 | 抽取 `<script>` 内容 `new Function()` | PASS（119,459 字符） |
| 预填生效 | headless Edge + CDP 读 DOM 值 | PASS：loginPhone=18601109233、loginPwd=123456、regPhone/regCode/regPwd=18601109233/123456/123456、resetPhone/resetCode 同上 |
| 包名 / 版本 / 名称 | `aapt2 dump badging` | PASS：`com.jianti.app.demo`、`2.0.0-demo`、label「简题演示」 |
| 图标 | `aapt2 dump badging` + APK 条目 | PASS：mdpi/hdpi/xhdpi/xxhdpi/xxxhdpi 五档 ic_launcher.png（48/72/96/144/192） |
| 内嵌 H5 一致 | 解包 `assets/index.html` 与源文件比 SHA256 | PASS：`b92eef051ce37b71c46d410dd657eca631687ccf09114c69e34861adb4315cfd` |
| 签名 | build.ps1 内 `apksigner verify` | PASS |
| 与正常版并存 | 包名不同 | PASS（可与 `com.jianti.app` 同时安装） |
| 业务逻辑 | 与 487 版逐行相同（仅加预填与图标） | 见 `../app-h5/SELFTEST.md` 第 10 节 13/13 |

## 4. 产物（515 首版）

- 文件：`jianti-demo-2.0.0-debug.apk`
- 大小：148,333 字节
- SHA256：`403b768e3dfe260371c894fda89a4c6c390c5f075b029bc2681a1a97523298d7`

## 5. 未做 / 边界

- 未改服务端，未动线上配置；网关测试模式认这套演示账号由 @abb 负责。
- 演示版不新增任何账号特判或跳过校验的逻辑；验证码 123456 能否通过取决于网关测试模式，App 侧不伪造成功。
- 未在真机安装验证（本机无 adb 设备），APK 结构、签名、资源、内嵌 H5 已逐项核对。

## 6. 526 图标定稿重打包（2026-11-18）

- 依据：9090 消息 526（Nyx）「图标定了：用云盘 jianti-app-icon-shutter-check.png（黑白快门钮＋大白对号），演示版就换这个」。
- 改动范围：只换图标源图与五档 `ic_launcher.png`，`index.html` 一字未动；正常版 `../app-h5/` 未动。

| 项 | 值 |
|---|---|
| 图标源图 | `jianti-app-icon-shutter-check.png`（1600×1600，92,740 字节，MD5 `faf13fffd8e0de282097cab946fc666a`，SHA256 `56776db2ecfd2d24dc6cf915c5c4dcf42ef68ae8f0c48ba6d7aeafcea10291eb`） |
| 图标尺寸 | mdpi 48 / hdpi 72 / xhdpi 96 / xxhdpi 144 / xxxhdpi 192 |
| 内嵌 H5 | `b92eef051ce37b71c46d410dd657eca631687ccf09114c69e34861adb4315cfd`（与 index.html 一致，未变） |
| 包名 / 版本 / 名称 | `com.jianti.app.demo` / `2.0.0-demo` / 「简题演示」（`aapt2 dump badging`） |
| 签名 | `apksigner verify --min-sdk-version 24` PASS |
| APK | 95,086 字节，SHA256 `efe025b16df9d038e2cc4748ec73a794d71e30f2f7e724e2db92db1749ad4d14` |

