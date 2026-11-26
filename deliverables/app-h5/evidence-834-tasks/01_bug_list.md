# 834-3：今日 bug 清单（2026-11-27，App 端）

## 已修复并真机验证
1. **结果页 answer_status 适配（msg971）**：unanswered 误贴"做错了"标签 → 改为"无对错·只给思路"，answered 保持原对错。真机两态截图见 evidence-971-answer-status。
2. **第3屏顶字缺失（msg974）**：取景/冻结态无「拍题中」→ 新增 `.shooting-tag`，顶部居中、字高≥屏宽 4.4%，两态都显示。真机截图见 evidence-974-shooting-tag。
3. **连通假显示（msg834-2）**：第2页右上角曾显示"未连接"但连通三步全通 → 根因是首页 `.srv` 与设置页 `.guide-srv` 状态未共享；现 boot health 与 probeServer 第3步都调 `setServerChip` 同步。真机第2页显示"服务器已连接"，截图见 evidence-744-745-screens/02_screen_home.png。

## 已知/待定（非本轮新修）
4. **左侧黑边**：App 全局 WebView 安全区留白，所有页面都有（含锁屏、首页）。浩爷 2026-11-26 确认"这样好多了，比带黑底的"，暂不处理；若要去掉需改 MainActivity cutout 模式（全局影响），等指令。
5. **二维码识别**：此前浩爷明确不得以"二维码太小/对焦受限"推责，须自查。当前无新增识别失败复现；如再出现按"自查+报 9080"流程处理。
6. **锁屏 condition 字段**：官方 43 已升 v9.7 并在 `/lockscreen` 返回 `condition`（如"雾"/"晴"），App 兼容逻辑已生效（≤5 字拼接、无字段只显温度）；真机锁屏显示 `8°C 晴` + MEMO，见 evidence-744-745-screens/01_screen_lock.png。无需改动。

## 环境说明
- 本机 github.com:443 git 出口不通，api.github.com:443 通，推送走 gh CLI 的 Git Data API；远端 dev 为准。
- 官方服 v9.7.0, test_mode=false；localhost:8000 同版本 test_mode=true；192.168.0.108 本机不可达。
- 测试账号 13800000003 登录 200（LOCAL_ACCOUNT 已彻底移除，无本地兜底）。
