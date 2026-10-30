"""路由引擎：单条测试、状态派生、冷却、整表校验、网关表落盘。"""

import hashlib
import json
import os
import threading
import time
import urllib.error
import urllib.request

import chrome
import store

TEST_TIMEOUT = 10

# 草稿测试凭据：只有服务端真跑过一次、且路由配置指纹一致，才认这条测试结果。
# 这样"导入即测"可以先测后存，而前端无法伪造 last_test.ok。
_STAMPS = {}
_STAMP_LOCK = threading.Lock()
STAMP_TTL = 1800


def stamp_of(route, api_key=""):
    # 带上 id：同一条路由、同一份配置才算「测过」，避免别的路由蹭同一个结果
    parts = [
        str(route.get("id") or ""),
        str(route.get("type") or ""),
        str(route.get("base_url") or "").strip(),
        str(route.get("model") or "").strip(),
        str(route.get("provider") or ""),
        str(route.get("endpoint") or "").strip(),
        str(route.get("file_path") or "").strip(),
        str(route.get("profile_dir") or "").strip(),
        str(route.get("debug_port") or ""),
        api_key or "",
    ]
    return hashlib.sha256("\x1f".join(parts).encode("utf-8")).hexdigest()


def remember_test(route, api_key, result):
    if not result:
        return
    with _STAMP_LOCK:
        _STAMPS[stamp_of(route, api_key)] = {"result": dict(result), "ts": time.time()}
        if len(_STAMPS) > 2000:
            cutoff = time.time() - STAMP_TTL
            for k, v in list(_STAMPS.items()):
                if v["ts"] < cutoff:
                    _STAMPS.pop(k, None)


def recall_test(route, api_key, max_age=STAMP_TTL):
    with _STAMP_LOCK:
        got = _STAMPS.get(stamp_of(route, api_key))
    if not got or time.time() - got["ts"] > max_age:
        return None
    return dict(got["result"])


# ---------------------------------------------------------------- HTTP 小工具

def _req(url, payload=None, headers=None, timeout=TEST_TIMEOUT):
    data = None
    hdrs = {"User-Agent": "gateway-route-admin/1.0"}
    hdrs.update(headers or {})
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=hdrs,
                                 method="POST" if payload is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read().decode("utf-8", "replace")
        return r.status, body


def _models_url(base: str) -> str:
    b = base.rstrip("/")
    return b + "/models" if b.endswith("/v1") else b + "/v1/models"


def _chat_url(base: str) -> str:
    b = base.rstrip("/")
    return b + "/chat/completions" if b.endswith("/v1") else b + "/v1/chat/completions"


# ---------------------------------------------------------------- 单条测试

def _result(ok, detail, latency_ms, keys=()):
    return {
        "ok": bool(ok),
        "detail": store.redact(detail, keys),
        "latency_ms": int(latency_ms),
        "at": store.now_iso(),
    }


def test_route(route, api_key=""):
    """只测这一条，绝不批量、绝不改别人的状态。"""
    t0 = time.time()
    kind = route.get("type")
    try:
        if kind == "webui":
            return _test_webui(route, t0)
        if kind == "api":
            return _test_api(route, api_key, t0)
        if kind == "local":
            return _test_local(route, t0)
        return _result(False, f"未知类型 {kind}", 0)
    except Exception as e:
        return _result(False, f"测试异常：{type(e).__name__}: {e}",
                       (time.time() - t0) * 1000, (api_key,))


def _test_webui(route, t0):
    port = int(route.get("debug_port") or 0)
    if not port:
        return _result(False, "未分配调试端口", 0)
    if chrome.devtools_up(port):
        ms = (time.time() - t0) * 1000
        return _result(True, f"浏览器可用（调试端口 {port}）", ms)
    return _result(False, f"未启动：调试端口 {port} 无响应，先点「启动」", (time.time() - t0) * 1000)


def _test_api(route, api_key, t0):
    base = (route.get("base_url") or "").strip()
    model = (route.get("model") or "").strip()
    if not base:
        return _result(False, "未填地址", 0)
    headers = {}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    # 1) 先看模型列表：既能验通，也能验模型名是否存在
    try:
        status, body = _req(_models_url(base), headers=headers)
        if status == 200:
            ms = (time.time() - t0) * 1000
            try:
                data = json.loads(body)
                ids = [str(m.get("id", "")) for m in (data.get("data") or [])]
            except Exception:
                ids = []
            if model and ids and model not in ids:
                return _result(False, f"连通但模型不在列表：{model}（列表含 {len(ids)} 个）", ms, (api_key,))
            return _result(True, f"连通，模型 {model or '未指定'} 可用", ms, (api_key,))
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403, 404, 405):
            return _result(False, f"HTTP {e.code}", (time.time() - t0) * 1000, (api_key,))
        if e.code in (401, 403):
            return _result(False, f"鉴权失败 HTTP {e.code}（key 不对或未授权）",
                           (time.time() - t0) * 1000, (api_key,))
    except Exception as e:
        return _result(False, f"连不上：{type(e).__name__}: {e}",
                       (time.time() - t0) * 1000, (api_key,))

    # 2) 没有 /models 端点，退一步打一次最小 chat
    if not model:
        return _result(False, "地址通但无 /models 端点，且未填模型名，无法验证", (time.time() - t0) * 1000)
    try:
        status, body = _req(_chat_url(base), payload={
            "model": model,
            "messages": [{"role": "user", "content": "ping"}],
            "max_tokens": 1,
            "stream": False,
        }, headers=headers, timeout=25)
        ms = (time.time() - t0) * 1000
        if status == 200:
            return _result(True, f"最小请求通过，模型 {model}", ms, (api_key,))
        return _result(False, f"HTTP {status}", ms, (api_key,))
    except urllib.error.HTTPError as e:
        return _result(False, f"最小请求 HTTP {e.code}", (time.time() - t0) * 1000, (api_key,))
    except Exception as e:
        return _result(False, f"最小请求失败：{type(e).__name__}", (time.time() - t0) * 1000, (api_key,))


def _test_local(route, t0):
    provider = route.get("provider") or "ollama"
    if provider == "file":
        path = (route.get("file_path") or "").strip()
        if not path:
            return _result(False, "未选本地模型文件", 0)
        if not os.path.isfile(path):
            return _result(False, "文件不存在", (time.time() - t0) * 1000)
        size = os.path.getsize(path)
        if size <= 0:
            return _result(False, "文件为空", (time.time() - t0) * 1000)
        ms = (time.time() - t0) * 1000
        return _result(True, f"已识别文件 {os.path.basename(path)}（{size / 1048576:.1f} MB）"
                             f"｜稳定性：未验证（原样记）", ms)

    endpoint = (route.get("endpoint") or "http://127.0.0.1:11434").rstrip("/")
    model = (route.get("model") or "").strip()
    if not model:
        return _result(False, "未选 ollama 模型名", 0)
    try:
        status, body = _req(endpoint + "/api/tags")
    except Exception as e:
        return _result(False, f"连不上 ollama（{endpoint}）：{type(e).__name__}",
                       (time.time() - t0) * 1000)
    ms = (time.time() - t0) * 1000
    if status != 200:
        return _result(False, f"ollama HTTP {status}", ms)
    try:
        names = [str(m.get("name", "")) for m in (json.loads(body).get("models") or [])]
    except Exception:
        names = []
    hit = model in names or f"{model}:latest" in names or any(n.split(":")[0] == model for n in names)
    if not hit:
        return _result(False, f"ollama 里没有该模型：{model}", ms)
    return _result(True, f"已识别 {model}｜稳定性：未验证（原样记）", ms)


def ollama_models(endpoint):
    endpoint = (endpoint or "http://127.0.0.1:11434").rstrip("/")
    try:
        status, body = _req(endpoint + "/api/tags")
        if status != 200:
            return [], f"HTTP {status}"
        names = [str(m.get("name", "")) for m in (json.loads(body).get("models") or [])]
        return sorted(n for n in names if n), ""
    except Exception as e:
        return [], f"连不上：{type(e).__name__}"


# ---------------------------------------------------------------- 状态 / 冷却

def status_of(route, now=None):
    now = now or time.time()
    if not route.get("enabled") or int(route.get("level") or 0) == 0:
        return "stopped"
    h = route.get("health") or {}
    if float(h.get("cooldown_until") or 0) > now:
        return "cooldown"
    lt = route.get("last_test") or {}
    return "ready" if lt.get("ok") else "failed"


def apply_result(route, result, cfg):
    h = route.setdefault("health", {"consecutive_fail": 0, "cooldown_until": 0})
    route["last_test"] = result
    if result["ok"]:
        h["consecutive_fail"] = 0
        h["cooldown_until"] = 0
    else:
        n = int(h.get("consecutive_fail") or 0) + 1
        h["consecutive_fail"] = n
        cd = (cfg or {}).get("cooldown") or store.DEFAULT_COOLDOWN
        threshold = int(cd.get("fail_threshold", 3))
        if n >= threshold:
            exp = n - threshold
            secs = min(int(cd.get("base_seconds", 300)) * (2 ** exp),
                       int(cd.get("max_seconds", 3600)))
            h["cooldown_until"] = time.time() + secs
    route["status"] = status_of(route)
    return route


def public_route(route, key_present=False):
    """给前端的视图：密钥只回答『有没有设置』，一个字符都不回显。"""
    out = {k: v for k, v in route.items() if not k.startswith("_")}
    out.pop("api_key", None)
    out["key_set"] = bool(key_present)
    out["status"] = status_of(route)
    return out


# ---------------------------------------------------------------- 校验

def validate(routes, secrets_map):
    errs = []
    seen_names, seen_ids = set(), set()
    for i, r in enumerate(routes):
        label = r.get("name") or f"第 {i + 1} 行"
        if not str(r.get("name") or "").strip():
            errs.append(f"第 {i + 1} 行：路由名不能为空")
        if r.get("name") in seen_names:
            errs.append(f"{label}：路由名重复")
        seen_names.add(r.get("name"))
        if r.get("id") in seen_ids:
            errs.append(f"{label}：id 重复")
        seen_ids.add(r.get("id"))
        if r.get("type") not in ("webui", "api", "local"):
            errs.append(f"{label}：类型非法")
            continue
        lvl = int(r.get("level") or 0)
        if not 0 <= lvl <= 5:
            errs.append(f"{label}：等级必须 0–5")
        if lvl == 0 and r.get("enabled"):
            errs.append(f"{label}：等级 0 表示不启动，不能同时开着")
        if not r.get("enabled"):
            continue
        lt = r.get("last_test") or {}
        t = r["type"]
        if t == "api":
            if not (r.get("base_url") or "").strip():
                errs.append(f"{label}：API 未填地址")
            if not lt.get("ok"):
                errs.append(f"{label}：API 必须先测通才能进池")
        elif t == "local":
            if not lt.get("ok"):
                errs.append(f"{label}：本地模型必须先测通才能进池")
        elif t == "webui":
            pd = (r.get("profile_dir") or "").strip()
            if not pd:
                errs.append(f"{label}：未选 profile 目录")
            elif not os.path.isdir(pd):
                errs.append(f"{label}：profile 目录不存在")
            if int(r.get("debug_port") or 0) <= 0:
                errs.append(f"{label}：未分配调试端口")
            if r.get("login_state") == "expired":
                errs.append(f"{label}：登录已过期，先点「登录」")
    return errs


# ---------------------------------------------------------------- 网关表

def build_gateway_payload(routes, key_ids=()):
    """只含启用且等级>0 的路由，按等级从高到低。密钥不出本机服务。"""
    key_ids = set(key_ids or ())
    active = [r for r in routes if r.get("enabled") and int(r.get("level") or 0) > 0]
    active.sort(key=lambda r: (-int(r.get("level") or 0), r.get("name") or ""))

    items = []
    for r in active:
        base = {
            "id": r.get("id"),
            "name": r.get("name"),
            "type": r.get("type"),
            "level": int(r.get("level") or 0),
        }
        if r["type"] == "webui":
            base.update({
                "profile_dir": r.get("profile_dir"),
                "debug_port": int(r.get("debug_port") or 0),
                "cdp_url": f"http://127.0.0.1:{int(r.get('debug_port') or 0)}",
            })
        elif r["type"] == "api":
            base.update({
                "base_url": r.get("base_url"),
                "model": r.get("model"),
                # 网关按 key_ref 从自己的密钥来源取，本文件永不含明文 key
                "key_ref": r.get("id"),
                "key_present": bool(r.get("id") in key_ids or r.get("key_set")),
            })
        elif r["type"] == "local":
            base.update({
                "provider": r.get("provider"),
                "model": r.get("model"),
                "file_path": r.get("file_path"),
                "endpoint": r.get("endpoint"),
            })
        items.append(base)

    return {
        "generated_at": store.now_iso(),
        "generator": "gateway-route-admin",
        "order": "level desc (5 最高, 1 最低)",
        "routes": items,
    }


def reload_gateway(routes, cfg, key_ids=()):
    path = (cfg or {}).get("gateway_output") or os.path.join(store.DATA_DIR, "gateway_routes.json")
    payload = build_gateway_payload(routes, key_ids)
    try:
        store.write_json_atomic(path, payload)
    except Exception as e:
        return {"ok": False, "path": path, "detail": f"写盘失败：{e}", "count": 0}
    return {
        "ok": True,
        "path": path,
        "count": len(payload["routes"]),
        "at": payload["generated_at"],
        "detail": f"已原子落盘 {len(payload['routes'])} 条启用路由，等网关自读",
    }
