# msg861：首屏天气改版（去“温度”二字 + condition）

## 需求
- 锁屏左下角去掉“温度”两个字，显示“27℃ 晴转多云”。
- condition 从 `/lockscreen` 的 `condition` 字段取，≤5 个字；取不到只显温度。
- 真机截图验证。

## 改动
- `index.html` 锁屏 HTML：删除 `<span class="temp-label">温度</span>`，仅保留 `.temp`。
- 删除 `.temp-label` CSS。
- `paintLockTemp()`：温度后拼 `lastLockCondition`（前置空格）。
- `envText()`：读取 `data.condition`，`String(...).trim().slice(0,5)`，空则只显温度；断网保留上次值。

## 服务端状态
- 官方 `http://solve.mhq-ai.xyz:8000/lockscreen` 当前返回：`{"city": "新加坡", "temperature": 26, "memo": "..."}`，尚无 `condition` 字段。
- 已报 NYX（msg910）请求服务端加 `condition`；App 端兼容已就位，字段一加即生效。

## 真机验证
- vivo 1578472099005SJ，横屏 2376×1080。
- APK SHA256 `D2010E595AF7CC05F13AB521F4C456688D8DA082F96570C60481652F9E952049`。
- `01_lockscreen_no_label.png`：锁屏左下显示 `26°C`，无“温度”二字，MEMO 正常，数据加载成功。
- `02_lockscreen_initial.png` SHA256 `224DDD76369A89B5ACA688BBE81AF9B3636D75DEBBDB61B097BE1B9842839163`：启动瞬间 `---°C`，布局一致。
- `01_lockscreen_no_label.png` SHA256 `23E51FE7A29DE9BCACC5FFFB11088D18C04276D851FA0EA60B49104B99173A83`：加载后 `26°C`，无“温度”二字，MEMO 正常。

## 结论
App 端改造完成；condition 显示依赖服务端加字段，已在 9080 报 NYX 推动。
