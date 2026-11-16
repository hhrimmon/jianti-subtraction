# dsh Windows 测试机适配备注（跑 abb 服务器端 v1 冒烟套件用）

> 适用：在 Windows 机器上用 Git-bash 跑 `tests/run_smoke.sh`（186 项）。
> Linux 部署机不需要本文件任何一条；这里只记 dsh 这台 Windows 测试机的通道差异。
> 实测基线：三项适配齐后 PASS 184 / FAIL 2，两处 FAIL 是本机 curl 通道限制、非代码 bug（见 §5）。
> 红线：不编假数据、不动线上配置、不碰 token/秘密；本文件无凭证。

## 0. 一键跑法（照抄可用）

```bash
# Git-bash 里，仓库解压到 /d/.../unpacked 后：
cd unpacked
export PATH="/d/AItools/projects/jt2.0/_work/dsh-verify-20261116/pyshim:$PATH"
export PYTHONUTF8=1
sed -i 's/\r$//' tests/run_smoke.sh    # 去 CRLF（仅当 zip 解出来带 \r 时）
bash tests/run_smoke.sh
```

前置：§1 shim 目录、§2 junction 已建好（一次性，之后复用即可）。

## 1. python3 shim（Git-bash 无 python3，只有 python）

run_smoke.sh 的 `jget()` 用 `python3` 解析 JSON。这台机 PATH 里只有
`F:\Python\python.exe`（3.12）。建一个 shim 目录，里面放一个无扩展名文件 `python3`：

```
#!/bin/bash
exec /f/Python/python.exe "$@"
```

`chmod +x python3`，把它 prepend 进 PATH。不改 abb 一行代码。

## 2. /tmp 双世界问题（最大坑）

- Git-bash 的 `/tmp` → `C:\Users\Administrator\AppData\Local\Temp`
- 原生 Windows python 打开 `'/tmp/...'` → 按**当前盘**解析成 `D:\tmp\...`（cwd 在 D:）

curl 写 RESP 进 bash 的 /tmp、python3 却去 D:\tmp 读 → 读不到，`jget` 的
`2>/dev/null` 把异常吞成空串 → TOKEN/balance 全取空 → 连锁 401，首跑只过 34/152。
修法：一条 junction 把两个世界对齐（需管理员视策略，dsh 机可直接成）：

```
cmd /c mklink /J D:\tmp "C:\Users\Administrator\AppData\Local\Temp"
```

验证：bash `printf hi > /tmp/x` 后 `python -c "print(open('/tmp/x').read())"` 能读出即对齐。

## 3. PYTHONUTF8=1（GBK 默认编码）

服务端响应全 UTF-8 含中文。原生 python 默认按系统码页(GBK)打开
`/tmp/v1test-resp.json`，`json.load` 抛 UnicodeDecodeError 又被 `2>/dev/null` 吞 → 全链取空。
`export PYTHONUTF8=1` 后此项根除（或 `python -X utf8`）。
同理：读服务 stderr 日志时带中文（如"快照损坏…"）在 PowerShell 里显示乱码，
是 GBK 控制台问题，不是日志坏了；python 端加 UTF-8 读即可。

## 4. CRLF

zip 解出的 .sh 若带 \r，bash 报 `syntax error near unexpected token`。
`sed -i 's/\r$//' tests/run_smoke.sh` 先去一遍。写 .sh/.mjs 用 LF。

## 5. 命令行中文被 curl 转 GBK（唯一残留 FAIL 的根因）

Windows 原生 curl（含 mingw 版走 argv 的路径）把命令行里的中文按 ANSI 码页转码后发送：
`-d '{"memo":"今天也要加油。"}'` 上行字节变成 GBK（抓包证实 `bdf1cc…`，非 UTF-8 `e4b88a…`），
服务端按 UTF-8 解 JSON → 400 invalid_json。`LANG/LC_ALL` 改了也没用（转码发生在 Windows
API 层）。**不是服务端 bug**：同一串 UTF-8 字节用文件体 `--data-binary @utf8file` 直发即 200、
数据正确入库回读。
- 影响：run_smoke 186 项里仅 MEMO 两项（421/424 行中文 -d）。判 PASS 184/2 时按此定性。
- 测试机解法（不改套件）：把含中文 body 的 curl 换 `--data-binary @file`（file 写 UTF-8 无 BOM）；
  或 dsh 用 python urllib 发（send_chat.py 即此法，UTF-8 干净）。

## 6. github.com 直连被重置（取包通道）

这台机 IE 层配了系统代理 127.0.0.1:20808（WinHTTP 却是 Direct），git 不走 IE 代理
→ 直连 github.com 常被重置；api.github.com 正常。解法：单次命令带一次性代理参数，
不动任何全局配置：

```
git -c http.proxy=http://127.0.0.1:20808 -c https.proxy=http://127.0.0.1:20808 \
    fetch origin dev
```

首次失败就重试一次（代理偶发慢）。克隆过：_work/github/jianti-subtraction。

## 7. 自清纪律

测试实例全部自起自清（terminate + 端口 18001–18010 释放确认）；数据只写 /tmp 下
dsh-* 临时目录；不碰 data/ 正式快照、不碰线上；探针脚本与原始输出留档
_work/dsh-verify-20261116/（conc_probe1/2/3、persist_probe1/2、wfailprobe3、
torn_probe、audit.py、routes.py、smoke-run*.log）。
