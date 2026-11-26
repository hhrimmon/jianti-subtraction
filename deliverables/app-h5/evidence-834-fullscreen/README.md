# 临时任务：拍题取景器全屏显示（黑色底栏去掉）

## 需求
- 去掉取景器底部黑色按钮条，视频画面铺满整个取景屏。
- 三个按钮不变，浮在画面表层，仍可操作。
- 取景器和拍照冻结帧显示同一块区域。
- 本临时工作属于 834 任务包中的临时加项，完成后继续 834 四项。

## 改动
- `.vf-view` 从 flex 子项改为 `position:absolute;inset:0;padding:0`，占满整个拍摄屏。
- `.vf-bar` 从布局流改为 `position:absolute;bottom:0;z-index:6;pointer-events:none`，按钮条浮层。
- `.vf-btn` 增加 `pointer-events:auto`，保持按钮可点。
- `.finder-video` / `.finder-still` 均为 `position:absolute;inset:0;object-fit:cover`，因此取景与冻结帧使用同一容器尺寸；`finderSourceRect()` 按该容器尺寸计算裁切源矩形，所见即所得。

## 真机验证
- 设备：vivo 1578472099005SJ，横屏 1080×2376（截图 2376×1080）。
- APK：`jianti-2.0.0-debug.apk` SHA256 `E291BFF55D6D223CB4074A47D30E6A4015E0BEFC7B3DE400625FAE47B80CCCF8`。
- 截图：
  - `01_viewfinder_fullscreen.png`：取景器全屏画面，三个按钮浮在视频表层，无黑底栏。
  - `01_viewfinder_fullscreen.png` SHA256 `6A036A915B8D0977789AC6528AD69221F8ED49FF0BCC96D8E9FC2EA1CFE6004F`
  - `02_after_shutter.png` SHA256 `685830229093B39D8CB6C5693CBC1E4B35356F5D8D3AB2A87B7A56F7DA0D80C8`

快门冻结帧截图提示"已拍好，点对号发送"，对号按钮高亮；画面与取景器一致。

## 结论
取景与拍照画面一致；底部黑底栏消失；按钮位置/样式未变，仍在表层可操作。
