"""judge.py — Qwen 客户端 + stub（标准库 urllib，平台服务器端 v1）.

规则（手册 v2 §2.1、§4.1，任务书）：
- Qwen 是主链路；API Key 只从环境变量 QWEN_API_KEY 读，绝不写进代码、
  文档、日志、报错返回。
- 没有 Key -> stub 模式：返回明确的“模型未接通”错误，绝不编假答案、
  绝不编对错/思路/答案。调用方（gateway）据此把 job 置为 failed。
- 有 Key -> 调 DashScope OpenAI 兼容接口做一次图文判题；调不通也如实
  返回失败，不编答案。模型名到 API model id 的映射仍是骨架自定
  （N1 已定：部署时再定，开发阶段留空，不追这条），见 ASSUMPTIONS.md V1-A1。
- 测试模式（v6，Nyx 500）：JIANTI_TEST_MODE=1 且无 Key 时，网关改调
  mock_judge_result() 走模拟返回，思路／答案明标“测试模拟”、job 打
  mock 标记；测试模式关时无 Key 仍按上条报 model_not_connected。
"""
from __future__ import annotations

import base64
import json
import os
import re
import time
import urllib.error
import urllib.request

MODEL_NOT_CONNECTED = {
    "code": "model_not_connected",
    "message": "模型未接通：未配置 QWEN_API_KEY，判题服务不可用，未生成任何答案。",
}

# 产品模型名 -> DashScope 兼容接口 model id（骨架自定映射，见 ASSUMPTIONS.md N1）。
# JT Lite 2.0 是官方默认模型，但其 API id 未在 PRD/原型中给出，这里按
# “默认走 Qwen 视觉模型”映射；Nyx/浩爷定下真实 id 后只改这一张表。
API_MODEL_MAP = {
    "jt-lite-2.0": "qwen-vl-max",
    "deepseek-v4.1-flash": "deepseek-v4.1-flash",  # 非 Qwen 系：经 Qwen 兼容通道不一定能调通
    "qwen-3.8": "qwen-vl-plus",
    "doubao-seed-2.1-turbo": "doubao-seed-2.1-turbo",
    "kimi-k3.1-pro": "kimi-k3.1-pro",
}
DASH_SCOPE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"

# E2 修复（NYX 17）：判题端点/模型可由环境变量覆写，供内网 7863 接线；
# 不设这两个变量时与交付版行为完全一致（默认 DashScope + 映射表）。
JUDGE_API_URL = os.environ.get("JUDGE_API_URL", DASH_SCOPE_URL)
JUDGE_API_MODEL = os.environ.get("JUDGE_API_MODEL", "")

JUDGE_PROMPT = (
    "你是小学数学批改老师。看这张作业照片，照片里有几道题就判几道，"
    "只判最终结果，不判过程。按 JSON 输出，不要输出其他文字："
    '{"results": [{"correct": true/false, "thinking": "分步思路，'
    '每步写算什么、算出什么，最后一步停在还差最后一下的地方，不给答案", '
    '"answer": "完整计算过程，一条等号链算到答案"}]}'
    "如果照片里没有可判的数学题，输出 {\"results\": []}。不要编造题目。"
)


MOCK_MARK = "【测试模拟数据，非真实模型输出】"


def mock_judge_result() -> dict:
    """测试模式专用模拟判题（v6，Nyx 500）。

    只在测试模式开（JIANTI_TEST_MODE=1）且无真 Key 时由网关调用，
    让 App 走完结果屏、扣卡、使用记录全链路；有真 Key 时不调用。
    返回与真实判题同结构的一道假题，思路与答案里明标“测试模拟”，
    job 上另打 mock=true／result_source="mock"，App 可凭字段区分，
    不得冒充真实模型输出。绝不在生产路径（测试模式关）里调用。
    """
    return {
        "results": [{
            "correct": True,
            "thinking": f"{MOCK_MARK}第一步：读题，确认这是一道加法题；"
                        "第二步：列式计算；第三步：验算结果。",
            "answer": f"{MOCK_MARK}3 + 4 = 7（模拟答案，仅供走通链路）。",
        }],
        "mock": True,
        "source": "mock",
    }


class ModelNotConnectedError(Exception):
    def __init__(self):
        super().__init__(MODEL_NOT_CONNECTED["message"])
        self.payload = MODEL_NOT_CONNECTED


class JudgeError(Exception):
    def __init__(self, message: str):
        super().__init__(message)
        self.payload = {"code": "judge_failed", "message": message}


def qwen_api_key() -> str | None:
    return os.environ.get("QWEN_API_KEY") or None


# ---- 判题后端（NYX 328 / 浩爷定：qw_bridge 千问网页桥优先，7864 从未通过弃用）----
# JUDGE_BACKEND: "openai"（默认，OpenAI 兼容 /v1/chat/completions，原链路不动）
#              | "qw_bridge"（v3.23 千问网页桥：POST 原始 JPEG -> {"ok","text"}）
BACKEND = os.environ.get("JUDGE_BACKEND", "openai").strip().lower()
BRIDGE_URL = os.environ.get("JUDGE_BRIDGE_URL", "").strip()
# 桥模式要等千问网页出答案（桥内 WAIT_ANSWER_S=240），默认放宽到 300 秒
JUDGE_TIMEOUT = int(os.environ.get(
    "JUDGE_TIMEOUT_SECONDS", "300" if BACKEND == "qw_bridge" else "60"))

_bridge_ok_cache = [0.0, False]


def _bridge_health() -> bool:
    """qw_bridge 模式的探活：真探桥 /health（缓存 30 秒，不假绿）。"""
    now = time.time()
    if now < _bridge_ok_cache[0]:
        return _bridge_ok_cache[1]
    ok = False
    try:
        root = BRIDGE_URL.rsplit("/", 1)[0]
        with urllib.request.urlopen(root + "/health", timeout=3) as r:
            ok = bool(json.loads(r.read().decode("utf-8")).get("ok"))
    except Exception:
        ok = False
    _bridge_ok_cache[0] = now + 30
    _bridge_ok_cache[1] = ok
    return ok


def is_connected() -> bool:
    if BACKEND == "qw_bridge":
        return bool(BRIDGE_URL) and _bridge_health()
    return bool(qwen_api_key())


def judge_image(image_bytes: bytes, model_id: str = "jt-lite-2.0",
                timeout: int | None = None) -> dict:
    """一次输入一张照片，一次输出对错+思路+答案（结构见 JUDGE_PROMPT）。

    返回 {"results": [{"correct": bool|None, "thinking": str, "answer": str}]}。
    后端未接通 raise ModelNotConnectedError；调用失败 raise JudgeError。
    任何失败路径都不返回编造的判题结果。
    timeout 不传时按后端取默认（openai 60s / qw_bridge 300s，可用
    JUDGE_TIMEOUT_SECONDS 覆写）。
    """
    if timeout is None:
        timeout = JUDGE_TIMEOUT
    if BACKEND == "qw_bridge":
        return _judge_image_bridge(image_bytes, timeout)
    key = qwen_api_key()
    if not key:
        raise ModelNotConnectedError()
    api_model = JUDGE_API_MODEL or API_MODEL_MAP.get(model_id, "qwen-vl-max")
    b64 = base64.b64encode(image_bytes).decode("ascii")
    body = {
        "model": api_model,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}"}},
                {"type": "text", "text": JUDGE_PROMPT},
            ],
        }],
        "temperature": 0,
    }
    req = urllib.request.Request(
        JUDGE_API_URL,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {key}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raise JudgeError(f"模型服务返回 HTTP {e.code}，判题未完成。")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise JudgeError(f"模型服务连接失败（{e}），判题未完成。")
    try:
        text = payload["choices"][0]["message"]["content"]
        parsed = json.loads(text)
        results = parsed.get("results", [])
        if not isinstance(results, list):
            raise ValueError("results 不是列表")
    except (KeyError, IndexError, ValueError, TypeError, json.JSONDecodeError):
        raise JudgeError("模型返回无法解析为判题结果，判题未完成。")
    return {"results": results, "raw_text": text}


def _parse_results_text(text: str) -> list:
    """模型/桥返回的 text -> results 列表。剥 markdown 代码围栏后按 JSON 解析。"""
    t = (text or "").strip()
    if t.startswith("```"):
        t = re.sub(r"^```[a-zA-Z]*\s*", "", t)
        t = re.sub(r"\s*```$", "", t)
    parsed = json.loads(t)
    results = parsed.get("results", [])
    if not isinstance(results, list):
        raise ValueError("results 不是列表")
    return results


def _judge_image_bridge(image_bytes: bytes, timeout: int) -> dict:
    """qw_bridge 后端（NYX 328 浩爷定）：POST 原始 JPEG -> {"ok","text"}。

    桥内提示词由 QW_PROMPT 覆写为 JUDGE_PROMPT 同款 JSON 指令（部署侧注入），
    返回 text 直接按 JSON 解析；失败路径一律 JudgeError，不编结果。
    """
    if not BRIDGE_URL:
        raise JudgeError("qw_bridge 未配置 JUDGE_BRIDGE_URL，判题未完成。")
    req = urllib.request.Request(
        BRIDGE_URL,
        data=image_bytes,
        headers={"Content-Type": "image/jpeg"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        raise JudgeError(f"千问桥返回 HTTP {e.code}，判题未完成。")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise JudgeError(f"千问桥连接失败（{e}），判题未完成。")
    if not payload.get("ok"):
        raise JudgeError(f"千问桥返回失败：{str(payload)[:160]}，判题未完成。")
    try:
        results = _parse_results_text(payload.get("text") or "")
    except (ValueError, json.JSONDecodeError):
        raise JudgeError("千问桥返回无法解析为判题 JSON，判题未完成。")
    return {"results": results, "raw_text": payload.get("text") or ""}
