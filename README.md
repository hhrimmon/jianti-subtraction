# 简题（减法版）

拍一张作业照片，手机上只出「思路卡」，答案锁在「看过程与答案」里——一个不把答案直接喂给孩子的自习工具。

本仓库是**减法版**：只保留能跑通的最小闭环，用于比赛提交与代码留痕。研究与实验材料（多提示词组全量、多 web AI 轮询、L0–L3 分层题库、防抄袭/学情分析/错题本全量）**不在本仓库**。

## 组成

| 目录 | 内容 |
| --- | --- |
| `app/` | HarmonyOS NEXT 壳应用（ArkTS），拍题、分卡、锁答案、扣卡、家长确认 |
| `gateway/gateway.py` | 本机网关 `:8000`，`POST /solve` 收图 → 调模型 → 分段 → 输出后校验 → 返回文本块 |
| `gateway/qw_bridge.py` | 网页版模型桥接 `:8001`，Playwright + 本机 Chrome，单账号组，滑块即停 |
| `gateway/autorun.pyw` | 开机自启 launcher，拉起网关与桥接 |
| `gateway/watchdog.py` | 常驻看门狗：探活 8000/8001，挂了自起；子进程用 pythonw + DETACHED_PROCESS，脱离控制台 |
| `gateway/run_gateway.ps1` | 计划任务用的网关重启脚本（幂等，不碰桥接） |

## 数据流

```
手机拍题(jpeg) → 壳 POST 网关 :8000/solve
                    └─ 校验图片 → 调后端
                         ├─ qianwen  → 桥接 :8001 → 本机 Chrome → 网页模型（上传题图，取回答）
                         └─ dashscope→ 云端 API（需 JT_VLM_API_KEY）
                    └─ 分段（第N题 / 第N题(k) / 思路 / 过程与答案）
                    └─ 输出后校验：思路段若命中最终答案特征，只记日志，孩子端给可行动提示
               → 返回纯文本 → 壳 cardsFromText 切卡 → 上屏
```

## 快速运行

前置：DevEco Studio / HarmonyOS SDK、Python 3.10+、本机 Chrome、网页模型账号已登录。

```powershell
# 1) 起网关（生产模式：真实模型，不用假数据）
$env:JT_VLM_BACKEND = "qianwen"
python -u gateway/gateway.py

# 2) 起桥接（另开一个终端）
python -u gateway/qw_bridge.py

# 3) 健康检查
curl.exe http://127.0.0.1:8000/health
curl.exe http://127.0.0.1:8001/health
```

壳应用内 `DEFAULT_URL` 指向网关地址（默认 `http://<本机局域网IP>:8000/solve`），改完重新编译安装 HAP。

## 显示规则（壳侧）

- **标题驱动切卡**：一行标题（如 `【第2题(1)·思路】`）开一张卡，正文归该卡。
- **按小题计数**：`n/N` 以题号去重计数，同一小题的思路卡与过程卡共享序号，解锁不会让 N 翻倍。
- **答案隔离**：思路卡不放最终答案；点「看解题」用一张卡换「过程与答案」卡。
- **免费卡额度**：每日额度由壳本地计，超额走家长确认。

## 已知限制

- 桥接依赖网页端登录态与平台风控：出现滑块/拒答即停该账号组并冷却，不做硬解。
- 输出后校验只拦「思路段泄答案」这一种形态，且拦截后**不阻塞**孩子继续用，漏检靠日志回溯。

## 许可

Apache-2.0，详见 [LICENSE](LICENSE)。比赛提交与评审用途不受限制；其他用途按许可证条款执行。
