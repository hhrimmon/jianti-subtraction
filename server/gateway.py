#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""简题网关（PC 端，局域网联调版）

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
import hashlib
import json
import os
import socket
import time
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

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
# 无主力/备用标签：千问、豆包、元宝、DeepSeek、文心 五组 web AI 按组轮换。
# 遇滑块/拒答/限流即停该组冷却、切下一组；不准同一组连刷。
# Kimi、智谱已放弃，不再列入。
WEB_BRIDGES = [
    {"name": "千问",   "url": os.environ.get("JT_BRIDGE_QIANWEN",  "http://127.0.0.1:8001/solve")},
    {"name": "豆包",   "url": os.environ.get("JT_BRIDGE_DOUBAO",   "http://127.0.0.1:8002/solve")},
    {"name": "元宝",   "url": os.environ.get("JT_BRIDGE_YUANBAO",  "http://127.0.0.1:8003/solve")},
    {"name": "DeepSeek","url": os.environ.get("JT_BRIDGE_DEEPSEEK","http://127.0.0.1:8004/solve")},
    {"name": "文心",   "url": os.environ.get("JT_BRIDGE_WENXIN",   "http://127.0.0.1:8005/solve")},
]
# 每组软超时（拿不到就算该组不可用，切下一组）；全部用完再走本机降级。
QW_SOFT_DEADLINE_S = int(os.environ.get("JT_QW_SOFT_DEADLINE", "90"))
# web AI 组失败后冷却秒数（滑块/拒答/限流 30 分钟；连接不上 2 分钟，别一直打）
BRIDGE_COOLDOWN_S = {"captcha": 1800, "refusal": 1800, "ratelimit": 900, "conn": 120}
LOCAL_TIMEOUT_S = int(os.environ.get("JT_OLLAMA_TIMEOUT", "90"))
FALLBACK_BASE_URL = os.environ.get("JT_FALLBACK_BASE_URL", DEFAULT_OLLAMA_BASE_URL)
FALLBACK_MODEL = os.environ.get("JT_FALLBACK_MODEL", "qwen2.5vl:7b")
FALLBACK_TIMEOUT_S = int(os.environ.get("JT_FALLBACK_TIMEOUT", "80"))

# ---------- API 优先版：WorkBuddy 本地 API 直连（deepseek 第 1 优先、glm 第 2 后备） ----------
# 只在本测试副本里生效；正式版其它分支一行未动。
WB_API_BASE = os.environ.get("JT_WB_API_BASE", "http://127.0.0.1:7863/v1").strip()
WB_MODEL_POOL = [
    # 顺序即优先级：deepseek 先调；失败/超时/空回包才切 glm 后备。
    # 实际命中哪个模型一律写进 trace 与返回（model / priority_order / fallback_used），不许混写。
    # deepseek 默认 reasoning_effort=high，8192 预算会被思考吃光、正文为空（2026-10-29 实测）；
    # 固定走 low + 16384 预算才能出正文。
    {"model": "deepseek-v4.1-flash", "timeout": int(os.environ.get("JT_WB_T_DS", "95")),
     "reasoning_effort": "low", "max_tokens": 16384},
    {"model": "glm-5.3-flash",       "timeout": int(os.environ.get("JT_WB_T_GLM", "30"))},
]
WB_PRIORITY_ORDER = [s["model"] for s in WB_MODEL_POOL]
# ---- 路由管理（只读）：读管理页导出的 gateway_routes.json 决定池顺序 ----
# 文件缺失/损坏/无可用路由一律回落上面的内置池；绝不在此抛异常。
try:
    import admin_routes  # noqa: E402
except Exception as _ar_exc:  # 读取器缺失不影响网关启动
    admin_routes = None
    print("admin_routes 不可用，继续用内置模型池：%s" % _ar_exc, flush=True)
# 本批（001-crop）：图片已裁成只含第3题，提示词里写死「只做第3题」。
# 只影响提示词，不动分题/清洗/重组/卡账本逻辑；这段原文同时写进 trace 供审计。
PROMPT_SCOPE = ("本图只截取了第3题，只做第3题：只识别并解答第3题，不要识别或编造其他题目。"
                "先把第3题题干里给出的关系式逐条原样列出（照抄，不许改写、不许补等号），"
                "再按题目要求作答，最后写出所选选项字母和符合条件的关系式个数。")
WB_ROUTE = "local" if WB_API_BASE.startswith(("http://127.0.0.1", "http://localhost")) else "cloud"
# 每次调用（含回落）都往这里记一行：模型、成败、耗时、token、积分、失败原文
WB_TRACE_PATH = os.path.join(BASE_DIR, "wb_model_trace.jsonl")
# 积分从面板数据源 usage.jsonl 里按「模型 + 时间窗」取（代理不回传积分，只在这里有）
WB_USAGE_PATH = os.environ.get(
    "JT_WB_USAGE", os.path.join(BASE_DIR, "state", "wb_usage.jsonl"))
_wb_lock = threading.Lock()

# 轮转状态：round-robin 下标 + 各组冷却截止 ts（内存，重启清零；桥接内部账号组冷却由桥接自己落盘）
_bridge_lock = threading.Lock()
_bridge_idx = 0
_bridge_blocked = {}  # name -> cooldown_until_ts


# 网关版本号：/health 和 /solve 都会返回它，Trae 凭此确认跑的是哪一版，
# 避免旧进程未杀、新实例被单实例保护挡掉后仍跑旧版的混乱。
GATEWAY_VERSION = "v3.23-card-preborrow-floor-8000"
# API 优先版：基底 = gateway_cards.py，只换模型池顺序（deepseek→glm）
# + 加 NLP log-only 标记，分题/清洗/重组/卡账本逻辑一处未改。

# ---------- 分题升级（测试副本）----------
# 只改「切卡」与「记录」：
#   1) 整题里内联写 (1)(2)(3) 时按小题拆开，不许并成一卡；
#   2) 每个题/小题固定成对输出「思路」+「过程与答案」；
#   3) 思路段漏答案只记 leak_flag，不硬拦、不改输出校验正则、不改板端。
SPLIT_LOG = os.path.join(BASE_DIR, "leak_flags.jsonl")
CARD_LOG = os.path.join(BASE_DIR, "split_records.jsonl")
MISSING_SECTION_TEXT = "（模型原文未给出这一段）"

# ---------- 显示清洗（测试副本；补规则 5-8）----------
# 只在 server 端做一份：思路卡、过程卡都洗，/solve 与 /split 共用 clean_text_for_display。
# 顺序写死，不许调换：1) LaTeX → 2) 转义 → 3) 代码 → 4) 标题/列表/引用/分隔线/链接/图片
#   → 5) 粗体/斜体（乘号先行）→ 6) 空白归一。
# 7) 不动项：− ½ × ÷ √ ≤ ≥ ≈、中文标点、括号、数字/字母/选项字母、「答案：B」「故选C」原样保留；
# 8) 幂等：clean(clean(x)) == clean(x)，每条单测跑两遍。
# 只去显示标记，不删数字、不改答案；模型原文由 _log_raw_text 落 RAW_TEXT_LOG，卡里只发洗过的。
RAW_TEXT_LOG = os.path.join(BASE_DIR, "raw_texts.jsonl")

_LATEX_SYMBOLS = [
    ("\\times", "×"), ("\\div", "÷"), ("\\pm", "±"), ("\\pi", "π"),
    ("\\leq", "≤"), ("\\geq", "≥"), ("\\neq", "≠"), ("\\approx", "≈"),
]
_HR_RE = re.compile(r"^\s*([-*_])\1{2,}\s*$")
_MD_HEAD_RE = re.compile(r"^\s*#{1,6}\s*(.*?)\s*#*\s*$")
_MD_QUOTE_RE = re.compile(r"^\s*>\s?")
_MD_UL_RE = re.compile(r"^\s*[-*+]\s+")
_MD_IMG_RE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_MD_LINK_RE = re.compile(r"\[([^\]]+)\]\([^)]*\)")

# 规则 5：粗体/斜体（乘号先行：数字*数字先转 ×，绝不删成 23）
_DIGIT_MUL_RE = re.compile(r"(?<=\d)\s*\*\s*(?=\d)")
_BOLD3_RE = re.compile(r"\*\*\*(?=\S)(.+?)(?<=\S)\*\*\*")
_BOLD2_RE = re.compile(r"\*\*(?=\S)(.+?)(?<=\S)\*\*")
_BOLD_UNDER_RE = re.compile(r"__(?=\S)(.+?)(?<=\S)__")
# 单星斜体：两边不能是星号，内容不含星号/换行；数字间的星号已被乘号规则先吃掉
_ITALIC_STAR_RE = re.compile(r"(?<!\*)\*(?!\s)([^*\n]+?)(?<!\s)\*(?!\*)")
# 单下划线斜体：两侧需非字母数字下划线边界，所以 a_b 这类变量原样保留
_ITALIC_UNDER_RE = re.compile(r"(?<![0-9A-Za-z_])_(?!\s)([^_\n]+?)(?<!\s)_(?![0-9A-Za-z_])")


def _clean_latex(s):
    """规则 1：LaTeX 定界符与常见记号。"""
    s = s.replace("\\(", "").replace("\\)", "")
    s = s.replace("\\[", "").replace("\\]", "")
    s = s.replace("$$", "").replace("$", "")
    s = re.sub(r"\\frac\s*\{([^{}]*)\}\s*\{([^{}]*)\}", r"\1/\2", s)
    s = re.sub(r"\\sqrt\s*\{([^{}]*)\}", r"√\1", s)
    for k, v in _LATEX_SYMBOLS:
        s = s.replace(k, v)
    # 上标只做 2、3；复杂上标 ^{n+1} 留成 ^(n+1)
    s = s.replace("^{2}", "²").replace("^{3}", "³")
    s = s.replace("^2", "²").replace("^3", "³")
    s = re.sub(r"\^\{([^{}]+)\}", r"^(\1)", s)
    return s


def _clean_escape(s):
    """规则 2：\\* \\_ \\` \\\\ 去掉反斜杠，只留符号本身。"""
    return re.sub(r"\\([*_`\\])", r"\1", s)


def _clean_code(s):
    """规则 3：``` 块去三反引号只留内容（换行保留）；行内 `x+1` 去反引号。"""
    s = re.sub(r"```[ \t]*[A-Za-z0-9_+.-]*[ \t]*\n", "", s)  # 开围栏行（含语言标记）
    s = s.replace("```", "")                                  # 结尾围栏
    return s.replace("`", "")                                 # 行内反引号


def _clean_markdown(s):
    """规则 4：标题/列表/引用/分隔线/链接/图片。"""
    out = []
    for ln in s.split("\n"):
        if _HR_RE.match(ln):
            out.append("")  # 单独一行的 --- *** ___ 整行删，改空行
            continue
        t = _MD_IMG_RE.sub(r"\1", ln)   # ![图](url) -> 图
        t = _MD_LINK_RE.sub(r"\1", t)   # [文字](url) -> 文字
        m = _MD_HEAD_RE.match(t)
        if m:
            t = m.group(1)              # #标题 -> 标题
        else:
            t = _MD_QUOTE_RE.sub("", t)              # > 引用
            t = _MD_UL_RE.sub("", t, count=1)        # - * + 无序列表（有序 1. 保留数字与内容）
        out.append(t)
    return "\n".join(out)


def _clean_emphasis(s):
    """规则 5：粗体/斜体。乘号先行（2*3→2×3），再按 ***→**→__→*→_ 顺序去标记。"""
    s = _DIGIT_MUL_RE.sub("×", s)          # 数字*数字 -> ×（绝不删成 23）
    s = _BOLD3_RE.sub(r"\1", s)            # ***粗斜体*** -> 粗斜体
    s = _BOLD2_RE.sub(r"\1", s)            # **-3** -> -3
    s = _BOLD_UNDER_RE.sub(r"\1", s)       # __重点__ -> 重点
    s = _ITALIC_STAR_RE.sub(r"\1", s)      # *斜体* -> 斜体
    s = _ITALIC_UNDER_RE.sub(r"\1", s)     # _斜体_ -> 斜体（a_b 不匹配，原样保留）
    return s


def _clean_whitespace(s):
    """规则 6：多余空行压成一个，行首行尾空格去掉，段落/列表换行保留。"""
    out = []
    blank = False
    for ln in s.split("\n"):
        ln = ln.strip()
        if not ln:
            if blank:
                continue
            blank = True
        else:
            blank = False
        out.append(ln)
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return "\n".join(out)


def clean_text_for_display(text):
    """按写死的顺序清洗：1) LaTeX → 2) 转义 → 3) 代码 → 4) 标题/列表/引用/分隔线/链接/图片
    → 5) 粗体/斜体（乘号先行）→ 6) 空白归一。规则 7 不动项由上述规则本身保证；规则 8 幂等。"""
    if not isinstance(text, str) or not text or text == MISSING_SECTION_TEXT:
        return text
    s = _clean_latex(text)
    s = _clean_escape(s)
    s = _clean_code(s)
    s = _clean_markdown(s)
    s = _clean_emphasis(s)
    s = _clean_whitespace(s)
    return s


def _log_raw_text(src, text, saved=None):
    """原文留档：卡里只发洗过的，模型/上游原文只进日志。"""
    try:
        _write_jsonl(RAW_TEXT_LOG, {"ts": int(time.time()), "src": src,
                                    "saved": saved, "chars": len(text or ""),
                                    "text": text})
    except Exception as exc:
        print("raw text log failed: %s" % exc, flush=True)


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


# ---------- NLP log-only 标记（只标记，不改答案、不硬堵） ----------
# 用途：把「模型列的式子个数/原文、答案字母、答案个数」记成结构化 flags，供批量复用。
# 边界：只读原文、只写日志、只往返回里加 flags；不删字、不改字、不重试。
CASES_DIR = os.environ.get("JT_CASES_DIR", os.path.join(BASE_DIR, "cases"))
DEFAULT_CASE = os.environ.get("JT_DEFAULT_CASE", "").strip()
FLAGS_LOG = os.path.join(BASE_DIR, "api_prio_flags.jsonl")
# 关系式抽取：y=x+1 / xy=-1 / y=6x / y/x=1 / y=3/x / y=-x / y=2x / y=-1/x
_ITEM_RE = re.compile(
    r"([A-Za-z]{1,2}(?:/[A-Za-z])?\s*=\s*[-+]?[0-9A-Za-z]+(?:[+\-*/][0-9A-Za-z]+)*)")
_LETTER_RES = [
    re.compile(r"答案\s*[:：是为]?\s*[（(]?\s*([A-Da-d])\s*[）)]?"),
    re.compile(r"选\s*[（(]?\s*([A-Da-d])\s*[）)]?"),
    re.compile(r"应\s*选\s*([A-Da-d])"),
    re.compile(r"[（(]\s*([A-Da-d])\s*[）)]"),
]
_COUNT_RE = re.compile(r"(\d+)\s*个")


def _norm_item(s):
    """关系式归一：去空白、转小写，只用于比对，不改原文。"""
    return re.sub(r"\s+", "", (s or "")).lower()


def _load_case(case_id):
    """读用例规格（纸面原文 + 正确答案），读不到返回 None（只做结构标记，不猜）。"""
    if not case_id:
        return None
    path = os.path.join(CASES_DIR, "%s.json" % case_id)
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as exc:
        print("读用例规格失败（%s）：%s" % (path, exc), flush=True)
        return None


def _rewrite_variants(item):
    """纸面式子的常见「被改写」等价形式，只用来标 flag，不替换原文。"""
    it = _norm_item(item)
    out = []
    m = re.match(r"^xy=([-+]?[0-9a-z.]+)$", it)
    if m:
        k = m.group(1)
        out.append("y=%s/x" % k)
        if k in ("-1", "+-1"):
            out.append("y=-x")
        if k in ("1", "+1"):
            out.append("y=x")
    m = re.match(r"^y/x=([-+]?[0-9a-z.]+)$", it)
    if m:
        out.append("y=%sx" % m.group(1))
    m = re.match(r"^y=([-+]?[0-9a-z.]+)/x$", it)
    if m:
        out.append("xy=%s" % m.group(1))
    return out


def _api_prio_flags(text, case_id):
    """对模型原文做只读标记：抽式子、抽字母/个数、和纸面规格比对。

    返回 dict（写进 /solve 返回的 api_prio 字段），同时落一行 api_prio_flags.jsonl。
    """
    # 只对「分析副本」剥 LaTeX 壳（\(xy=-1\) / y=\frac{3}{x}），原文 text 一字不动。
    raw = text or ""
    try:
        raw = _clean_latex(raw)
    except Exception:
        raw = text or ""
    norm_text = _norm_item(raw)
    listed = []
    seen = set()
    for m in _ITEM_RE.finditer(raw):
        it = _norm_item(m.group(1))
        if it and it not in seen:
            seen.add(it)
            listed.append(it)
    letter = ""
    for rx in _LETTER_RES:
        m = rx.search(raw)
        if m:
            letter = m.group(1).upper()
            break
    counts = [int(x) for x in _COUNT_RE.findall(raw)]
    count = counts[-1] if counts else None
    case = _load_case(case_id) or {}
    paper_items = [str(x) for x in (case.get("paper_items") or [])]
    paper_norm = [_norm_item(x) for x in paper_items]
    present = [paper_items[i] for i, it in enumerate(paper_norm) if it in seen]
    missing = [paper_items[i] for i, it in enumerate(paper_norm) if it not in seen]
    flags = []
    if paper_items and len(present) != len(paper_items):
        flags.append("listed_count_mismatch:%d/%d" % (len(present), len(paper_items)))
    for it in paper_items:
        if _norm_item(it) not in seen:
            flags.append("paper_item_missing:%s" % it)
    for it in missing:
        for var in _rewrite_variants(it):
            if var in seen:
                flags.append("rewrite_detected:%s->%s" % (it, var))
    correct_letter = str(case.get("correct_letter") or "").upper()
    correct_count = case.get("correct_count")
    if correct_letter and letter and letter != correct_letter:
        flags.append("option_letter_mismatch:%s!=%s" % (letter, correct_letter))
    if correct_count is not None and count is not None and int(count) != int(correct_count):
        flags.append("option_count_mismatch:%s!=%s" % (count, correct_count))
    opt_map = {str(k).upper(): v for k, v in (case.get("option_count_map") or {}).items()}
    if letter and count is not None and letter in opt_map:
        if int(opt_map[letter]) != int(count):
            flags.append("letter_count_self_contradiction:%s=%s个却写%s个"
                         % (letter, opt_map[letter], count))
    letter_ok = (not correct_letter) or (letter == correct_letter)
    count_ok = (correct_count is None) or (count is not None and int(count) == int(correct_count))
    info = {
        "case_id": case_id or "",
        "paper_id": case.get("paper_id") or "",
        "question_no": case.get("question_no"),
        "paper_items": paper_items,
        "paper_items_present": present,
        "paper_items_missing": missing,
        "listed_items": listed,
        "listed_count": len(listed),
        "answer_letter": letter,
        "answer_count": count,
        "counts_found": counts,
        "correct_letter": correct_letter,
        "correct_count": correct_count,
        "letter_ok": letter_ok,
        "count_ok": count_ok,
        "consistent": bool(letter_ok and count_ok),
        "flags": flags,
    }
    try:
        _write_jsonl(FLAGS_LOG, {"ts": int(time.time()),
                                 "case_id": info["case_id"],
                                 "flags": flags,
                                 "listed_items": listed,
                                 "answer_letter": letter,
                                 "answer_count": count})
    except Exception as exc:
        print("写 flags 日志失败：%s" % exc, flush=True)
    return info


def grade_prompt():
    mid = [random.choice(_G_STRUCT), random.choice(_G_BAN), random.choice(_G_QID),
           random.choice(_G_SUBQ), random.choice(_G_NOANS), random.choice(_G_FILL),
           random.choice(_G_NOCROSS), random.choice(_G_ORDER)]
    random.shuffle(mid)
    # 尾禁令固定在末尾，不参与打乱（开头禁令管不住结尾汇总）；范围行紧跟开头。
    return random.choice(_G_OPEN) + PROMPT_SCOPE + "".join(mid) + random.choice(_G_TAIL)


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
    ("等于数值", re.compile(r"(等\s*于|结\s*果\s*是|得\s*数\s*是|值\s*为|化\s*简\s*为)\s*-?\d")),
    ("概念是数值", re.compile(r"(相反数|倒数|绝对值|平方|立方)\s*是\s*-?\d")),
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


# ---------- 服务端规范化：AI 原文 -> 逐题卡（客户端不用改） ----------
CARD_HEAD = re.compile(
    r"^第\s*([0-9一二三四五六七八九十]+)\s*题\s*(?:[（(]\s*([0-9]{1,2})\s*[)）])?\s*[·．.]?\s*(思路|过程与答案)?")
# 裸括号小题头：(1) / （1），继承最近大题号
SUB_HEAD = re.compile(r"^[（(]\s*([0-9]{1,2})\s*[)）]")

# 思路段最终答案特征（与 validate_free_sections 一致，命中即清掉思路正文）
_IDEA_ANS_PATTERNS = [
    re.compile(r"答\s*案\s*[是为：:]?\s*\S"),
    re.compile(r"(结\s*论|最终结果|最终答案)\s*[是为：:]?\s*\S"),
    re.compile(r"选\s*[A-Ea-e](?![a-zA-Z])"),
    re.compile(r"(应\s*填|填\s*入|填\s*[:：])\s*[「\"']?\S"),
    re.compile(r"[a-zA-Z]\s*[=＝]\s*-?\d+(\.\d+)?"),
    re.compile(r"[=＝]\s*-?\d+(\.\d+)?(\s*[a-zA-Z一-鿿]{0,4})?\s*[。\.；;，,]?\s*$"),
    re.compile(r"(等\s*于|结\s*果\s*是|得\s*数\s*是|值\s*为|化\s*简\s*为)\s*-?\d"),
    re.compile(r"(相反数|倒数|绝对值|平方|立方)\s*是\s*-?\d"),
]


def _idea_has_answer(text):
    """思路段是否含最终答案特征。"""
    for ln in text.split("\n"):
        s = ln.strip()
        if not s:
            continue
        for pat in _IDEA_ANS_PATTERNS:
            if pat.search(s):
                return True
    return False


def _write_jsonl(path, rec):
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception as exc:
        print("jsonl write failed (%s): %s" % (path, exc), flush=True)


# 叙述式答案的特征（只用于打 leak_flag，不进输出校验、不改正则）
_NARRATIVE_LEAK_PATTERNS = [
    ("叙述应为", re.compile(r"(应为|即为|就是|就等于)\s*[−\-]?\s*\d")),
    ("叙述为值", re.compile(r"(相反数|倒数|绝对值|平方|立方|结果|得数|值)\s*[是为]\s*[−\-]?\s*\d")),
    ("故得值", re.compile(r"(故|因此|所以)[^。；\n]{0,12}?(故选|[−\-]?\d+(\.\d+)?)\s*[。；;]?\s*$")),
]


def _leak_scan(body):
    """逐行扫思路段：先复用输出校验的 _ANS_PATTERNS（原样不动），再补叙述式特征。

    返回 [(特征名, 命中行)]；只用于记录，不改任何正文。
    """
    hits = []
    for ln in (body or "").split("\n"):
        s = ln.strip()
        if not s:
            continue
        hit = None
        for name, pat in _ANS_PATTERNS:
            if pat.search(s):
                hit = (name, s)
                break
        if hit is None:
            for name, pat in _NARRATIVE_LEAK_PATTERNS:
                if pat.search(s):
                    hit = (name, s)
                    break
        if hit:
            hits.append(hit)
    return hits


def _log_leak(qid, body, hits, stage="card"):
    rec = {"ts": int(time.time()), "qid": qid, "stage": stage, "leak_flag": True,
           "patterns": [h[0] for h in hits], "lines": [h[1][:120] for h in hits],
           "excerpt": (body or "").strip()[:300]}
    _write_jsonl(SPLIT_LOG, rec)
    print("leak_flag %s 思路段命中 %s" % (qid, "/".join(rec["patterns"])), flush=True)
    return rec


def _scan_text_leaks(text, saved_path=None):
    """逐段扫「思路」段，只记录不改文；返回回包用的 leak_flags 列表。"""
    flags = []
    if not isinstance(text, str) or not text.strip():
        return flags
    lines = text.split("\n")
    i, n = 0, len(lines)
    while i < n:
        m = _HEAD_RE.match(lines[i].strip())
        if m and m.group(3) == "思路":
            j = i + 1
            body = []
            while j < n and not _HEAD_RE.match(lines[j].strip()):
                body.append(lines[j])
                j += 1
            qid = "第%s题%s" % (m.group(1), "(%s)" % m.group(2) if m.group(2) else "")
            hits = _leak_scan("\n".join(body))
            if hits:
                rec = _log_leak(qid, "\n".join(body), hits, stage="solve")
                rec["saved"] = saved_path
                flags.append({"qid": qid, "leak_flag": True, "patterns": rec["patterns"],
                              "line": rec["lines"][0][:120]})
            i = j
        else:
            i += 1
    return flags


def _sanitize_idea_body(body, qid):
    """不硬拦。命中答案特征只记 leak_flag，思路正文原样保留。

    本版不硬拦：命中只记录，不把正文换成孩子提示。
    输出校验正则 _ANS_PATTERNS 一行未改。
    """
    hits = _leak_scan(body)
    if hits:
        _log_leak(qid, body, hits, stage="card")
    return body


# ---------- 卡内数学算式拆行合并（recombine_math_lines） ----------
# 位置：server 端、分卡之后、每张卡内部；思路卡与过程卡同一套，/solve 与 /split 共用。
# 条件（必须同时满足，缺一条就不拼）：
#   1) 只在同一张卡内拼，不跨卡、不跨题、不跨思路/过程；
#   2) 只拼单换行 \n；空行是分段，绝不拼；
#   3) 每一行必须是单个数学 token 且单行短（≤12 字符），无中文、无「步骤/答案」这类词；
#   4) 拼出的整串必须是合法算式（运算符交替、以数字结尾、中间最多一个 =），且最多拼 7 行。
# 原文由 _log_raw_text 落 RAW_TEXT_LOG，卡里只发重组后的。
_MATH_TOKEN_RE = re.compile(
    r"^(?:[+\-−]?\d+(?:\.\d+)?|[+\-−]?\.\d+|\d+(?:\.\d+)?/\d+(?:\.\d+)?"
    r"|[A-Za-z]|[+\-−×÷*/=().])$")
_CJK_RE = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")
_MATH_ATOM = r"(?:\d+(?:\.\d+)?|[A-Za-z]|\([^()]*\))"
_MATH_TERM = _MATH_ATOM + r"(?:\s*[×÷*/]\s*" + _MATH_ATOM + r")*"
_MATH_EXPR = _MATH_TERM + r"(?:\s*[+−\-]\s*" + _MATH_TERM + r")*"
_MATH_FULL_RE = re.compile(
    r"^[+−\-]?" + _MATH_EXPR + r"(?:\s*=\s*[+−\-]?" + _MATH_EXPR + r")?$")
_MATH_SIGNS = ("+", "-", "−")
_MATH_OPS = ("+", "-", "−", "×", "÷", "*", "/", "=", "(")


def _is_math_token_line(ln):
    """整行就是一个数学 token（数字/变量/运算符/括号/分数/小数点），且短、无中文。"""
    s = (ln or "").strip()
    if not s or len(s) > 12 or _CJK_RE.search(s):
        return False
    return bool(_MATH_TOKEN_RE.match(s))


def _join_math_tokens(toks):
    """拼法：运算符加单空格；/ 与 . 前后不加空格；负号与数字不加空格。"""
    out = ""
    for i, tk in enumerate(toks):
        if i == 0:
            out = tk
            continue
        prev = toks[i - 1]
        prev_is_unary_sign = (prev in _MATH_SIGNS) and (i - 1 == 0 or toks[i - 2] in _MATH_OPS)
        glue = (tk in (".", "/", ")", ",") or prev in ("/", ".", "(") or prev_is_unary_sign)
        out = out + ("" if glue else " ") + tk
    return out


def recombine_math_lines(text):
    """卡内重组：连续的数学 token 行拼成一条算式；不满足条件的原样保留。"""
    if not isinstance(text, str) or not text or text == MISSING_SECTION_TEXT:
        return text
    lines = text.split("\n")
    out = []
    i, n = 0, len(lines)
    while i < n:
        if not _is_math_token_line(lines[i]):
            out.append(lines[i])
            i += 1
            continue
        j = i
        while j < n and _is_math_token_line(lines[j]):
            j += 1
        run = [ln.strip() for ln in lines[i:j]]
        merged = None
        if 2 <= len(run) <= 7:
            cand = _join_math_tokens(run)
            if _MATH_FULL_RE.match(cand) and cand[-1:].isdigit():
                merged = cand
        if merged is not None:
            out.append(merged)
        else:
            out.extend(lines[i:j])
        i = j
    return "\n".join(out)


# 附带小项：标题同行拆分——一行开头就是【标记】、后面紧跟正文（无换行）时拆成两行，
# 正文归入该卡，后续行继续该卡；其他分题逻辑与输出校验正则一行不动。
_INLINE_HEAD_RE = re.compile(r"^\s*【([^】]{1,30})】\s*(\S.*)$")


def _split_inline_heads(text):
    if not isinstance(text, str) or "【" not in text:
        return text
    out = []
    for ln in text.split("\n"):
        m = _INLINE_HEAD_RE.match(ln)
        if m:
            inner = m.group(1).strip()
            rest = m.group(2).strip()
            if (CARD_HEAD.match(inner) or "标准答案" in inner
                    or inner.startswith("解题思路") or inner.startswith("核心解题步骤")):
                out.append("【%s】" % inner)
                if rest:
                    out.append(rest)
                continue
        out.append(ln)
    return "\n".join(out)


def build_cards(text):
    """把「解题思路 / 核心解题步骤 / 第11题 / 标准答案 / 第11题」这种题组式原文切成逐题卡。

    标题统一成「第N题·思路」「第N题·过程与答案」，括号小题记成 第3题(1)·
    切不出来就返回空列表，不拿整卷压成一张卡。

    题组式兜底：AI 不按【第N题·思路】/【第N题·过程与答案】输出、而是写成
    「核心解题步骤 + 第N题（考点/分析/结论）+ 标准答案」时：
      - 裸 (1)(2)(3) 按独立小题切分，继承大题号；
      - 每题首行（考点/入手点）归思路，其余归过程与答案；
      - 思路段命中答案特征则只留考点行、清掉含答案的部分；
      - 标准答案按顺序挂到各题过程末尾。
    """
    # 标题与正文写在同一行时先拆成两行，正文不丢（只此一项，不改其他分题逻辑）
    text = _split_inline_heads(text)
    by, order = {}, []
    state = {"key": None, "mode": "idea", "buf": [], "last_main": None, "explicit": None}
    std_answers = []
    in_std = False
    has_group = (("核心解题步骤" in text) or ("解题思路" in text)) and ("标准答案" in text)

    def add(key, kind, body):
        body = (body or "").strip()
        if not body:
            return
        if key not in by:
            by[key] = {"idea": "", "process": ""}
            order.append(key)
        if by[key][kind]:
            by[key][kind] += "\n" + body
        else:
            by[key][kind] = body

    def flush():
        key = state["key"]
        buf = state["buf"]
        state["buf"] = []
        if key is None:
            return
        body = "\n".join(buf).strip()
        if not body:
            return
        if state["explicit"] is not None:
            # 标记模式：【第N题·思路】/【第N题·过程与答案】显式指定段类型，直接归入不拆分
            kind = state["explicit"]
            if kind == "idea":
                body = _sanitize_idea_body(body, key)
            add(key, kind, body)
        elif state["mode"] == "proc":
            # 题组式过程段：首行（考点/入手点）归思路，其余归过程与答案
            lines = [l for l in body.split("\n")]
            idea_line = lines[0].strip() if lines else ""
            proc_body = "\n".join(lines[1:]).strip() if len(lines) > 1 else ""
            # 思路段禁最终答案：命中即换提示，不准留答案、不准静默丢
            if idea_line:
                idea_line = _sanitize_idea_body(idea_line, key)
            add(key, "idea", idea_line)
            add(key, "process", proc_body)
        else:
            idea = _sanitize_idea_body(body, key)
            add(key, "idea", idea)

    for ln in text.split("\n"):
        t = ln.strip()
        if not t:
            if state["key"] is not None:
                state["buf"].append("")
            continue
        plain = t.replace("【", "").replace("】", "")
        # 先匹配题号头（含【第N题·过程与答案】），避免被栏目词误判
        m = CARD_HEAD.match(plain)
        if m:
            flush()
            num = m.group(1)
            state["last_main"] = num
            state["key"] = num + ("" if m.group(2) is None else "(%s)" % m.group(2))
            seg = m.group(3) or ""
            if "过程" in seg:
                state["mode"] = "proc"
                state["explicit"] = "process"
            elif "思路" in seg:
                state["mode"] = "idea"
                state["explicit"] = "idea"
            else:
                state["explicit"] = None  # 裸题号，题组式继承 mode
            in_std = False
            continue
        # 裸括号小题 (1)：继承最近大题号，独立成卡
        sm = SUB_HEAD.match(plain)
        if sm and state["last_main"] is not None:
            flush()
            state["key"] = "%s(%s)" % (state["last_main"], sm.group(1))
            state["explicit"] = None  # 小题头，题组式继承 mode
            in_std = False
            rest = plain[sm.end():].strip(" \t:：、.")
            if rest:  # (1) 后面同一行的正文不能丢
                state["buf"].append(rest)
            continue
        if "标准答案" in plain or "过程与答案" in plain:
            flush()
            state["key"] = None
            state["mode"] = "proc"
            in_std = True
            continue
        if plain.startswith("解题思路"):
            flush()
            state["key"] = None
            state["mode"] = "idea"
            in_std = False
            continue
        if plain.startswith("核心解题步骤"):
            flush()
            state["key"] = None
            state["mode"] = "proc"
            in_std = False
            continue
        if in_std and state["key"] is None:
            std_answers.append(t)
            continue
        if state["key"] is not None:
            state["buf"].append(ln)
    flush()

    # 题组式：把「标准答案」段的答案按顺序挂到各题过程末尾
    if has_group and std_answers:
        for i, key in enumerate(order):
            if i < len(std_answers) and key in by:
                ans = std_answers[i].strip()
                if ans:
                    if by[key]["process"]:
                        by[key]["process"] += "\n标准答案：" + ans
                    else:
                        by[key]["process"] = "标准答案：" + ans

    # ---------- 分题升级 ----------
    # 1) 整题正文里内联写 (1)(2)(3) 的，按小题拆成独立卡（继承大题号）；
    # 2) 每个题/小题固定成对输出「思路」+「过程与答案」，缺一段用占位说明补齐；
    # 3) 按 题号 → 小题号 自然顺序输出，思路在前、过程在后。
    by, order = _explode_inline_subs(by, order)
    out = []
    for key in sorted(order, key=_key_order):
        c = by[key]
        head = _card_head(key)
        # 出卡前统一清洗（思路卡、过程卡同一套），空段仍用占位说明不洗
        # 清洗后、出卡前做卡内数学算式拆行合并（同一套）
        idea = recombine_math_lines(clean_text_for_display(c["idea"])) if c["idea"] else MISSING_SECTION_TEXT
        proc = recombine_math_lines(clean_text_for_display(c["process"])) if c["process"] else MISSING_SECTION_TEXT
        out.append({"title": head + "·思路", "text": idea})
        out.append({"title": head + "·过程与答案", "text": proc})
    _write_jsonl(CARD_LOG, {"ts": int(time.time()), "count": len(out),
                            "titles": [c["title"] for c in out]})
    return out


_SUB_LINE = re.compile(r"^[（(]\s*([0-9]{1,2})\s*[)）]\s*(.*)$")


def _card_head(key):
    if "(" in key:
        num, sub = key.split("(", 1)
        return "第%s题(%s" % (num, sub)  # sub 自带右括号
    return "第%s题" % key


def _key_order(key):
    m = re.match(r"^(\d+)(?:\((\d+)\))?$", key or "")
    if not m:
        return (9999, 9999)
    return (int(m.group(1)), int(m.group(2) or 0))


def _split_sub_lines(body):
    """把正文按行首 (1)/(2) 切成 [(小题号, 正文)]；不足两段返回 None（不拆）。"""
    parts, cur = [], None
    for ln in body.split("\n"):
        m = _SUB_LINE.match(ln.strip())
        if m:
            if cur:
                parts.append(cur)
            cur = [m.group(1), [m.group(2).strip()] if m.group(2).strip() else []]
        elif cur is not None:
            cur[1].append(ln)
    if cur:
        parts.append(cur)
    texts = [(p[0], "\n".join(p[1]).strip()) for p in parts if "\n".join(p[1]).strip()]
    return texts if len(texts) >= 2 else None


def _explode_inline_subs(by, order):
    """整题正文内联 (1)(2)… 时按小题拆卡，避免整题并成一卡。"""
    new_by = {k: {"idea": v["idea"], "process": v["process"]} for k, v in by.items()}
    new_order = list(order)
    for key in order:
        if "(" in key:
            continue
        for kind in ("idea", "process"):
            body = by[key][kind]
            if not body:
                continue
            parts = _split_sub_lines(body)
            if not parts:
                continue
            new_by[key][kind] = ""
            for sub, txt in parts:
                sk = "%s(%s)" % (key, sub)
                if sk not in new_by:
                    new_by[sk] = {"idea": "", "process": ""}
                    if sk not in new_order:
                        new_order.append(sk)
                old = new_by[sk][kind]
                new_by[sk][kind] = (old + "\n" + txt) if old else txt
    new_order = [k for k in new_order if new_by[k]["idea"] or new_by[k]["process"]]
    return new_by, new_order


def _openai_vision(jpeg_bytes, base_url, model, api_key, timeout, tag):
    """OpenAI 兼容的视觉接口（Ollama / DashScope / DeepSeek 通用），返回模型原文。"""
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
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = "Bearer " + api_key
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
        raise RuntimeError("模型调用失败（%s）：%s" % (tag, exc))
    try:
        content = body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise RuntimeError("模型返回结构异常：%s" % json.dumps(body, ensure_ascii=False)[:300])
    if isinstance(content, list):  # 有些返回把 content 包成数组
        content = "".join(
            part.get("text", "") for part in content if isinstance(part, dict))
    return content


def _wb_credit(model, t0):
    """代理不回传积分，从面板数据源 usage.jsonl 里取本次请求那一行（模型 + 时间窗）。"""
    try:
        if not os.path.exists(WB_USAGE_PATH):
            return {}
        with open(WB_USAGE_PATH, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 65536))
            lines = f.read().decode("utf-8", "ignore").splitlines()
        hit = None
        for ln in lines[-120:]:
            ln = ln.strip()
            if not ln:
                continue
            try:
                rec = json.loads(ln)
            except ValueError:
                continue
            if rec.get("model") != model:
                continue
            if float(rec.get("at") or 0) + 2 < t0:
                continue
            hit = rec
        if not hit:
            return {}
        return {"credit": hit.get("credit"), "total_tokens": hit.get("total_tokens"),
                "prompt_tokens": hit.get("prompt_tokens"),
                "completion_tokens": hit.get("completion_tokens"),
                "reasoning_tokens": hit.get("reasoning_tokens"),
                "ttft_ms": hit.get("ttft_ms"), "gen_ms": hit.get("gen_ms")}
    except Exception as exc:
        print("读 usage.jsonl 失败（不影响主链路）：%s" % exc, flush=True)
        return {}


def _wb_trace(rec):
    """每次模型调用都落一行（含回落），供报回逐次核对。"""
    try:
        with _wb_lock:
            with open(WB_TRACE_PATH, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception as exc:
        print("写 trace 失败：%s" % exc, flush=True)


def _call_wb_api(jpeg_bytes, model, timeout, reasoning_effort=None, max_tokens=8192):
    """调本机 WorkBuddy API（OpenAI 兼容），成功返回模型原文；失败抛 RuntimeError（含原文）。"""
    url = WB_API_BASE.rstrip("/") + "/chat/completions"
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
        "max_tokens": max_tokens,
    }
    if reasoning_effort:
        payload["reasoning_effort"] = reasoning_effort
    # 代理 api_key_required=false，本地回环不带 Authorization（key 不落盘、不进日志）
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    t0 = time.time()
    http_status, err, content, usage = 0, "", "", {}
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            http_status = resp.status
            raw = resp.read().decode("utf-8", "ignore")
        body = json.loads(raw)
        usage = body.get("usage") or {}
        content = (body["choices"][0]["message"].get("content") or "")
        if isinstance(content, list):
            content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
        content = (content or "").strip()
        if not content:
            err = "空回包（choices[0].message.content 为空）"
    except urllib.error.HTTPError as exc:
        http_status = exc.code
        detail = exc.read().decode("utf-8", "ignore")[:300]
        err = "HTTP %s：%s" % (exc.code, detail)
    except Exception as exc:
        err = "调用异常（%s）：%s" % (model, exc)
    elapsed_ms = int((time.time() - t0) * 1000)
    cr = _wb_credit(model, t0)
    first = content.split("\n")[0].strip()[:80] if content else ""
    _wb_trace({
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "model": model,
        "route": WB_ROUTE, "api_base": WB_API_BASE, "prompt_scope": PROMPT_SCOPE,
        "ok": (not err), "http": http_status, "elapsed_ms": elapsed_ms,
        "timeout_s": timeout,
        "prompt_tokens": cr.get("prompt_tokens", usage.get("prompt_tokens")),
        "completion_tokens": cr.get("completion_tokens", usage.get("completion_tokens")),
        "reasoning_tokens": cr.get("reasoning_tokens"),
        "total_tokens": cr.get("total_tokens", usage.get("total_tokens")),
        "credit": cr.get("credit"), "ttft_ms": cr.get("ttft_ms"),
        "first_line": first, "error": err[:400],
    })
    if err:
        raise RuntimeError("%s：%s" % (model, err))
    return content, {"model": model, "elapsed_ms": elapsed_ms,
                     "wb_route": WB_ROUTE, "wb_api_base": WB_API_BASE,
                     "credit": cr.get("credit"), "total_tokens": cr.get("total_tokens"),
                     "prompt_tokens": cr.get("prompt_tokens"),
                     "completion_tokens": cr.get("completion_tokens"),
                     "reasoning_tokens": cr.get("reasoning_tokens"),
                     "ttft_ms": cr.get("ttft_ms"), "attempts": []}


def _call_web_bridge(name, url, jpeg_bytes):
    """调一组 web AI 桥接，失败抛一句人话（含失败类型供冷却判定）。"""
    req = urllib.request.Request(url, data=jpeg_bytes,
                                 headers={"Content-Type": "image/jpeg"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=min(QW_TIMEOUT_S, QW_SOFT_DEADLINE_S)) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = (json.loads(exc.read().decode("utf-8")) or {}).get("error") or ""
        except Exception:
            pass
        if detail:
            raise RuntimeError("%s" % detail)
        raise RuntimeError("%s桥接 HTTP %d：%s" % (name, exc.code, exc))
    except Exception as exc:
        # 连接不上 = 该组桥接未启动/掉线，不算风控，冷却短一点
        raise RuntimeError("%s桥接不可用（conn）：%s" % (name, exc))
    if not body.get("ok"):
        raise RuntimeError("%s" % body.get("error") or "%s桥接返回失败" % name)
    return body.get("text", ""), {"upload_ms": body.get("upload_ms"),
                                 "answer_ms": body.get("answer_ms"),
                                 "shots": body.get("shots") or []}


def _classify_failure(err):
    """从错误文本判定失败类型，决定冷却时长。"""
    s = str(err)
    if any(k in s for k in ("滑块", "captcha", "风控")):
        return "captcha"
    if any(k in s for k in ("拒答", "不回答", "无法回答", "不适合")):
        return "refusal"
    if any(k in s for k in ("限流", "频率", "rate", "429", "繁忙")):
        return "ratelimit"
    return "conn"


def _pick_next_bridge():
    """round-robin 取下一个未冷却的 web AI 组下标；全冷却返回 None。

    从当前下标开始试（含当前），确保首个可用组不被跳过。
    """
    global _bridge_idx
    now = time.time()
    with _bridge_lock:
        for step in range(len(WEB_BRIDGES)):
            nxt = (_bridge_idx + step) % len(WEB_BRIDGES)
            name = WEB_BRIDGES[nxt]["name"]
            if now < _bridge_blocked.get(name, 0):
                continue
            _bridge_idx = (nxt + 1) % len(WEB_BRIDGES)  # 下次从下一组开始，不准同组连刷
            return nxt
        return None


def call_model(jpeg_bytes):
    """调模型，返回 (原始文本, key_err, 计时)。

    无主力/备用：千问/豆包/元宝/DeepSeek/文心 五组 web AI 按组轮换，
    遇滑块/拒答/限流即停该组冷却切下一组；不准同组连刷。
    全部 web 组失败后才走本机 Ollama 降级（标明降级、不冒充 web 截图/两段）。
    """
    backend = os.environ.get("JT_VLM_BACKEND", "dashscope").strip().lower() or "dashscope"
    timing = None
    if backend == "qianwen":
        # 走 web AI 分组轮换（不再以千问为唯一主力）
        errors = []
        for _ in range(len(WEB_BRIDGES)):
            idx = _pick_next_bridge()
            if idx is None:
                break
            grp = WEB_BRIDGES[idx]
            try:
                text, timing = _call_web_bridge(grp["name"], grp["url"], jpeg_bytes)
                return text, None, timing
            except Exception as exc:
                err = str(exc)
                errors.append("%s: %s" % (grp["name"], err[:80]))
                ftype = _classify_failure(exc)
                cd = BRIDGE_COOLDOWN_S.get(ftype, 120)
                with _bridge_lock:
                    _bridge_blocked[grp["name"]] = time.time() + cd
                print("%s 不可用（%s，冷却 %d 分钟）→ 切下一组"
                      % (grp["name"], ftype, cd // 60), flush=True)
        # 五组 web AI 全失败 → 本机降级（只作文本降级，不冒充 web 截图与两段）
        print("web AI 五组全失败（%s）→ 转本机降级 %s（仅文本，标明降级）"
              % ("；".join(errors), FALLBACK_MODEL), flush=True)
        try:
            content = _openai_vision(jpeg_bytes, FALLBACK_BASE_URL, FALLBACK_MODEL, "",
                                     FALLBACK_TIMEOUT_S, "本机降级")
        except Exception as exc2:
            raise RuntimeError("web AI 全失败；本机降级也失败：%s" % exc2)
        print("本机降级成功（%s）" % FALLBACK_MODEL, flush=True)
        # 降级结果：加前缀标明是降级，无 shots、不冒充两段结构
        degraded = "【本机降级答案·非 web AI】\n" + content
        return degraded, None, {"fallback": FALLBACK_MODEL, "degraded": True,
                                "web_errors": errors}
    if backend == "ollama":
        base_url = os.environ.get("JT_VLM_BASE_URL", DEFAULT_OLLAMA_BASE_URL).strip() or DEFAULT_OLLAMA_BASE_URL
        model = os.environ.get("JT_VLM_MODEL", DEFAULT_OLLAMA_MODEL).strip() or DEFAULT_OLLAMA_MODEL
        try:
            return _openai_vision(jpeg_bytes, base_url, model, "", LOCAL_TIMEOUT_S, "本机 ollama"), None, timing
        except Exception as exc:
            local_err = str(exc)
        print("本机模型不可用（%s）→ 转 web AI 分组轮换兜底" % local_err, flush=True)
        errors = []
        for _ in range(len(WEB_BRIDGES)):
            idx = _pick_next_bridge()
            if idx is None:
                break
            grp = WEB_BRIDGES[idx]
            try:
                text, timing = _call_web_bridge(grp["name"], grp["url"], jpeg_bytes)
                return text, None, dict(timing or {}, fallback="web", local_error=local_err)
            except Exception as exc2:
                errors.append("%s: %s" % (grp["name"], str(exc2)[:80]))
                ftype = _classify_failure(exc2)
                cd = BRIDGE_COOLDOWN_S.get(ftype, 120)
                with _bridge_lock:
                    _bridge_blocked[grp["name"]] = time.time() + cd
        raise RuntimeError("本机模型不可用：%s；web AI 兜底也失败：%s" % (local_err, "；".join(errors)))
    if backend == "wbapi":
        # WorkBuddy API 双模型池，deepseek 第 1 优先、glm 第 2 后备
        attempts = []
        _pool = admin_routes.current_pool(WB_MODEL_POOL) if admin_routes else WB_MODEL_POOL
        for idx, slot in enumerate(_pool):
            model, tmo = slot["model"], slot["timeout"]
            try:
                text, meta = _call_wb_api(jpeg_bytes, model, tmo,
                                          slot.get("reasoning_effort"),
                                          slot.get("max_tokens", 8192))
                meta["attempts"] = attempts
                meta["priority_order"] = [s["model"] for s in _pool]
                meta["priority_rank"] = idx + 1
                meta["fallback_used"] = bool(attempts)
                meta["attempted_models"] = [a["model"] for a in attempts]
                print("wbapi 实际模型=%s（线路=%s %s，第 %d 优先，%d ms，积分 %s，回落=%s）"
                      % (model, WB_ROUTE, WB_API_BASE, idx + 1, meta.get("elapsed_ms") or 0,
                         meta.get("credit"), "Y" if attempts else "N"), flush=True)
                return text, None, meta
            except Exception as exc:
                attempts.append({"model": model, "error": str(exc)[:300]})
                print("wbapi %s 失败（%s）→ 回落下一个模型" % (model, str(exc)[:120]), flush=True)
        raise RuntimeError("双模型池全部失败（不静默走本地模型）：%s"
                           % "；".join("%s: %s" % (a["model"], a["error"]) for a in attempts))
    if backend == "dashscope":
        api_key = os.environ.get("JT_VLM_API_KEY", "").strip()
        if not api_key:
            return None, "JT_VLM_API_KEY 未设置", None
        base_url = os.environ.get("JT_VLM_BASE_URL", DEFAULT_BASE_URL).strip() or DEFAULT_BASE_URL
        model = os.environ.get("JT_VLM_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL
        return _openai_vision(jpeg_bytes, base_url, model, api_key, MODEL_TIMEOUT_S, "dashscope 后端"), None, timing
    raise RuntimeError("JT_VLM_BACKEND 不支持：%s（只能填 dashscope、ollama 或 qianwen）" % backend)


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


# ---------- 解题卡账本 ----------
# 边界：只加卡账本与 /card/* 路由；分题、清洗、重组、/solve 出卡逻辑一律不动。
# 单家庭账号测试版：按 device_id 记账，默认每日 5 张，跨日按配额重置。
CARD_LEDGER_PATH = os.path.join(BASE_DIR, "cards_ledger.jsonl")
CARD_STATE_PATH = os.path.join(BASE_DIR, "cards_state.json")
CARD_LOCK = threading.Lock()
CARD_DAILY_QUOTA = 5
CARD_APPROVE_AMOUNT = 5
CARD_RECORDS_MAX = 30

# 20261030：解锁键 = 设备 + 拍题批次 + 小题索引。
# 批次由 App 在拍照时生成一次、全程携带（/solve 走 header X-JT-Batch，/card/* 走 body.batch_id）；
# App 未携带时才退回题图 sha1 前 12 位（同一张图=同一批，兼容固件/旧客户端）。
# _LAST_CASE_BY_DEV 只是「按设备记住最近一次 /solve 的批次」的**临时测试兜底**，
# 仅供尚未带 batch_id 的旧客户端联调用，正式口径以 App 显式携带的批次为准。
_LAST_CASE_BY_DEV = {}


def _batch_key(device_id, batch, qkey):
    b = (str(batch).strip() if batch else "") or _LAST_CASE_BY_DEV.get(device_id, "")
    return ("%s::%s" % (b, qkey)) if b else str(qkey)


def _today_str():
    return time.strftime("%Y-%m-%d")


def _load_card_state():
    """读账本状态；不存在/损坏都当空表，不炸请求。"""
    try:
        with open(CARD_STATE_PATH, "r", encoding="utf-8") as f:
            st = json.load(f)
        if isinstance(st, dict) and isinstance(st.get("devices"), dict):
            st.setdefault("applications", [])
            st.setdefault("device_id", "")
            return st
    except Exception:
        pass
    return {"device_id": "", "devices": {}, "applications": []}


def _save_card_state(st):
    tmp = CARD_STATE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, ensure_ascii=False, indent=1)
    os.replace(tmp, CARD_STATE_PATH)


def _ledger_append(device_id, qkey, action, delta, balance, note=""):
    """追加一行账本：设备号、时间、题号、动作、余额变化、动作后余额。"""
    rec = {
        "ts": int(time.time()),
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "day": _today_str(),
        "device_id": device_id,
        "qkey": qkey,
        "action": action,
        "delta": delta,
        "balance": balance,
        "note": note,
    }
    with open(CARD_LEDGER_PATH, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    print("ledger: dev=%s qkey=%s action=%s delta=%+d balance=%d %s"
          % (device_id, qkey, action, delta, balance, note), flush=True)
    return {k: rec[k] for k in ("ts", "time", "action", "qkey", "delta", "balance", "note")}


def _device_state(st, device_id):
    devs = st["devices"]
    dev = devs.get(device_id)
    if not isinstance(dev, dict):
        dev = {
            "day": _today_str(),
            "quota": CARD_DAILY_QUOTA,
            "balance": CARD_DAILY_QUOTA,
            "used_today": 0,
            "preborrow_total": 0,
            "unlocked": {},
            "records": [],
        }
        devs[device_id] = dev
    dev.setdefault("unlocked", {})
    dev.setdefault("records", [])
    dev.setdefault("preborrow_total", 0)
    dev.setdefault("used_today", 0)
    dev.setdefault("quota", CARD_DAILY_QUOTA)
    dev.setdefault("balance", CARD_DAILY_QUOTA)
    dev.setdefault("day", _today_str())
    # 20261030 定稿：预支单独记当日数、上限由网关强制校验；
    # preborrow_total 是历史累计（不清），preborrow_used_today 只算当日（跨日归零）。
    dev.setdefault("preborrow_used_today", 0)
    dev.setdefault("preborrow_limit", int(dev.get("quota", CARD_DAILY_QUOTA)))
    # 余额地板：任何时候不得小于 0（历史 -1 状态在这里一次性夹住）
    if int(dev.get("balance", 0)) < 0:
        dev["balance"] = 0
    st["device_id"] = device_id  # 单账号：记录最近活跃设备
    return dev


def _daily_rollover(st, dev, device_id):
    """跨日按当日配额重置；余额不累加（测试版口径）。"""
    today = _today_str()
    if dev.get("day") == today:
        return False
    dev["day"] = today
    dev["balance"] = int(dev.get("quota", CARD_DAILY_QUOTA))
    dev["used_today"] = 0
    # 次日不抵扣：昨日预支只留在 preborrow_total 历史里，当日预支数归零、额度照常给满
    dev["preborrow_used_today"] = 0
    rec = _ledger_append(device_id, "-", "daily_reset", dev["balance"], dev["balance"],
                         "跨日按当日配额重置（当日预支归零，历史累计 preborrow_total=%s）"
                         % dev.get("preborrow_total", 0))
    dev["records"].append(rec)
    del dev["records"][:-CARD_RECORDS_MAX]
    return True


def _status_payload(st, device_id):
    dev = _device_state(st, device_id)
    _daily_rollover(st, dev, device_id)
    pending = [{"application_id": a["application_id"], "qkey": a.get("qkey"),
                "ts": a.get("ts"), "time": a.get("time"), "status": a.get("status"),
                "amount": a.get("amount", CARD_APPROVE_AMOUNT)}
               for a in st["applications"]
               if a.get("device_id") == device_id and a.get("status") == "pending"]
    return {
        "ok": True,
        "gateway": GATEWAY_VERSION,
        "device_id": device_id,
        "day": dev.get("day"),
        "balance": dev.get("balance"),
        "daily_quota": dev.get("quota"),
        "used_today": dev.get("used_today"),
        "preborrow_total": dev.get("preborrow_total"),
        "preborrow_used_today": dev.get("preborrow_used_today", 0),
        "preborrow_limit": dev.get("preborrow_limit", dev.get("quota", CARD_DAILY_QUOTA)),
        "pending_count": len(pending),
        "pending_applications": pending,
        "records": dev.get("records", [])[-CARD_RECORDS_MAX:],
    }


def _card_unlock(device_id, qkey, batch=""):
    """有卡扣 1 张；同一 设备+批次+小题 只扣一次；没卡回 no_card 不扣。"""
    with CARD_LOCK:
        st = _load_card_state()
        dev = _device_state(st, device_id)
        _daily_rollover(st, dev, device_id)
        key = _batch_key(device_id, batch, qkey)
        if dev["unlocked"].get(key):
            _save_card_state(st)
            return {"ok": True, "unlocked": True, "charged": False, "dup": True,
                    "remaining": dev["balance"], "device_id": device_id}
        if int(dev["balance"]) <= 0:
            _save_card_state(st)
            return {"ok": True, "unlocked": False, "reason": "no_card", "remaining": 0,
                    "device_id": device_id}
        dev["balance"] -= 1
        dev["used_today"] += 1
        dev["unlocked"][key] = True
        rec = _ledger_append(device_id, qkey, "unlock", -1, dev["balance"],
                             "解题扣卡 key=%s balance=%d preborrow_today=%d/%d"
                             % (key, dev["balance"], dev.get("preborrow_used_today", 0),
                                dev.get("preborrow_limit", 0)))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "unlocked": True, "charged": True, "remaining": dev["balance"],
                "device_id": device_id}


def _card_apply(device_id, qkey, batch=""):
    """生成待批申请单；同设备已有 pending 单则不重复建（返回既有单）。
    申请单创建即落金额 5（家长批准时只改状态、不再写金额）。"""
    with CARD_LOCK:
        st = _load_card_state()
        dev = _device_state(st, device_id)
        key = _batch_key(device_id, batch, qkey)
        for a in st["applications"]:
            if a.get("device_id") == device_id and a.get("status") == "pending":
                _save_card_state(st)
                return {"ok": True, "application_id": a["application_id"], "status": "pending",
                        "amount": a.get("amount", CARD_APPROVE_AMOUNT), "dup": True,
                        "device_id": device_id}
        aid = "app-%d-%04d" % (int(time.time()), random.randint(0, 9999))
        item = {"application_id": aid, "device_id": device_id, "qkey": qkey,
                "ts": int(time.time()), "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                "status": "pending", "amount": CARD_APPROVE_AMOUNT}
        st["applications"].append(item)
        rec = _ledger_append(device_id, qkey, "apply", 0, dev["balance"],
                             "向家长申请 %s %d 张 key=%s balance=%d preborrow_today=%d/%d"
                             % (aid, CARD_APPROVE_AMOUNT, key, dev["balance"],
                                dev.get("preborrow_used_today", 0),
                                dev.get("preborrow_limit", 0)))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "application_id": aid, "status": "pending",
                "amount": CARD_APPROVE_AMOUNT, "device_id": device_id}


def _card_approve(aid):
    """家长批准：只改状态，金额按申请单创建时落定的 amount 入账（不再重写金额）。"""
    with CARD_LOCK:
        st = _load_card_state()
        hit = None
        for a in st["applications"]:
            if a.get("application_id") == aid:
                hit = a
                break
        if hit is None:
            return None, 404, "申请单不存在"
        if hit.get("status") == "approved":
            return None, 409, "该申请单已批准过"
        if hit.get("status") == "cancelled":
            return None, 409, "该申请单已取消，不能批准"
        device_id = hit["device_id"]
        dev = _device_state(st, device_id)
        _daily_rollover(st, dev, device_id)
        amount = int(hit.get("amount", CARD_APPROVE_AMOUNT))
        hit["status"] = "approved"
        hit["approve_ts"] = int(time.time())
        dev["balance"] += amount
        if dev["balance"] < 0:
            dev["balance"] = 0
        rec = _ledger_append(device_id, hit.get("qkey"), "approve", amount,
                             dev["balance"],
                             "家长批准 %s +%d 张 key=%s balance=%d preborrow_today=%d/%d"
                             % (aid, amount, hit.get("qkey") or "-", dev["balance"],
                                dev.get("preborrow_used_today", 0),
                                dev.get("preborrow_limit", 0)))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "application_id": aid, "amount": amount,
                "balance": dev["balance"], "device_id": device_id, "status": "approved"}, 200, ""


def _card_cancel(aid):
    """取消申请：pending -> cancelled；已批不准取消。"""
    with CARD_LOCK:
        st = _load_card_state()
        for a in st["applications"]:
            if a.get("application_id") == aid:
                if a.get("status") == "approved":
                    return None, 409, "该申请单已批准，不能取消"
                if a.get("status") == "cancelled":
                    return {"ok": True, "application_id": aid, "status": "cancelled"}, 200, ""
                a["status"] = "cancelled"
                a["cancel_ts"] = int(time.time())
                dev = _device_state(st, a["device_id"])
                rec = _ledger_append(a["device_id"], a.get("qkey"), "cancel", 0, dev["balance"],
                                     "取消申请 %s" % aid)
                dev["records"].append(rec)
                del dev["records"][:-CARD_RECORDS_MAX]
                _save_card_state(st)
                return {"ok": True, "application_id": aid, "status": "cancelled"}, 200, ""
        return None, 404, "申请单不存在"


def _card_preborrow(device_id, qkey, batch=""):
    """自己预支：每次只给 1 张，余额不动（stays 0，永不为负），单独记当日预支数；
    当日预支上限由网关强制校验，到限直接拦下（只能向家长申请或等次日）。"""
    with CARD_LOCK:
        st = _load_card_state()
        dev = _device_state(st, device_id)
        _daily_rollover(st, dev, device_id)
        key = _batch_key(device_id, batch, qkey)
        used = int(dev.get("preborrow_used_today", 0))
        limit = int(dev.get("preborrow_limit", dev.get("quota", CARD_DAILY_QUOTA)))
        # 同一 key 已解锁则不重复计数（重复点按不消耗当日预支）
        if dev["unlocked"].get(key):
            _save_card_state(st)
            return {"ok": True, "unlocked": True, "charged": False, "preborrow": True,
                    "dup": True, "remaining": dev["balance"],
                    "preborrow_used_today": used, "preborrow_limit": limit,
                    "preborrow_total": dev["preborrow_total"], "device_id": device_id}
        # 网关强制校验当日预支上限：到限不扣不增，余额保持不变
        if used >= limit:
            rec = _ledger_append(device_id, qkey, "preborrow_block", 0, dev["balance"],
                                 "预支到限被拦 key=%s balance=%d preborrow_today=%d/%d"
                                 % (key, dev["balance"], used, limit))
            dev["records"].append(rec)
            del dev["records"][:-CARD_RECORDS_MAX]
            _save_card_state(st)
            return {"ok": False, "unlocked": False, "charged": False,
                    "preborrow": True, "reason": "preborrow_limit",
                    "remaining": dev["balance"], "preborrow_used_today": used,
                    "preborrow_limit": limit, "preborrow_total": dev["preborrow_total"],
                    "device_id": device_id}
        # 正常预支：余额不动，只加当日预支数与历史累计
        dev["preborrow_used_today"] = used + 1
        dev["preborrow_total"] += 1
        dev["unlocked"][key] = True
        rec = _ledger_append(device_id, qkey, "preborrow", 0, dev["balance"],
                             "自己预支 1 张（家长记录可见） key=%s balance=%d preborrow_today=%d/%d"
                             % (key, dev["balance"], dev["preborrow_used_today"], limit))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "unlocked": True, "charged": True, "preborrow": True,
                "remaining": dev["balance"],
                "preborrow_used_today": dev["preborrow_used_today"],
                "preborrow_limit": limit,
                "preborrow_total": dev["preborrow_total"],
                "device_id": device_id}


def _card_dev_reset(device_id, balance, quota, reset_unlocked=True, preborrow_limit=None):
    """测试专用：把某设备余额/配额直接改成指定值。
    reset_unlocked=True 时同时清空该设备的已解锁题表，让同一 qkey 能重走 no_card 分支。
    同时清当日预支数；余额按地板夹到 >= 0；可指定当日预支上限（测试挡第 2 次预支）。"""
    with CARD_LOCK:
        st = _load_card_state()
        dev = _device_state(st, device_id)
        if quota is not None:
            dev["quota"] = int(quota)
        if balance is not None:
            dev["balance"] = max(0, int(balance))
        if preborrow_limit is not None:
            dev["preborrow_limit"] = max(0, int(preborrow_limit))
        dev["used_today"] = 0
        dev["preborrow_used_today"] = 0
        dev["day"] = _today_str()
        if reset_unlocked:
            dev["unlocked"] = {}
        rec = _ledger_append(device_id, "-", "dev_reset", 0, dev["balance"],
                             "测试重置 balance=%d quota=%d preborrow_limit=%d reset_unlocked=%s"
                             % (dev["balance"], dev["quota"],
                                dev.get("preborrow_limit", dev["quota"]),
                                "Y" if reset_unlocked else "N"))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "device_id": device_id, "balance": dev["balance"],
                "daily_quota": dev["quota"], "preborrow_limit": dev.get("preborrow_limit", dev["quota"]),
                "preborrow_used_today": dev["preborrow_used_today"],
                "reset_unlocked": bool(reset_unlocked)}


def _card_set_quota(device_id, quota, preborrow_limit=None):
    """App 改每日张数：配额立即生效，当日余额 = 配额 - 今日已用（不小于 0）。
    可同时改当日预支上限（家长设置），上限由网关在预支时强制校验。"""
    quota = max(0, int(quota))
    with CARD_LOCK:
        st = _load_card_state()
        device_id = device_id or str(st.get("device_id") or "")
        if not device_id:
            return {"ok": False, "error": "no device yet"}
        dev = _device_state(st, device_id)
        _daily_rollover(st, dev, device_id)
        old_q = int(dev.get("quota", CARD_DAILY_QUOTA))
        old_b = int(dev.get("balance", 0))
        old_pl = int(dev.get("preborrow_limit", old_q))
        used = int(dev.get("used_today", 0))
        dev["quota"] = quota
        if preborrow_limit is not None:
            dev["preborrow_limit"] = max(0, int(preborrow_limit))
        new_b = max(0, quota - used)
        dev["balance"] = new_b
        rec = _ledger_append(device_id, "-", "quota", new_b - old_b, new_b,
                             "改每日张数 %d->%d preborrow_limit %d->%d used_today=%d"
                             % (old_q, quota, old_pl, dev.get("preborrow_limit", old_pl), used))
        dev["records"].append(rec)
        del dev["records"][:-CARD_RECORDS_MAX]
        _save_card_state(st)
        return {"ok": True, "device_id": device_id, "daily_quota": quota,
                "balance": new_b, "used_today": used,
                "preborrow_limit": dev.get("preborrow_limit", quota),
                "preborrow_used_today": int(dev.get("preborrow_used_today", 0))}


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

    def _read_body(self, limit=262144):
        """读较大的 JSON 请求体（/split 送整段 API 文本用）；空/非法返回 None。"""
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            return None
        if length <= 0 or length > limit:
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception:
            return None

    def _api_split(self):
        """文本入口：与板端/本地大模型同一套解析，只做分题切卡 + leak 记录。"""
        raw = self._read_body()
        text = raw.get("text") if isinstance(raw, dict) else None
        if not isinstance(text, str) or not text.strip():
            self._send_json({"ok": False, "error": "text required"}, status=400)
            return
        t0 = time.time()
        _log_raw_text("split", text)
        cards = build_cards(text)
        leak_flags = _scan_text_leaks(text, None)
        ms = int((time.time() - t0) * 1000)
        print("split %d chars -> %d cards (%d ms)" % (len(text), len(cards), ms), flush=True)
        self._send_json({"ok": True, "gateway": GATEWAY_VERSION, "latency_ms": ms,
                         "count": len(cards), "cards": cards, "leak_flags": leak_flags})

    def _api_card_status(self):
        """GET /card/status?device_id=xxx —— 余额、今日已用、预支总数、待批申请、用卡记录。
        单家庭账号测试版：不带 device_id 时取最近活跃设备（App 管理页用）。"""
        qs = parse_qs(urlparse(self.path).query)
        device_id = (qs.get("device_id") or [""])[0].strip()
        with CARD_LOCK:
            st = _load_card_state()
            if not device_id:
                device_id = st.get("device_id") or ""
            if not device_id:
                self._send_json({"ok": False, "error": "no device yet"}, status=404)
                return
            payload = _status_payload(st, device_id)
            _save_card_state(st)
        print("card status: dev=%s balance=%s used=%s preborrow=%s pending=%s"
              % (payload["device_id"], payload["balance"], payload["used_today"],
                 payload["preborrow_total"], payload["pending_count"]), flush=True)
        self._send_json(payload)

    def _api_card_ledger(self):
        """GET /card/ledger?device_id=&limit= —— 账本行（证据留档用）。"""
        qs = parse_qs(urlparse(self.path).query)
        device_id = (qs.get("device_id") or [""])[0].strip()
        try:
            limit = int((qs.get("limit") or ["200"])[0])
        except ValueError:
            limit = 200
        rows = []
        try:
            with open(CARD_LEDGER_PATH, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                    except Exception:
                        continue
                    if device_id and rec.get("device_id") != device_id:
                        continue
                    rows.append(rec)
        except FileNotFoundError:
            pass
        self._send_json({"ok": True, "count": len(rows), "rows": rows[-limit:]})

    def _api_card_action(self, path):
        """POST /card/unlock|apply|approve|cancel|preborrow|dev_reset 统一入口。"""
        body = self._read_json()
        if not isinstance(body, dict):
            self._send_json({"ok": False, "error": "bad json"}, status=400)
            return
        device_id = str(body.get("device_id") or "")[:64].strip()
        qkey = body.get("qkey")
        # 批次：App 显式传 batch_id/case_id 时优先，否则用 /solve 按题图哈希记住的最近批次
        batch = str(body.get("batch_id") or body.get("case_id") or "").strip()[:64]
        print("card_body: path=%s dev=%s qkey=%s batch=%s body=%s"
              % (path.rsplit("/", 1)[-1], device_id, qkey, batch or "(auto)",
                 json.dumps(body, ensure_ascii=False)[:300]), flush=True)

        if path == "/card/approve":
            aid = str(body.get("application_id") or "").strip()
            if not aid:
                self._send_json({"ok": False, "error": "application_id required"}, status=400)
                return
            out, code, err = _card_approve(aid)
            if out is None:
                self._send_json({"ok": False, "error": err}, status=code)
            else:
                self._send_json(out)
            return

        if path == "/card/cancel":
            aid = str(body.get("application_id") or "").strip()
            if not aid:
                self._send_json({"ok": False, "error": "application_id required"}, status=400)
                return
            out, code, err = _card_cancel(aid)
            if out is None:
                self._send_json({"ok": False, "error": err}, status=code)
            else:
                self._send_json(out)
            return

        if path == "/card/quota":
            try:
                q = body.get("daily_quota", body.get("quota"))
                q = int(q)
                pl = body.get("preborrow_limit")
                pl = None if pl is None else int(pl)
            except (TypeError, ValueError):
                self._send_json({"ok": False, "error": "daily_quota must be int"}, status=400)
                return
            self._send_json(_card_set_quota(device_id, q, pl))
            return

        if path == "/card/dev_reset":
            try:
                bal = body.get("balance")
                bal = None if bal is None else int(bal)
                quota = body.get("daily_quota")
                quota = None if quota is None else int(quota)
                pl = body.get("preborrow_limit")
                pl = None if pl is None else int(pl)
            except (TypeError, ValueError):
                self._send_json({"ok": False, "error": "balance/daily_quota must be int"}, status=400)
                return
            if not device_id:
                self._send_json({"ok": False, "error": "device_id required"}, status=400)
                return
            rr = body.get("reset_unlocked")
            self._send_json(_card_dev_reset(device_id, bal, quota,
                                            True if rr is None else bool(rr), pl))
            return

        if not device_id:
            self._send_json({"ok": False, "error": "device_id required"}, status=400)
            return
        if qkey is None or str(qkey) == "":
            self._send_json({"ok": False, "error": "qkey required"}, status=400)
            return

        if path == "/card/unlock":
            out = _card_unlock(device_id, qkey, batch)
        elif path == "/card/apply":
            out = _card_apply(device_id, qkey, batch)
        elif path == "/card/preborrow":
            out = _card_preborrow(device_id, qkey, batch)
        else:
            self._send_json({"ok": False, "error": "unknown card path"}, status=404)
            return
        print("card %s: dev=%s qkey=%s -> %s"
              % (path.rsplit("/", 1)[-1], device_id, qkey,
                 json.dumps(out, ensure_ascii=False)), flush=True)
        self._send_json(out)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            _ar = admin_routes.info(WB_MODEL_POOL) if admin_routes else {"source": "builtin", "pool": WB_PRIORITY_ORDER}
            self._send_json({"ok": True, "gateway": GATEWAY_VERSION, "admin_routes": _ar})
        elif path == "/card/status":
            self._api_card_status()
            return
        elif path == "/card/ledger":
            self._api_card_ledger()
            return
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
        if path == "/split":
            self._api_split()
            return
        if path == "/api/parent_request":
            self._api_parent_request()
            return
        if path == "/api/parent_approve":
            self._api_parent_approve()
            return
        if path == "/api/parent_cancel":
            self._api_parent_cancel()
            return
        if path.startswith("/card/"):
            self._api_card_action(path)
            return
        if path != "/solve":
            self._send_json({"ok": False, "error": "unknown path"}, status=404)
            return
        # /solve 也记设备号（header X-JT-Device 优先，其次 query device_id）
        _qs = parse_qs(urlparse(self.path).query)
        solve_dev = (self.headers.get("X-JT-Device")
                     or (_qs.get("device_id") or [""])[0] or "").strip()[:64]
        # 用例规格（header X-JT-Case 优先，其次 query case，最后环境默认值）
        case_id = (self.headers.get("X-JT-Case")
                   or (_qs.get("case") or [""])[0] or DEFAULT_CASE).strip()[:32]
        print("solve: device_id=%s case=%s" % (solve_dev or "(none)", case_id or "(none)"), flush=True)
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_IMAGE_BYTES:
            self._send_json({"ok": False, "error": "empty or oversize image"}, status=400)
            return
        jpeg_bytes = self.rfile.read(length)
        if not jpeg_bytes.startswith(b"\xff\xd8") or not jpeg_bytes.endswith(b"\xff\xd9"):
            self._send_json({"ok": False, "error": "not a jpeg"}, status=400)
            return
        # 20261030：拍题批次。App 拍照时生成一次、全程携带（header X-JT-Batch / query batch）；
        # 未携带时退回题图 sha1 前 12 位，仅作旧客户端兼容。
        solve_batch = (self.headers.get("X-JT-Batch")
                       or (_qs.get("batch") or [""])[0]
                       or hashlib.sha1(jpeg_bytes).hexdigest()[:12]).strip()[:64]
        if solve_dev:
            # 临时测试兜底：按设备记住最近批次，供未带 batch_id 的旧 /card/* 调用取用
            _LAST_CASE_BY_DEV[solve_dev] = solve_batch
        print("solve: batch=%s" % solve_batch, flush=True)
        os.makedirs(UPLOAD_DIR, exist_ok=True)
        saved_path = os.path.join(UPLOAD_DIR, "jt_%d.jpg" % int(time.time()))
        with open(saved_path, "wb") as f:
            f.write(jpeg_bytes)
        if os.environ.get("JT_SOLVE_FAKE") == "1":
            # 验收假解题：不碰真实模型，直接回三题罐头结果（文本切卡路径）
            # 20261030：可注入外部假答案文件（JSON 数组，每项一段完整罐头文本），用于初中题测试
            fake_path = os.environ.get("JT_SOLVE_FAKE_FILE")
            if fake_path and os.path.exists(fake_path):
                try:
                    with open(fake_path, "r", encoding="utf-8") as _ff:
                        _pool = json.load(_ff)
                    _idx = int(os.environ.get("JT_SOLVE_FAKE_IDX", "0"))
                    _resp = _pool[_idx % len(_pool)] if isinstance(_pool, list) and _pool else None
                    if _resp:
                        _env_idx = int(os.environ.get("JT_SOLVE_FAKE_ADVANCE", "0"))
                        if _env_idx:
                            os.environ["JT_SOLVE_FAKE_IDX"] = str(_idx + 1)
                        print("saved %s (%d bytes) -> FAKE response #%d from file" % (saved_path, len(jpeg_bytes), _idx), flush=True)
                        self._send_json({"ok": True, "text": _resp, "latency_ms": 1,
                                         "batch": solve_batch, "gateway": GATEWAY_VERSION + "+fake"})
                        return
                except Exception as _exc:
                    print("fake file load failed: %s" % _exc, flush=True)
            fake_text = (
                "第1题 思路\n凑十法，把 8 拆成 5 和 3。\n"
                "第1题 过程与答案\n7+8=15\n"
                "第2题 思路\n先算个位，再算十位。\n"
                "第2题 过程与答案\n23+15=38\n"
                "第3题 思路\n移项要变号。\n"
                "第3题 过程与答案\nx=6\n"
            )
            print("saved %s (%d bytes) -> FAKE response (JT_SOLVE_FAKE=1)" % (saved_path, len(jpeg_bytes)), flush=True)
            self._send_json({"ok": True, "text": fake_text, "latency_ms": 1,
                             "batch": solve_batch, "gateway": GATEWAY_VERSION + "+fake"})
            return
        t0 = time.time()
        try:
            text, timing = infer_image(jpeg_bytes, saved_path)
        except Exception as exc:  # 模型异常不炸链路，照实返回
            self._send_json({"ok": False, "error": "infer failed: %s" % exc}, status=500)
            return
        # 输出校验正则原样保留，但不再硬拦；命中只记 leak_flag，原文照发给板端
        leak_flags = _scan_text_leaks(text, saved_path)
        _log_raw_text("solve", text, saved_path)
        # NLP 只标记不改内容（原文判定，结果进返回 flags 与 api_prio_flags.jsonl）
        prio_info = _api_prio_flags(text, case_id)
        latency_ms = int((time.time() - t0) * 1000)
        print("saved %s (%d bytes) -> %d ms" % (saved_path, len(jpeg_bytes), latency_ms), flush=True)
        # 卡里只发洗过的；顶层 text 同步清洗（原文在 raw_texts.jsonl）
        out = {"ok": True, "text": clean_text_for_display(text), "latency_ms": latency_ms,
               "batch": solve_batch, "gateway": GATEWAY_VERSION}
        # 服务端规范化：把 AI 原文切成逐题卡（第N题·思路 / 第N题·过程与答案），
        # 这样客户端不用改也能按题号显示，括号小题也能算题。
        cards = build_cards(text)
        if cards:
            out["cards"] = cards
            # 清洗后的卡另存一份，供批量 test_cases.jsonl 指路（只多写文件）
            cards_path = os.path.splitext(saved_path)[0] + "_cards.json"
            try:
                with open(cards_path, "w", encoding="utf-8") as f:
                    json.dump(cards, f, ensure_ascii=False, indent=1)
                out["cleaned_cards_path"] = cards_path
            except Exception as exc:
                print("写清洗卡文件失败：%s" % exc, flush=True)
        out["raw_text_path"] = os.path.splitext(saved_path)[0] + ".json"
        out["flags"] = prio_info["flags"]
        out["api_prio"] = prio_info
        if leak_flags:
            # 只记录不硬拦：完整原文见 leak_flags.jsonl
            out["leak_flags"] = leak_flags
        if timing:
            shots = timing.pop("shots", None)
            out.update(timing)
            if shots:
                # shots 原样返回给 Pad 端渲染（截图嵌结果页，不用文本/HTML 重组代替）；
                # 同时落盘一份 PNG 作证据留底。png_b64 不解码、不重排，直接透传。
                out["shots"] = shots
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
    # 端口统一 8000（内网/公网同端口，2026-10-29 定）
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
    print("简题 %s 网关已启动" % GATEWAY_VERSION, flush=True)
    print("  固件 POST 地址：http://%s:%d/solve" % (lan_ip(), args.port), flush=True)
    print("  健康检查：http://%s:%d/health" % (lan_ip(), args.port), flush=True)
    print("  家长批准页：http://%s:%d/parent" % (lan_ip(), args.port), flush=True)
    print("  收图目录：%s" % UPLOAD_DIR, flush=True)
    print("  分题测试版：段标题 %s / 分卡记录 %s"
          % (SPLIT_LOG, CARD_LOG), flush=True)
    print("  文本入口：POST http://%s:%d/split  body={\"text\": \"...\"}"
          % (lan_ip(), args.port), flush=True)
    print("  卡内数学算式拆行合并 recombine_math_lines（清洗后、出卡前，思路/过程同一套），原文 %s"
          % RAW_TEXT_LOG, flush=True)
    print("  卡账本：账本 %s / 状态 %s（默认每日 %d 张，批准 +%d 张）"
          % (CARD_LEDGER_PATH, CARD_STATE_PATH, CARD_DAILY_QUOTA, CARD_APPROVE_AMOUNT), flush=True)
    print("  卡接口：GET /card/status /card/ledger；POST /card/unlock /card/apply /card/approve /card/cancel /card/preborrow /card/quota",
          flush=True)
    if os.environ.get("JT_SOLVE_FAKE") == "1":
        print("  *** JT_SOLVE_FAKE=1 假解题模式：/solve 回罐头结果，不碰真实模型 ***", flush=True)
    _backend = os.environ.get("JT_VLM_BACKEND", "dashscope").strip().lower() or "dashscope"
    if _backend == "wbapi":
        _bp = admin_routes.current_pool(WB_MODEL_POOL) if admin_routes else WB_MODEL_POOL
        print("  推理后端：本机 WorkBuddy API（%s），模型池 %s（顺序来自管理页导出）"
              % (WB_API_BASE, " → ".join(s["model"] for s in _bp)), flush=True)
        if admin_routes:
            print("  只读路由文件：%s" % admin_routes.ADMIN_ROUTES_PATH, flush=True)
        print("  模型调用记录：%s" % WB_TRACE_PATH, flush=True)
        print("  API 优先版（8000）：用例规格目录 %s，默认用例 %s，flags 日志 %s"
              % (CASES_DIR, DEFAULT_CASE or "(none)", FLAGS_LOG), flush=True)
    elif _backend == "ollama":
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
