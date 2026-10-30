"""只读读取管理页导出的 gateway_routes.json，给网关模型池提供顺序（仅测试副本使用）。

第 1 段约束：
- 只读，不写任何文件；文件缺失 / 损坏 / 格式不对 → 回落内置池，绝不抛到调用方；
- 开关关（enabled=false）、等级 <= 0 的路由不进池；
- 按等级从高到低排序，同模型去重；
- 只收 type=api 的路由（wbapi 分支只会走 OpenAI 风格的 HTTP 接口，local/webui 不适用）；
- 按 mtime+size 变化触发重载，默认 30 秒检查一次。
"""
import json
import os
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _default_path():
    """定位管理页导出的路由文件。

    顺序：环境变量 JT_ADMIN_ROUTES → 本目录 admin_routes_path.txt 里写的绝对路径 →
    从本目录逐级向上找 admin\\data\\gateway_routes.json。
    """
    env = os.environ.get("JT_ADMIN_ROUTES", "").strip()
    if env:
        return os.path.normpath(env)
    pointer = os.path.join(HERE, "admin_routes_path.txt")
    try:
        with open(pointer, "r", encoding="utf-8") as f:
            line = f.read().strip()
        if line:
            return os.path.normpath(line)
    except OSError:
        pass
    cur = HERE
    for _ in range(6):
        cand = os.path.join(cur, "admin", "data", "gateway_routes.json")
        if os.path.exists(cand):
            return cand
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    return os.path.join(HERE, "gateway_routes.json")


ADMIN_ROUTES_PATH = _default_path()
TTL = float(os.environ.get("JT_ADMIN_ROUTES_TTL", "30"))
SLOT_TIMEOUT = int(os.environ.get("JT_ADMIN_ROUTES_T", "60"))

_lock = threading.Lock()
_cache = {"sig": None, "slots": [], "source": "builtin", "error": "",
          "path": ADMIN_ROUTES_PATH, "loaded_at": 0.0, "checked_at": 0.0}


def _sig(path):
    try:
        st = os.stat(path)
        return (st.st_mtime_ns, st.st_size)
    except OSError:
        return None


def _parse(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict) or not isinstance(data.get("routes"), list):
        raise ValueError("格式不对：缺少 routes 数组")
    rows = []
    for item in data["routes"]:
        if not isinstance(item, dict):
            continue
        if item.get("enabled") is False:
            continue
        if str(item.get("type") or "api").strip().lower() != "api":
            continue
        try:
            level = int(item.get("level") or 0)
        except (TypeError, ValueError):
            level = 0
        if level <= 0:
            continue
        model = str(item.get("model") or "").strip()
        if not model:
            continue
        rows.append((level, model, str(item.get("name") or model)))
    rows.sort(key=lambda row: -row[0])
    seen, slots = set(), []
    for level, model, name in rows:
        if model in seen:
            continue
        seen.add(model)
        slots.append({"model": model, "timeout": SLOT_TIMEOUT, "level": level, "name": name})
    return slots


def refresh(force=False):
    now = time.time()
    with _lock:
        if not force and now - _cache["checked_at"] < TTL:
            return
        _cache["checked_at"] = now
        path = ADMIN_ROUTES_PATH
        sig = _sig(path)
        if sig == _cache["sig"] and _cache["loaded_at"]:
            return
        _cache["sig"] = sig
        _cache["path"] = path
        _cache["loaded_at"] = now
        if sig is None:
            _cache.update(slots=[], source="builtin", error="文件不存在")
            return
        try:
            slots = _parse(path)
        except Exception as exc:  # 坏文件一律回落，不往上抛
            _cache.update(slots=[], source="builtin", error=str(exc)[:200])
            return
        if not slots:
            _cache.update(slots=[], source="builtin", error="无可用 api 路由（关闭或等级 0）")
            return
        _cache.update(slots=slots, source="external", error="")


def current_pool(builtin):
    """返回 [{'model','timeout',...}]；任何异常都回落 builtin。

    内置池里同名的模型会把 reasoning_effort / max_tokens 等调用参数带过来——
    deepseek 必须 low + 16384 才出正文，丢了这层参数会静默变成空回复。
    """
    try:
        refresh()
    except Exception:
        return list(builtin)
    with _lock:
        slots = list(_cache["slots"])
    if not slots:
        return list(builtin)
    base = {s.get("model"): s for s in builtin}
    merged = []
    for s in slots:
        extra = base.get(s["model"]) or {}
        item = {"model": s["model"], "timeout": extra.get("timeout") or s["timeout"]}
        for key in ("reasoning_effort", "max_tokens"):
            if key in extra:
                item[key] = extra[key]
        merged.append(item)
    return merged


def info(builtin=None):
    """给 /health 用的只读快照（先按 TTL 检查一次，保证顺序能跟上文件变化）。"""
    try:
        refresh()
    except Exception:
        pass
    with _lock:
        external = bool(_cache["slots"])
        pool = [s["model"] for s in _cache["slots"]]
        if not pool and builtin:
            pool = [s["model"] for s in builtin]
        return {
            "source": "external" if external else "builtin",
            "pool": pool,
            "levels": {s["model"]: s["level"] for s in _cache["slots"]},
            "path": _cache["path"],
            "error": _cache["error"],
            "loaded_at": _cache["loaded_at"],
            "ttl": TTL,
        }
