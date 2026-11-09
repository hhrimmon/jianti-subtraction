# 版本管理 · 2026-11-09（第 48 批 WBC）

本目录是 WBC 兼职版本管理的**版本元信息基线**，只含纯文本与可复算证据，不含代码、二进制、密钥。

| 文件 | 内容 |
| --- | --- |
| 01_STATUS_CHECK.md | GitHub 仓库与本地现状核查表、8 项差距 D1-D8、写入通道实测 |
| 02_VERSION_POLICY.md | 版本管理办法 v1.0：分工、分支策略、tag 规则、同步时机、回滚三档 |
| 03_BASELINE_INVENTORY.md | 当前版本逐文件清单（位置 / 字节 / SHA-256 / 版本号） |
| 04_VERSION_HOWTO.md | 一页速查：怎么提交、怎么打 tag、怎么回滚、出了问题找谁 |
| 10_EVIDENCE_GITHUB_API.json | GitHub 公开 REST 探测摘要 |
| 11_EVIDENCE_TREE.json | main 完整树统计（924 节点未截断） |
| 12_EVIDENCE_LOCAL_BASELINE.json | 本地基线逐文件 SHA-256（可复算） |
| 13_EVIDENCE_UPSTREAM_SHA.json | 上游 5 件文件 SHA-256 对照 |

边界：只做版本管理，不改代码、不改产品文档内容；Gitee 仍是主账本，本目录不替代 Gitee；仓库 Apache-2.0，GPL 系工具只独立使用、不并入。
状态：待 Nyx 验收。对应交付包 第48批_WBC_GitHub版本管理_20261109.zip。
