"""本地存储：原子写、DPAPI 加密密钥、口令哈希、脱敏。

只读写 admin/ 自己的目录，不碰网关、不碰 8000 配置。
"""

import base64
import ctypes
import hashlib
import hmac
import json
import os
import secrets
import threading
import time
import uuid
from ctypes import wintypes

ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(ROOT_DIR, "data")
PROFILES_DIR = os.path.join(ROOT_DIR, "profiles")
STATIC_DIR = os.path.join(ROOT_DIR, "static")

CONFIG_PATH = os.path.join(DATA_DIR, "admin.json")
ROUTES_PATH = os.path.join(DATA_DIR, "routes.json")
SECRETS_PATH = os.path.join(DATA_DIR, "secrets.bin")
FIRST_RUN_PATH = os.path.join(DATA_DIR, "first_run_password.txt")

PBKDF2_ROUNDS = 240_000

_lock = threading.RLock()


# ---------------------------------------------------------------- 基础工具

def ensure_dirs():
    for d in (DATA_DIR, PROFILES_DIR):
        os.makedirs(d, exist_ok=True)


def now_ts():
    return time.time()


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def new_id(prefix="r"):
    return f"{prefix}_{uuid.uuid4().hex[:10]}"


def read_json(path, default=None):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return default


def write_bytes_atomic(path, data: bytes):
    """先写临时文件 + fsync，再 os.replace，避免半截文件被网关读到。"""
    with _lock:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = f"{path}.tmp-{os.getpid()}-{secrets.token_hex(4)}"
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)


def write_json_atomic(path, payload):
    body = json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8")
    write_bytes_atomic(path, body)


# ---------------------------------------------------------------- DPAPI

class _BLOB(ctypes.Structure):
    _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def _dpapi(data: bytes, unprotect: bool = False) -> bytes:
    """Windows DPAPI：只允许当前 Windows 用户解开，落盘文件被拷走也读不出。"""
    if os.name != "nt":
        raise OSError("密钥加密依赖 Windows DPAPI，当前系统不支持")

    buf = ctypes.create_string_buffer(data, max(len(data), 1))
    blob_in = _BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _BLOB()
    fn = ctypes.windll.crypt32.CryptUnprotectData if unprotect else ctypes.windll.crypt32.CryptProtectData
    ok = fn(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out))
    if not ok:
        raise OSError(f"DPAPI 调用失败，错误码 {ctypes.GetLastError()}")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


# ---------------------------------------------------------------- 口令

def hash_password(password: str, salt: str = None) -> dict:
    salt = salt or secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                                 bytes.fromhex(salt), PBKDF2_ROUNDS)
    return {"algo": "pbkdf2-sha256", "rounds": PBKDF2_ROUNDS,
            "salt": salt, "hash": digest.hex()}


def verify_password(password: str, record: dict) -> bool:
    if not record:
        return False
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                                 bytes.fromhex(record["salt"]), int(record["rounds"]))
    return hmac.compare_digest(digest.hex(), record["hash"])


def random_password():
    # 去掉易混字符，方便手抄
    alphabet = "abcdefghjkmnpqrstuvwxyz23456789"
    return "".join(secrets.choice(alphabet) for _ in range(16))


# ---------------------------------------------------------------- 配置

DEFAULT_COOLDOWN = {"fail_threshold": 3, "base_seconds": 300, "max_seconds": 3600}


def load_config():
    return read_json(CONFIG_PATH)


def save_config(cfg):
    write_json_atomic(CONFIG_PATH, cfg)


def default_config(password: str):
    return {
        "version": 1,
        "created_at": now_iso(),
        "password": hash_password(password),
        "token": secrets.token_urlsafe(24),
        "session_secret": secrets.token_hex(32),
        "chrome_path": "",
        "gateway_output": os.path.join(DATA_DIR, "gateway_routes.json"),
        "cooldown": dict(DEFAULT_COOLDOWN),
    }


# ---------------------------------------------------------------- 密钥

def load_secrets() -> dict:
    """{route_id: api_key}，落盘为 DPAPI 密文。"""
    raw = None
    try:
        with open(SECRETS_PATH, "rb") as f:
            raw = f.read()
    except FileNotFoundError:
        return {}
    if not raw:
        return {}
    try:
        return json.loads(_dpapi(raw, unprotect=True).decode("utf-8"))
    except Exception:
        # 解不开就当作没有，绝不把异常内容带进接口响应
        return {}


def save_secrets(mapping: dict):
    body = json.dumps(mapping, ensure_ascii=False).encode("utf-8")
    write_bytes_atomic(SECRETS_PATH, _dpapi(body))


def mask_key(key: str) -> str:
    """对外/对日志：只承认『设置过』，不透露任何字符。"""
    return "已设置" if key else ""


def redact(text: str, keys) -> str:
    out = str(text or "")
    for k in keys:
        if k and len(k) >= 6 and k in out:
            out = out.replace(k, "[已隐藏]")
    return out


# ---------------------------------------------------------------- 路由表

def load_routes() -> list:
    data = read_json(ROUTES_PATH, {"routes": []}) or {"routes": []}
    routes = data.get("routes") or []
    for r in routes:
        normalize_route(r)
    return routes


def save_routes(routes: list):
    write_json_atomic(ROUTES_PATH, {"version": 1, "updated_at": now_iso(), "routes": routes})


# ---------------------------------------------------------------- 变更记录

CHANGE_LOG_PATH = os.path.join(DATA_DIR, "change_log.jsonl")
CHANGE_LOG_MAX = 500


def load_changes(limit: int = 100) -> list:
    """读最近 limit 条变更记录；坏行直接跳过，绝不让记录文件影响主流程。"""
    out = []
    try:
        with open(CHANGE_LOG_PATH, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except Exception:
                    continue
    except FileNotFoundError:
        return []
    return out[-limit:] if limit > 0 else out


def append_change(entry: dict):
    """追加一条变更记录（JSONL），只保留最近 CHANGE_LOG_MAX 条。"""
    with _lock:
        items = load_changes(CHANGE_LOG_MAX)
        items.append(entry)
        items = items[-CHANGE_LOG_MAX:]
        body = "\n".join(json.dumps(x, ensure_ascii=False) for x in items) + "\n"
        write_bytes_atomic(CHANGE_LOG_PATH, body.encode("utf-8"))


def normalize_route(r: dict) -> dict:
    r.setdefault("id", new_id())
    r.setdefault("name", "未命名")
    r.setdefault("type", "api")
    r.setdefault("enabled", False)
    r["level"] = max(0, min(5, int(r.get("level", 0) or 0)))
    r.setdefault("status", "stopped")
    r.setdefault("last_test", None)
    r.setdefault("health", {"consecutive_fail": 0, "cooldown_until": 0})
    t = r["type"]
    if t == "webui":
        r.setdefault("profile_dir", "")
        r.setdefault("check_url", "")
        r.setdefault("debug_port", 0)
        r.setdefault("login_state", "unknown")
        r.setdefault("headless", True)
    elif t == "api":
        r.setdefault("base_url", "")
        r.setdefault("model", "")
    elif t == "local":
        r.setdefault("provider", "ollama")
        r.setdefault("model", "")
        r.setdefault("file_path", "")
        r.setdefault("endpoint", "http://127.0.0.1:11434")
    return r
