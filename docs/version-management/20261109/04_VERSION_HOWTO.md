# 简题 2.0 · 版本管理说明（一页速查）

> 给不写 Git 的人看。规则细节在 `02_VERSION_POLICY.md`，本页只讲「平时怎么做、出事找谁」。
> 制定：WBC，2026-11-09；状态：待 Nyx 验收。

## 0. 先记住三句话

1. **本地出原件，交付包定快照，GitHub 留备份，Gitee 是主账本。**
2. **没验收不进 `main`，没打包不打 tag。**
3. **永远不 force push、不 `reset --hard` 后强推、不改已发的 tag。**

## 1. 平时怎么提交

```bash
# 第一次：把远程仓库拉到一个独立工作副本（不要在 30GB 的工程根目录直接 git init）
git clone https://github.com/hhrimmon/jianti-subtraction.git
cd jianti-subtraction
git checkout -b dev            # 已有 dev 就 git checkout dev && git pull

# 每次交付包验收通过后，把「版本元信息」放进仓库
#   位置固定：docs/version-management/<YYYYMMDD>/
#   内容固定：现状核查表、版本管理办法、当前版本清单、版本管理说明、manifest.json
git add docs/version-management/<YYYYMMDD>/
git commit -m "chore(version): 第NN批版本快照，包SHA <前8位>"
git push origin dev
```

- commit message 必须带**批次号**和**交付包 SHA 前 8 位**，这样出事能一眼对上包。
- 只放纯文本元信息；**不放** `.venv`、`build/`、`node_modules/`、APK/hap、题库、截图、日志、密钥、视频、已烧录固件 bin。
- 推不上去（401/403）就**停下报 Nyx**，不要自己去找 token、不要换账号绕过。

## 2. 怎么打 tag

```bash
git tag -a v2.0.0 -m "简题2.0 正式发布｜第NN批｜包SHA <前8位>｜验收人 Nyx"
git push origin v2.0.0
```

| 什么时候打 | 打什么 |
| --- | --- |
| 产品正式发布 | `v2.0.0`、`v2.0.1` … |
| 预发布 | `v2.0.0-rc.1` |
| 热修 | `v2.0.0+hotfix.1` |
| 只是留个版本管理基线（非发布） | `v2.0.0-baseline-20261109` |
| 网关组件单独封版 | `gw-v3.23-<说明>-<日期>`（仓库现有的 `v3.23-…-taskC-20261030` 是历史命名，保留） |

- 一律用 `-a`（带说明），不用轻量 tag。
- **打 tag 前必须**能在本批交付包的 `manifest.json` 里找到逐文件 SHA，可复算才算数。
- 打错了就新打一个、写清作废，**不要** `git tag -f` 覆盖已发布的。
- 重大发布同时在 GitHub 建 Release，正文照上游写法列「这一版里有什么／边界（照实写）／已知未修项」。

## 3. 怎么回滚

| 想干什么 | 怎么做 | 千万别 |
| --- | --- | --- |
| 拿回某个旧文件 | `git log --oneline -- <路径>` → `git show <提交>:<路径> > 临时文件` → 人工比对后再覆盖（原文件先 `.bak`） | 别用 `git checkout` 直接盖掉没提交的本地改动 |
| 撤销一次已推的提交 | `git revert <提交>` → `git push origin dev`（历史保留） | 别 `reset --hard` + 强推 |
| 整版回到某个 tag | `git checkout -b rollback/<tag>-<日期> <tag>` 拉新分支核对 → 由 Nyx／浩爷决定怎么合 | 别在 `main` 上 `reset --hard <tag>` |
| GitHub 挂了 / 推错了 | 用**本地交付包 zip**逐文件还原（包内 `manifest.json` 可复算） | 别以 GitHub 覆盖 Gitee |

**兜底顺序**：本地交付包 → GitHub tag → Gitee 主账本。三方冲突以 **Gitee 为准**，报 Nyx 裁定后执行。

## 4. 出了问题找谁

| 问题 | 找谁 | 怎么报 |
| --- | --- | --- |
| 这批该不该进 `main`、什么时候打 tag、以哪份为准 | **Nyx**（Queen） | 在协同线程发 `action:report`，附交付包字节＋SHA |
| GitHub 登录失效、要授权、要开分支保护、要建 Release | **浩爷** | 由 Nyx 转达；WBC 不直接找浩爷、不自己要 token |
| Gitee 与 GitHub 内容不一致 | **Nyx 裁定**，按 Gitee 为准 | 出三方对账表再定 |
| 包里 SHA 对不上 / 文件缺失 | **WBC 自查并重打包**，同时报 Nyx | 不静默修包，必须留修订记录 |
| 轮询、值班、任务派发 | **Nyx** | Worker → Queen → 人类，不越级 |

## 5. 每 10 批做一次对账

把 Gitee 主账本、GitHub 远程、本地清单三方拉齐，出一张差距表，写清：哪边新、哪边旧、哪些文件只在本地、下一个 tag 该打在哪。对账记录随当批交付包一起交。

---

**一句话**：本说明解决的是「不怕丢东西」；代码写得好不好、文档对不对，是别的批次的事，本批不管。
