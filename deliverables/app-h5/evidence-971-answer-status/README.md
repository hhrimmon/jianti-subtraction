# msg971：结果页 answer_status 字段适配

## 需求（NYX 971 / ABB 921+929）
- `unanswered`：只给思路，不贴对错标签；correct 为空，unlocked=false。
- `answered`：按 correct 正常显示"这个题做对了/做错了"。
- `no_question`（失败）：不动。
- 多题每题各带 `answer_status`；顶层也有，取首题。

## 改动（index.html）
1. `setQuestions(items, topAnswerStatus)`：每题存 `answerStatus = item.answer_status || 顶层 topAnswerStatus`。
2. `renderQuestion()`：verdictLine 在 `answerStatus==='unanswered'` 时显示"无对错 · 只给思路"，answered 保持原逻辑。
3. 轮询调用 `setQuestions(items, data && data.answer_status)` 传顶层字段。

## 真机验证（vivo 1578472099005SJ，横屏 1080×508）
- `01_unanswered_no_verdict.png` SHA256 `7B7E7CACB90571213971DAA452417CA58FA770BF8902AE4C0425A9E721D1A1E1`
  - 第1题 unanswered：显示"无对错 · 只给思路"+"→ 先计算3+4 / 停在写出得数前"，无对错标签。
- `02_answered_verdict.png` SHA256 `8A3969DFCFE58553981264FD058BD3218DF1CD49745D02CA604ECEA2BCF3F7D5`
  - 第2题 answered + correct=true：显示"这个题做对了"+"→ 通分后合并同类项"，原有逻辑不变。
- 验证用 LOCAL_MODE 临时注入两态数据，验证后已恢复 LOCAL_MODE=false、删除测试钩子和 mock 数据，正式 APK 重打包安装。
- 服务端 v9.3 当前无 answer_status 字段，字段上线后 App 自动按三态适配。

## 结论
971 App 端适配完成、真机两态验证通过；no_question 失败分支未动。
