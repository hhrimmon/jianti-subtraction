# 简题手机 App · 演示版（app-h5-demo）

- 用途：给浩爷对外演示用的单独一版。与正常版 `../app-h5/` 并存，**正常版 index.html 与 APK 一律未改动**。
- 演示账号（界面预填，不绕过接口校验）：
  - 手机号 `18601109233`
  - 登录密码 / 注册密码 `123456`
  - 短信验证码 `123456`
  - 管理密码 `123456`
- 与正常版的差别只有两处：
  1. 登录、注册、重置密码、管理密码六位框进屏自动预填演示账号（演示时不用打字）；
  2. APK 图标换成 `jianti-app-icon.png`，包名改为 `com.jianti.app.demo`、应用名「简题演示」，可与正常版同时安装。
- 业务逻辑与 `../app-h5/index.html`（commit fe47c777，SHA256 `5f72d065…`）完全一致；接口仍走服务器，不改服务端。
- 网关测试模式需认这套演示账号（@abb 负责，App 侧不做任何账号特判）。

## 文件

| 文件 | 说明 |
|---|---|
| `index.html` | 演示版单文件 H5（正常版 + 演示预填） |
| `jianti-demo-2.0.0-debug.apk` | 演示版 APK（包名 com.jianti.app.demo，debug 签名） |
| `apk-shell/` | 打包壳源码（Manifest / build.ps1 / MainActivity.java / res 图标 / icon-src.png 图标原图） |
| `manifest.sha256` | 本目录 9 个文件的 SHA256 |
| `SELFTEST.md` | 本轮构建与核对记录 |

## 构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\apk-shell\build.ps1
```

产物：`jianti-demo-2.0.0-debug.apk`（构建脚本会自动同步到本目录）。
