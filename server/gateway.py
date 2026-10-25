#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""简题 v0.2 网关（PC 端，局域网联调版）

用途：设备拍题后经 WiFi 把灰度 JPEG POST 到本机，本网关收图、
调视觉模型、把答案 JSON 返回给设备回屏显示。

运行（零依赖，只需 Python 3.8+，不用 pip 装任何东西）：
    python gateway.py            # 默认监听 0.0.0.0:8000
    python gateway.py --port 8000

启动后会打印本机局域网 IP，固件 v0.2 就往这个地址 POST：
    POST http://<局域网IP>:8000/solve
    Content-Type: image/jpeg
    Body: 灰度 JPEG 原始字节

返回 JSON：
    {"ok": true, "text": "答案文本", "latency_ms": 123}
    失败时：{"ok": false, "error": "原因"}

推理（infer_image）：
    - JT_VLM_BACKEND=dashscope（默认）：设了 JT_VLM_API_KEY（DashScope key）
      时调 Qwen-VL 模型（默认 qwen-vl-max，可用 JT_VLM_MODEL 改），走 DashScope
      兼容模式 chat completions 接口，图片以 base64 data URL 传入。
      没设 key 时返回占位答案（只用于链路测试），并在日志里明说。
    - JT_VLM_BACKEND=ollama：调本机 Ollama（http://localhost:11434/v1，
      无需 key），模型默认 qwen2.5vl:3b（需先 ollama pull qwen2.5vl:3b）。
      注意：Ollama 里纯文字模型（如 qwen2.5-coder）看不了图，必须用带 vision
      的视觉模型；本地出答案慢是正常的，第一张可能要一两分钟。
    - key 只存在运行网关的本机环境变量里，不要写进代码、不要发给作者。
    - 模型原始回复存在 uploads/ 目录（jt_<时间戳>.json），便于复查。

健康检查：GET /health -> {"ok": true}
收到的图存在 uploads/ 目录，文件名 jt_<时间戳>.jpg。
"""

import argparse
import random
import re
import sys
import threading
import base64
import json
import os
import socket
import time
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
UPLOAD_DIR = os.path.join(BASE_DIR, "uploads")
MAX_IMAGE_BYTES = 2 * 1024 * 1024  # 2MB 上限，灰度图 110KB 左右绰绰有余

DEFAULT_BASE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1"
DEFAULT_MODEL = "qwen-vl-max"
DEFAULT_OLLAMA_BASE_URL = "http://localhost:11434/v1"
DEFAULT_OLLAMA_MODEL = "qwen2.5vl:3b"
MODEL_TIMEOUT_S = 60
OLLAMA_TIMEOUT_S = 300
QW_TIMEOUT_S = 300

# 网关版本号：/health 和 /solve 都会返回它，Trae 凭此确认跑的是哪一版，
# 避免旧进程未杀、新实例被单实例保护挡掉后仍跑旧版的混乱。
GATEWAY_VERSION = "v3.16-bridgeerr"


# 提示词（v3.14：按 Muse 审核意见补齐降级池护栏）：
# 不变量：【第N题·思路】/【第N题·过程与答案】两段结构、第一行直接进题、禁整卷总思路。
# 本次补齐：思路禁答案（含答案定义）、括号小题切分、题号沿用（原题号优先，无题号才
# 顺序编，化解「第一行第1题」与「沿用原题号」打架）、填空与大题区分、禁跨题引用、
# 尾禁令（固定在末尾不参与打乱——开头禁令管不住结尾汇总）。
_G_OPEN = ["能分析吗，这是什么题，答案是什么。",
           "请分析图片里的题目并给出答案。",
           "这是什么题？怎么解？"]
_G_STRUCT = [
    "有多道题时严格按题分块，每题用【第N题·思路】和【第N题·过程与答案】两个小标题，标题独占一行。",
    "多道题时逐题分块，每题只用【第N题·思路】与【第N题·过程与答案】两个小标题，各占一行。",
    "若有多道题，严格一题一块，标题只用【第N题·思路】和【第N题·过程与答案】，标题独占一行。",
]
_G_BAN = [
    "禁止写整卷总思路、开头总述、「本组题目考查什么」之类的总起段落，第一行就直接从第一道题的小标题开始。",
    "不要整卷总思路、开头总述或「本组题目考查什么」式总起，第一行从第一道题的小标题写起。",
    "严禁整卷总思路、开头总述、「本组题目考查什么」这类开场，直接从第一道题的小标题写起。",
]
_G_QID = [
    "标题题号沿用图片里印的原题号；图片没印题号时才从 1 开始按顺序编号。",
    "【第N题】的 N 用试卷原题号；试卷没编题号时才按顺序从 1 编起。",
    "题号以图片中印的原题号为准；没有印题号时才按顺序从 1 编号。",
]
_G_SUBQ = [
    "大题含 (1)(2)(3) 小题时按小题再分块，标题用【第N题(k)·思路】与【第N题(k)·过程与答案】，k 为小题号。",
    "遇 (1)(2)(3) 小题要逐小题分块，小标题写作【第N题(k)·思路】和【第N题(k)·过程与答案】。",
    "有小题（(1)(2)(3)）的大题按小题拆块，标题格式【第N题(k)·思路】/【第N题(k)·过程与答案】。",
]
_G_NOANS = [
    "思路段只讲方法、切入点和用什么公式定理，不得出现最终答案；最终答案包括数字结果、选项字母、填空内容、方程的解、化简结果与最终结论，也不得变相提示（如「代入即得 15」「显然选 B」）。",
    "思路里只写解题方法与入手方向，不得写出最终答案——数字结果、选项字母、填空内容、方程的解、化简结果、最终结论都算最终答案，变相提示（如「一算便知是 15」）也算。",
    "思路段仅给方法和思路框架，严禁出现最终答案（数字结果、选项字母、填空内容、方程的解、化简结果、最终结论），也不得以任何形式暗示答案。",
]
_G_FILL = [
    "填空、选择、判断题的最终答案是要填的内容、选项字母和判断结论；计算与解答大题的最终答案是最终数值与结论。两种题型的思路段都不得写出答案。",
    "注意区分题型：填空选择判断题答案是所填内容、选项字母或判断结论，计算解答大题答案是最终数值与结论；两类题的思路段都不得给答案。",
]
_G_NOCROSS = [
    "各题、各小题独立成段：思路段不得引用其他题或其他小题的答案与中间结果（如「由(1)得 15」「同第2题」）；确实需要时在本题「过程与答案」段内把推导完整重写。",
    "题与题、小题与小题之间不得互相引用答案或中间结果；必须引用时，在「过程与答案」段内完整重推一遍，思路段一律不引用。",
]
_G_ORDER = ["每题先写思路再写过程与答案。", "先思路后过程与答案。", "每题先思路、再过程与答案。"]
_G_TAIL = [
    "最后重申：思路段只讲方法，任何最终答案（数字结果、选项字母、填空内容、方程的解、结论）只能出现在「过程与答案」段，思路段出现答案即视为作废重写。",
    "再次强调：最终答案（数字结果、选项字母、填空内容、方程的解、结论）只能写在「过程与答案」段；思路段只讲方法，出现答案按作废处理。",
]


def grade_prompt():
    mid = [random.choice(_G_STRUCT), random.choice(_G_BAN), random.choice(_G_QID),
           random.choice(_G_SUBQ), random.choice(_G_NOANS), random.choice(_G_FILL),
           random.choice(_G_NOCROSS), random.choice(_G_ORDER)]
    random.shuffle(mid)
    # 尾禁令固定在末尾，不参与打乱（开头禁令管不住结尾汇总）。
    return random.choice(_G_OPEN) + "".join(mid) + random.choice(_G_TAIL)


def _strip_fences(text):
    """剥掉模型常加的 ```json ... ``` 外框（不剥就解析失败、屏上显示原文）。"""
    t = text.strip()
    if not t.startswith("```"):
        return t
    nl = t.find("\n")
    if nl != -1:
        t = t[nl + 1:]
    else:
        t = t[3:]
    if t.rstrip().endswith("```"):
        t = t.rstrip()[:-3]
    return t.strip()


def _parse_lenient(stripped):
    """容错解析：标准 -> 允许串内真换行 -> 从第一个 { 起取首个完整对象。"""
    try:
        return json.loads(stripped)
    except (ValueError, TypeError):
        pass
    try:
        return json.loads(stripped, strict=False)
    except (ValueError, TypeError):
        pass
    start = stripped.find("{")
    if start != -1:
        try:
            parsed, _ = json.JSONDecoder().raw_decode(stripped[start:])
            return parsed
        except (ValueError, TypeError):
            pass
    return None


def _salvage_field(text, keys):
    """尽力从残缺 JSON 里抠一个字段的字符串值（对象被截断时用）。"""
    for key in keys:
        needle = '"' + key + '"'
        i = text.find(needle)
        if i == -1:
            continue
        j = text.find(':', i + len(needle))
        if j == -1:
            continue
        j += 1
        while j < len(text) and text[j] in ' \t\r\n':
            j += 1
        if j >= len(text) or text[j] != '"':
            continue
        j += 1
        out = []
        closed = False
        while j < len(text):
            ch = text[j]
            if ch == '\\' and j + 1 < len(text):
                out.append(text[j:j + 2])
                j += 2
                continue
            if ch == '"':
                closed = True
                break
            out.append(ch)
            j += 1
        val = ''.join(out)
        if closed:
            try:
                return json.loads('"' + val + '"')
            except (ValueError, TypeError):
                return val
        val = val.strip().rstrip(',').rstrip('}').strip()
        if val:
            return val
    return None

def parse_model_text(content):
    """把模型返回解析成给设备的答案文本。

    新格式 {"seen": ..., "feedback": ...} -> “看到：...\n...”；
    旧格式 {"has_answer": ..., "feedback": ...} -> feedback；
    非 JSON -> 原文。
    """
    if not isinstance(content, str):
        return json.dumps(content, ensure_ascii=False)
    stripped = _strip_fences(content)
    parsed = _parse_lenient(stripped)
    if not isinstance(parsed, dict):
        seen = _salvage_field(stripped, ["seen"])
        feedback = _salvage_field(stripped, ["feedback", "answer"])
        parts = []
        if seen:
            parts.append("看到：" + str(seen))
        if feedback:
            parts.append(str(feedback))
        if parts:
            return "\n".join(parts)
        return stripped
    parts = []
    seen = parsed.get("seen")
    if seen:
        parts.append("看到：" + str(seen))
    # 小模型常把键写成 answer，或把一层 JSON 字符串嵌在 answer 里；都兼容。
    feedback = parsed.get("feedback")
    if feedback is None:
        feedback = parsed.get("answer")
    if isinstance(feedback, str):
        inner = feedback.strip()
        if inner.startswith("{"):
            try:
                inner_parsed = json.loads(inner)
                if isinstance(inner_parsed, dict):
                    inner_fb = inner_parsed.get("feedback")
                    if inner_fb is None:
                        inner_fb = inner_parsed.get("answer")
                    if inner_fb:
                        feedback = inner_fb
            except (ValueError, TypeError):
                pass
    if feedback:
        parts.append(str(feedback))
    if parts:
        return "\n".join(parts)
    return stripped


# ---------- 输出后校验：思路段泄露最终答案则换提示，不进免费卡 ----------
# 背景（Muse 审核 C/D）：降级池曾经没有「思路禁答案」，模型会把答案写进免费思路卡，
# 绕过解题卡。这里做兜底：返回设备前逐段扫描，思路段命中最终答案特征就把该段正文
# 换成一句孩子能照着做的话（标题保留、切卡结构不变），原文与命中特征记
# validate_blocked.jsonl 供家长/开发回看。
# 原则（用户定）：漏了就记日志，不硬堵、不等人工——孩子端永远有下一步可走。
VALIDATE_LOG = os.path.join(BASE_DIR, "validate_blocked.jsonl")

# 孩子端提示：可行动，不提「校验/暂扣/人工」这些内部词
VALIDATE_KID_NOTICE = "（这题思路先跳过：点「看过程与答案」继续，或点「下一题」换一道）"

# 段标题：兼容【第N题·思路】/【第N题(k)·思路】与旧格式「第N题 思路」
_HEAD_RE = re.compile(
    r"^【?第\s*(\d+)\s*题\s*(?:[（(]\s*(\d+)\s*[）)])?\s*[·\-—:：]?\s*"
    r"(思路|过程与答案|过程|答案)】?\s*$")

# 思路段最终答案特征（命中任一即换提示；不漏放，原文只进日志）
_ANS_PATTERNS = [
    ("答案字样", re.compile(r"答\s*案\s*[是为：:]?\s*\S")),
    ("结论字样", re.compile(r"(结\s*论|最终结果|最终答案)\s*[是为：:]?\s*\S")),
    ("选项直给", re.compile(r"选\s*[A-Ea-e](?![a-zA-Z])")),
    ("填空直给", re.compile(r"(应\s*填|填\s*入|填\s*[:：])\s*[「\"']?\S")),
    ("方程解", re.compile(r"[a-zA-Z]\s*[=＝]\s*-?\d+(\.\d+)?")),
    ("数值结果", re.compile(r"[=＝]\s*-?\d+(\.\d+)?(\s*[a-zA-Z一-鿿]{0,4})?\s*[。\.；;，,]?\s*$")),
]


def validate_free_sections(text, saved_path=None):
    """返回 (放行文本, 报告|None)。思路段命中答案特征：正文暂扣换说明，其余原样。"""
    if not isinstance(text, str) or not text.strip():
        return text, None
    lines = text.split("\n")
    out, blocked = [], []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        m = _HEAD_RE.match(line.strip())
        if m and m.group(3) == "思路":
            j = i + 1
            body_lines = []
            while j < n and not _HEAD_RE.match(lines[j].strip()):
                body_lines.append(lines[j])
                j += 1
            hit = None
            for name, pat in _ANS_PATTERNS:
                for ln in body_lines:
                    if pat.search(ln):
                        hit = (name, ln.strip())
                        break
                if hit:
                    break
            if hit:
                out.append(line)
                out.append(VALIDATE_KID_NOTICE)
                blocked.append({
                    "qid": "第%s题%s" % (m.group(1), "(%s)" % m.group(2) if m.group(2) else ""),
                    "section": line.strip()[:60],
                    "pattern": hit[0], "line": hit[1][:120],
                    "excerpt": "\n".join(body_lines).strip()[:300],
                })
            else:
                out.append(line)
                out.extend(body_lines)
            i = j
        else:
            out.append(line)
            i += 1
    if not blocked:
        return text, None
    rec = {"ts": int(time.time()), "saved": saved_path, "blocked": blocked}
    try:
        with open(VALIDATE_LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception as exc:
        print("validate log write failed: %s" % exc, flush=True)
    for b in blocked:
        print("输出校验拦下 %s 思路段（命中「%s」: %s）"
              % (b["qid"], b["pattern"], b["line"][:60]), flush=True)
    return "\n".join(out), {"blocked": blocked}


def call_model(jpeg_bytes):
    """调模型（DashScope 云端 或 本地 Ollama），返回 (原始文本, key_err, 计时)。

    计时：仅千问网页后端返回 {"upload_ms":..., "answer_ms":...}，其余为 None。
    """
    backend = os.environ.get("JT_VLM_BACKEND", "dashscope").strip().lower() or "dashscope"
    timing = None
    if backend == "qianwen":
        # 千问网页版：转发给本地 Playwright 桥接（qw_bridge.py，默认 127.0.0.1:8001）
        bridge = os.environ.get("JT_QW_BRIDGE", "http://127.0.0.1:8001/solve").strip()
        req = urllib.request.Request(bridge, data=jpeg_bytes,
                                     headers={"Content-Type": "image/jpeg"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=QW_TIMEOUT_S) as resp:
                body = json.loads(resp.read().decode("utf-8"))
        except Exception as exc:
            # 桥接的失败原因要透传给手机：冷却/拒答/掉线都写成一句人话，
            # 不能只剩一个 HTTP 500（孩子那头只会看到"失败"，家长也查不到原因）
            detail = ""
            reader = getattr(exc, "read", None)
            if callable(reader):
                try:
                    detail = (json.loads(reader().decode("utf-8")) or {}).get("error") or ""
                except Exception:
                    detail = ""
            if detail:
                # 桥接已经带了「千问桥接失败：」前缀，别叠两层
                raise RuntimeError(detail if detail.startswith("千问桥接") else "千问桥接失败：%s" % detail)
            raise RuntimeError("千问桥接不可用：%s" % exc)
        if not body.get("ok"):
            raise RuntimeError("千问桥接返回失败：%s" % body.get("error"))
        timing = {"upload_ms": body.get("upload_ms"), "answer_ms": body.get("answer_ms"),
                  "shots": body.get("shots") or []}
        return body.get("text", ""), None, timing
    if backend == "ollama":
        base_url = os.environ.get("JT_VLM_BASE_URL", DEFAULT_OLLAMA_BASE_URL).strip() or DEFAULT_OLLAMA_BASE_URL
        model = os.environ.get("JT_VLM_MODEL", DEFAULT_OLLAMA_MODEL).strip() or DEFAULT_OLLAMA_MODEL
        timeout = OLLAMA_TIMEOUT_S
        headers = {"Content-Type": "application/json"}
    elif backend == "dashscope":
        api_key = os.environ.get("JT_VLM_API_KEY", "").strip()
        if not api_key:
            return None, "JT_VLM_API_KEY 未设置", None
        base_url = os.environ.get("JT_VLM_BASE_URL", DEFAULT_BASE_URL).strip() or DEFAULT_BASE_URL
        model = os.environ.get("JT_VLM_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL
        timeout = MODEL_TIMEOUT_S
        headers = {"Content-Type": "application/json", "Authorization": "Bearer " + api_key}
    else:
        raise RuntimeError("JT_VLM_BACKEND 不支持：%s（只能填 dashscope、ollama 或 qianwen）" % backend)
    url = base_url.rstrip("/") + "/chat/completions"
    b64 = base64.b64encode(jpeg_bytes).decode("ascii")
    payload = {
        "model": model,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "image_url",
                     "image_url": {"url": "data:image/jpeg;base64," + b64}},
                    {"type": "text", "text": grade_prompt()},
                ],
            }
        ],
        "temperature": 0.2,
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "ignore")[:300]
        raise RuntimeError("模型接口 HTTP %s：%s" % (exc.code, detail))
    except Exception as exc:
        raise RuntimeError("模型调用失败（%s 后端）：%s" % (backend, exc))
    try:
        content = body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise RuntimeError("模型返回结构异常：%s" % json.dumps(body, ensure_ascii=False)[:300])
    if isinstance(content, list):  # 有些返回把 content 包成数组
        content = "".join(
            part.get("text", "") for part in content if isinstance(part, dict))
    return content, None, timing


def infer_image(jpeg_bytes, saved_path):
    """返回 (给设备的答案文本, 计时)；同时把模型原始回复落盘到 uploads/。

    有 key -> 真模型；没 key -> 占位答案（链路测试用，日志明说）。
    """
    content, key_err, timing = call_model(jpeg_bytes)
    if key_err:
        print("注意：%s，返回占位答案（仅链路测试用）" % key_err, flush=True)
        return ("链路已通（占位答案）：网关已收到图片，设置 JT_VLM_API_KEY 后返回真实批改结果。",
                None)
    json_path = os.path.splitext(saved_path)[0] + ".json"
    with open(json_path, "w", encoding="utf-8") as f:
        f.write(content if isinstance(content, str) else json.dumps(content, ensure_ascii=False))
    return parse_model_text(content), timing


# ---------- 家长申请单（卡用完流程：孩子申请 -> 家长网页批准发卡） ----------
REQ_PATH = os.path.join(BASE_DIR, "parent_requests.json")
REQ_LOCK = threading.Lock()
REQ_AMOUNTS = (1, 3, 5)  # 家长在批准页点按钮时自选发几张


def _load_reqs():
    """读申请单文件；不存在或损坏都当空表，不炸请求。"""
    try:
        with open(REQ_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            return data
    except Exception:
        pass
    return []


def _save_reqs(reqs):
    """先写临时文件再替换，避免半截文件。"""
    tmp = REQ_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(reqs, f, ensure_ascii=False, indent=1)
    os.replace(tmp, REQ_PATH)


PARENT_PAGE = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="10">
<title>简题 · 家长批准</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto;padding:0 12px;background:#f4f6f8}
h2{margin:0 0 6px}.tip{color:#666;font-size:13px;margin:0 0 16px}
.card{background:#fff;border-radius:10px;padding:14px 16px;margin-bottom:10px}
.card .p{color:#885500;font-weight:bold}
.card.done{color:#666}
button{font-size:15px;padding:8px 16px;margin:8px 8px 0 0;border:0;border-radius:8px;
background:#1a5fb4;color:#fff}
</style></head>
<body>
<h2>简题 · 家长批准页</h2>
<p class="tip">孩子端「解题卡已用光」时提交的申请会出现在这里，点按钮即发对应张数。已取消的申请不再显示。本页每 10 秒自动刷新。</p>
<div id="list">加载中…</div>
<script>
async function load(){
  try{
    const r = await fetch('/api/parent_request');
    const j = await r.json();
    const reqs = (j.requests||[]).slice().reverse();
    let h='';
    for(const it of reqs){
      if(it.status==='cancelled') continue;
      const t=new Date((it.ts||0)*1000).toLocaleString();
      if(it.status==='pending'){
        h+='<div class="card"><b>'+t+'</b> <span class="p">待批准</span><br>发卡：'
          +'<button onclick="ap(\\''+it.id+'\\',1)">发 1 张</button>'
          +'<button onclick="ap(\\''+it.id+'\\',3)">发 3 张</button>'
          +'<button onclick="ap(\\''+it.id+'\\',5)">发 5 张</button></div>';
      }else{
        h+='<div class="card done"><b>'+t+'</b> 已批准 +'+it.amount+' 张</div>';
      }
    }
    if(!h) h='<p>（暂无申请）</p>';
    document.getElementById('list').innerHTML=h;
  }catch(e){document.getElementById('list').innerHTML='<p>加载失败：'+e+'</p>';}
}
async function ap(id,amount){
  const r=await fetch('/api/parent_approve',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({id:id,amount:amount})});
  const j=await r.json();
  if(j.ok){load();}else{alert('批准失败：'+(j.error||'unknown'));}
}
load();
</script>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 精简日志，只打一行
        print("[%s] %s" % (time.strftime("%H:%M:%S"), fmt % args), flush=True)

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html, status=200):
        body = html.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        """读 JSON 请求体（申请单接口用）；空/非法都返回 None。"""
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            return None
        if length <= 0 or length > 4096:
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception:
            return None

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            self._send_json({"ok": True, "gateway": GATEWAY_VERSION})
        elif path == "/api/parent_request":
            with REQ_LOCK:
                reqs = _load_reqs()
            self._send_json({"ok": True, "requests": reqs})
        elif path == "/parent":
            self._send_html(PARENT_PAGE)
        else:
            self._send_json({"ok": False, "error": "unknown path"}, status=404)

    def _api_parent_request(self):
        """孩子端提交申请单：登记为 pending，等家长在 /parent 批准。
        幂等：同设备已有 pending 单时不重复建单，返回既有单 dup:true。"""
        body = self._read_json() or {}
        device = str(body.get("device", "phone"))[:32]
        with REQ_LOCK:
            reqs = _load_reqs()
            for it in reqs:
                if it.get("device") == device and it.get("status") == "pending":
                    self._send_json({"ok": True, "id": it["id"], "status": "pending", "dup": True})
                    return
            rid = "req-%d-%04d" % (int(time.time()), random.randint(0, 9999))
            item = {
                "id": rid,
                "ts": int(time.time()),
                "device": device,
                "status": "pending",
                "amount": 0,
                "approve_ts": 0,
            }
            reqs.append(item)
            _save_reqs(reqs)
        print("家长申请单 %s 已登记（待批准）" % rid, flush=True)
        self._send_json({"ok": True, "id": rid, "status": "pending"})

    def _api_parent_cancel(self):
        """孩子端撤回申请：pending -> cancelled。已取消的单家长页不再显示、不得再批。"""
        body = self._read_json()
        if not isinstance(body, dict):
            self._send_json({"ok": False, "error": "bad json"}, status=400)
            return
        rid = str(body.get("id", ""))
        with REQ_LOCK:
            reqs = _load_reqs()
            hit = None
            for it in reqs:
                if it.get("id") == rid:
                    if it.get("status") == "approved":
                        self._send_json({"ok": False, "error": "该申请单已批准，不能取消"}, status=409)
                        return
                    if it.get("status") == "cancelled":
                        self._send_json({"ok": True, "id": rid, "status": "cancelled"})
                        return
                    it["status"] = "cancelled"
                    it["cancel_ts"] = int(time.time())
                    hit = it
                    break
            if hit is None:
                self._send_json({"ok": False, "error": "申请单不存在"}, status=404)
                return
            _save_reqs(reqs)
        print("家长申请单 %s 已取消" % rid, flush=True)
        self._send_json({"ok": True, "id": rid, "status": "cancelled"})

    def _api_parent_approve(self):
        """家长网页批准：发 amount 张（1/3/5），孩子端轮询到账。"""
        body = self._read_json()
        if not isinstance(body, dict):
            self._send_json({"ok": False, "error": "bad json"}, status=400)
            return
        rid = str(body.get("id", ""))
        try:
            amount = int(body.get("amount", 0))
        except (TypeError, ValueError):
            amount = 0
        if amount not in REQ_AMOUNTS:
            self._send_json({"ok": False, "error": "amount 只能是 1/3/5"}, status=400)
            return
        with REQ_LOCK:
            reqs = _load_reqs()
            hit = None
            for it in reqs:
                if it.get("id") == rid:
                    if it.get("status") == "approved":
                        self._send_json({"ok": False, "error": "该申请单已批准过"}, status=409)
                        return
                    if it.get("status") == "cancelled":
                        self._send_json({"ok": False, "error": "该申请单已取消，不能批准"}, status=409)
                        return
                    it["status"] = "approved"
                    it["amount"] = amount
                    it["approve_ts"] = int(time.time())
                    hit = it
                    break
            if hit is None:
                self._send_json({"ok": False, "error": "申请单不存在"}, status=404)
                return
            _save_reqs(reqs)
        print("家长申请单 %s 已批准 +%d 张" % (rid, amount), flush=True)
        self._send_json({"ok": True, "id": rid, "amount": amount})

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/parent_request":
            self._api_parent_request()
            return
        if path == "/api/parent_approve":
            self._api_parent_approve()
            return
        if path == "/api/parent_cancel":
            self._api_parent_cancel()
            return
        if path != "/solve":
            self._send_json({"ok": False, "error": "unknown path"}, status=404)
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_IMAGE_BYTES:
            self._send_json({"ok": False, "error": "empty or oversize image"}, status=400)
            return
        jpeg_bytes = self.rfile.read(length)
        if not jpeg_bytes.startswith(b"\xff\xd8") or not jpeg_bytes.endswith(b"\xff\xd9"):
            self._send_json({"ok": False, "error": "not a jpeg"}, status=400)
            return
        os.makedirs(UPLOAD_DIR, exist_ok=True)
        saved_path = os.path.join(UPLOAD_DIR, "jt_%d.jpg" % int(time.time()))
        with open(saved_path, "wb") as f:
            f.write(jpeg_bytes)
        if os.environ.get("JT_SOLVE_FAKE") == "1":
            # 验收假解题：不碰真实模型，直接回三题罐头结果（文本切卡路径）
            fake_text = (
                "第1题 思路\n凑十法，把 8 拆成 5 和 3。\n"
                "第1题 过程与答案\n7+8=15\n"
                "第2题 思路\n先算个位，再算十位。\n"
                "第2题 过程与答案\n23+15=38\n"
                "第3题 思路\n移项要变号。\n"
                "第3题 过程与答案\nx=6\n"
            )
            print("saved %s (%d bytes) -> FAKE response (JT_SOLVE_FAKE=1)" % (saved_path, len(jpeg_bytes)), flush=True)
            self._send_json({"ok": True, "text": fake_text, "latency_ms": 1, "gateway": GATEWAY_VERSION + "+fake"})
            return
        t0 = time.time()
        try:
            text, timing = infer_image(jpeg_bytes, saved_path)
        except Exception as exc:  # 模型异常不炸链路，照实返回
            self._send_json({"ok": False, "error": "infer failed: %s" % exc}, status=500)
            return
        # 输出后校验：思路段疑似含最终答案则暂扣该段正文，不进免费卡
        text, vrep = validate_free_sections(text, saved_path)
        latency_ms = int((time.time() - t0) * 1000)
        print("saved %s (%d bytes) -> %d ms" % (saved_path, len(jpeg_bytes), latency_ms), flush=True)
        out = {"ok": True, "text": text, "latency_ms": latency_ms, "gateway": GATEWAY_VERSION}
        if vrep:
            # 报回样例与校验结果（完整原文见 validate_blocked.jsonl）
            out["validate"] = {"blocked": [{"qid": b["qid"], "pattern": b["pattern"],
                                            "line": b["line"]} for b in vrep["blocked"]]}
        if timing:
            shots = timing.pop("shots", None)
            out.update(timing)
            if shots:
                out["shots"] = shots
                # 分段原图落盘，便于验收核对
                base = os.path.splitext(saved_path)[0]
                for i, shot in enumerate(shots):
                    try:
                        png = base64.b64decode(shot.get("png_b64", ""))
                        label = "".join(c for c in shot.get("label", "seg") if c.isalnum() or c in "·-")
                        with open("%s_shot%02d_%s.png" % (base, i, label), "wb") as f:
                            f.write(png)
                    except Exception as exc:
                        print("save shot %d failed: %s" % (i, exc), flush=True)
        self._send_json(out)


def lan_ip():
    """取本机局域网 IP（固件要往这个地址 POST）。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    # 防重开：同机同端口已有网关在跑时直接退出（Windows 下 SO_REUSEADDR
    # 可能让两个进程同时占住一个端口，请求会随机落到旧进程上）。
    try:
        probe = urllib.request.Request("http://127.0.0.1:%d/health" % args.port)
        with urllib.request.urlopen(probe, timeout=1) as resp:
            if resp.status == 200 and json.loads(resp.read().decode("utf-8")).get("ok"):
                print("端口 %d 上已有网关在跑，本次不重复启动（先停掉旧的再启动）。"
                      % args.port, flush=True)
                sys.exit(2)
    except SystemExit:
        raise
    except Exception:
        pass  # 探不到 = 没人在跑，正常启动
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("简题 v0.2 网关已启动", flush=True)
    print("  固件 POST 地址：http://%s:%d/solve" % (lan_ip(), args.port), flush=True)
    print("  健康检查：http://%s:%d/health" % (lan_ip(), args.port), flush=True)
    print("  家长批准页：http://%s:%d/parent" % (lan_ip(), args.port), flush=True)
    print("  收图目录：%s" % UPLOAD_DIR, flush=True)
    if os.environ.get("JT_SOLVE_FAKE") == "1":
        print("  *** JT_SOLVE_FAKE=1 假解题模式：/solve 回罐头结果，不碰真实模型 ***", flush=True)
    _backend = os.environ.get("JT_VLM_BACKEND", "dashscope").strip().lower() or "dashscope"
    if _backend == "ollama":
        print("  推理后端：本地 Ollama（模型 %s，无需 key）"
              % (os.environ.get("JT_VLM_MODEL", DEFAULT_OLLAMA_MODEL) or DEFAULT_OLLAMA_MODEL), flush=True)
    elif not os.environ.get("JT_VLM_API_KEY"):
        print("  注意：未设置 JT_VLM_API_KEY，当前返回占位答案；"
              "设好 key 后重启即返回真实批改。", flush=True)
    else:
        print("  推理后端：DashScope（模型 %s）"
              % (os.environ.get("JT_VLM_MODEL", DEFAULT_MODEL) or DEFAULT_MODEL), flush=True)
    print("按 Ctrl+C 停止。", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("网关已停止。", flush=True)


if __name__ == "__main__":
    main()
