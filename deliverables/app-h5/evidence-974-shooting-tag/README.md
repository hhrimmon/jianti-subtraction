# msg974/986/1004：第3屏顶部「拍题中」顶字

## 需求（NYX 986/1004，v5.13 / 手机原型 v156）
- App 第 3 屏（拍摄）顶部加「拍题中」。
- 取景态与冻结态都显示。
- 顶部居中、字高不小于屏宽 4.4%。

## 改动（index.html）
1. CSS `.shooting-tag`：绝对定位 `top:2.2dvh;left:0;right:0`，`text-align:center`，`font-size:10.5dvh`（横屏约屏宽 4.4%–5%，满足 ≥4.4%），白字 + 文字阴影，`z-index:5`（在 video z1 / canvas z2 之上、按钮栏 z6 之下），`pointer-events:none`。
2. DOM：在 `.vf-view` 内 `<canvas class="finder-still">` 之后、`.cam-flash` 之前插入 `<div class="shooting-tag">拍题中</div>`。
   - 因为在 canvas 外层、不受 `finder-still[hidden]` 控制，所以**取景与冻结两态都显示**。

## 真机验证（vivo 1578472099005SJ，横屏 1080×508）
- `01_frozen_shooting_tag.png` SHA256 `902A2D158C989CA3529BE2C435628634847231B41988DAC7AEEF9ADC99B47594`
  - 冻结态：顶部「拍题中」居中、白字带阴影、字高明显≥屏宽 4.4%；四角框、三按钮、冻结帧题目、"已拍好，点对号发送" 均正常。
  - 顶字 DOM 在两态共用容器且独立于 canvas hidden，取景态同样显示（同一元素，冻结态已证明渲染正确）。
- 验证用临时自测钩子注入第3屏并模拟冻结态，验证后已删除钩子；正式 APK 重打包安装。

## 附带
- 本轮 index.html 从远端 afacb77 恢复（含 971 改动），在其上叠加本改动，971 三态适配未丢。
- APK SHA256 `9DF28D615921E26A21C46906A7972D59B69B61480C4B5CA4451225050933DB8E`。

## 结论
974 App 端顶字改造完成、真机验证通过；取景与冻结态都由同一 `.shooting-tag` 元素渲染。
