"""管理页服务：只绑 127.0.0.1，密码登录换会话 Cookie，写操作校验 CSRF。

与正式网关（8000）完全无关：不代理、不重启、不改它的配置，
只在保存时把路由表原子落盘到配置路径，等网关自己读。
"""

import argparse
import hashlib
import hmac
import json
import mimetypes
import os
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import chrome
import engine
import store

BIND_HOST = "127.0.0.1"
COOKIE_NAME = "gw_admin_sid"
SESSION_TTL = 12 * 3600
LOGIN_WINDOW = 60
LOGIN_MAX_FAILS = 5

HERE_DIR = os.path.dirname(os.path.abspath(__file__))
SERVICE_PS = os.path.join(HERE_DIR, "service.ps1")
PID_FILE = os.path.join(store.DATA_DIR, "admin.pid")
AUTOSTART_TASK = "RouteAdminAutoStart"
STARTED_AT = time.time()
LOG_FILES = {
    "err": os.path.join(store.DATA_DIR, "admin.err.log"),
    "out": os.path.join(store.DATA_DIR, "admin.out.log"),
}
# 只读探测正式网关的 /health：拿它「此刻实际生效」的模型池，供界面核对
GATEWAY_HEALTH_URL = os.environ.get("JT_GATEWAY_HEALTH", "http://127.0.0.1:8000/health")
GATEWAY_HEALTH_TIMEOUT = 3

CFG = {}
SESSIONS = {}          # sid -> expires_at
LOGIN_FAILS = []       # 时间戳
STATE_LOCK = threading.RLock()


# ---------------------------------------------------------------- 启动初始化

def bootstrap(password=None, port=8791):
    store.ensure_dirs()
    cfg = store.load_config()
    first_run = False
    if not cfg:
        pw = password or store.random_password()
        cfg = store.default_config(pw)
        cfg["port"] = port
        store.save_config(cfg)
        first_run = True
        note = (
            "首次启动，已生成管理员凭证。登录后请立即修改密码并删除本文件。\n\n"
            f"管理页地址: http://{BIND_HOST}:{port}/\n"
            f"管理员密码  : {pw}\n"
            f"Bearer Token: {cfg['token']}\n"
        )
        with open(store.FIRST_RUN_PATH, "w", encoding="utf-8") as f:
            f.write(note)
        print(note, flush=True)
    return cfg, first_run


# ---------------------------------------------------------------- 工具

def new_sid():
    return secrets.token_urlsafe(32)


def csrf_for(sid: str) -> str:
    key = CFG["session_secret"].encode("utf-8")
    return hmac.new(key, sid.encode("utf-8"), hashlib.sha256).hexdigest()[:32]


def login_blocked() -> bool:
    now = time.time()
    with STATE_LOCK:
        LOGIN_FAILS[:] = [t for t in LOGIN_FAILS if now - t < LOGIN_WINDOW]
        return len(LOGIN_FAILS) >= LOGIN_MAX_FAILS


def routes_with_keys():
    routes = store.load_routes()
    secrets_map = store.load_secrets()
    out = []
    for r in routes:
        key = secrets_map.get(r["id"], "") if r["type"] == "api" else ""
        if r["type"] == "api":
            r["_key_present"] = bool(key)
            r["key_set"] = bool(key)
        out.append(r)
    return routes, secrets_map


def view(routes, secrets_map):
    views = []
    for r in routes:
        key = secrets_map.get(r["id"], "")
        views.append(engine.public_route(r, bool(key)))
    return views


# ---------------------------------------------------------------- 服务管理

def version():
    try:
        with open(os.path.join(HERE_DIR, "VERSION"), "r", encoding="utf-8") as f:
            return f.read().strip() or "0.0.0"
    except OSError:
        return "0.0.0"


def powershell_exe():
    exe = shutil.which("powershell.exe")
    if exe:
        return exe
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return os.path.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def run_service(action, timeout=30):
    """同步调 service.ps1，返回 (rc, 输出文本)。"""
    cmd = [powershell_exe(), "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", SERVICE_PS, "-Action", action]
    p = subprocess.run(cmd, cwd=HERE_DIR, capture_output=True, text=True, timeout=timeout,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    out = (p.stdout or "").strip()
    err = (p.stderr or "").strip()
    return p.returncode, (out + ("\n" + err if err else "")).strip()


def autostart_state():
    try:
        rc, out = run_service("autostart-status", timeout=15)
    except Exception:
        return False
    lines = [x.strip() for x in out.splitlines() if x.strip()]
    return rc == 0 and bool(lines) and lines[-1] == "on"


def spawn_service(action):
    """脱离本进程调 service.ps1：restart 时当前进程会被它杀掉，必须解耦。

    注意：不能用 DETACHED_PROCESS——powershell.exe 是无控制台就跑不起来的
    控制台程序，加了该标志会静默退出（rc=0 但什么都没做）。
    CREATE_NO_WINDOW 让它隐藏窗口运行，且不在本进程的作业对象里。
    """
    cmd = [powershell_exe(), "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", SERVICE_PS, "-Action", action]
    flags = 0
    if os.name == "nt":
        flags = (getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
                 | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x00000200))
    subprocess.Popen(cmd, cwd=HERE_DIR, close_fds=True, creationflags=flags,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)


def decode_log(raw):
    """日志解码：pythonw 的 stdout 被 cmd.exe 重定向时按系统代码页写（中文为 GBK）。"""
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("gbk", "replace")


def gateway_health():
    """只读探一次正式网关 /health，返回它当前实际生效的模型池。

    网关没起、端口不通、回包不是 JSON，都只算「没读到」：
    不重试、不抛异常、不改网关任何状态，界面据此提示而不是报错。
    """
    t0 = time.time()
    url = GATEWAY_HEALTH_URL
    try:
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=GATEWAY_HEALTH_TIMEOUT) as resp:
            raw = resp.read(256 * 1024)
        data = json.loads(raw.decode("utf-8", "replace"))
        if not isinstance(data, dict):
            raise ValueError("回包不是 JSON 对象")
    except Exception as e:
        msg = str(e) or e.__class__.__name__
        return {"ok": False, "url": url, "error": msg,
                "elapsed_ms": int((time.time() - t0) * 1000)}
    ar = data.get("admin_routes")
    if isinstance(ar, dict):
        try:
            note_gateway(ar)
        except Exception:
            pass
    return {
        "ok": True,
        "url": url,
        "gateway": data.get("gateway", ""),
        "admin_routes": ar if isinstance(ar, dict) else None,
        "elapsed_ms": int((time.time() - t0) * 1000),
        "checked_at": int(time.time()),
    }


_LAST_GW = {"key": None}


def note_gateway(ar):
    """网关实际读数有变化才记一条，避免 6 秒轮询把变更记录刷满。"""
    key = (
        ar.get("source") or "",
        tuple(ar.get("pool") or []),
        json.dumps(ar.get("levels") or {}, sort_keys=True, ensure_ascii=False),
        ar.get("error") or "",
    )
    if key == _LAST_GW["key"]:
        return
    _LAST_GW["key"] = key
    store.append_change({
        "ts": store.now_iso(),
        "kind": "gateway",
        "source": ar.get("source") or "",
        "pool": list(ar.get("pool") or []),
        "levels": ar.get("levels") or {},
        "error": ar.get("error") or "",
    })


def delayed(seconds, fn):
    def runner():
        time.sleep(seconds)
        try:
            fn()
        except Exception:
            pass
    threading.Thread(target=runner, daemon=True).start()


def hard_exit():
    """先回包再退出，避免浏览器拿到连接重置。"""
    try:
        if os.path.isfile(PID_FILE):
            os.remove(PID_FILE)
    except OSError:
        pass
    os._exit(0)


# ---------------------------------------------------------------- Handler

class Handler(BaseHTTPRequestHandler):
    server_version = "RouteAdmin/" + version()
    sys_version = ""

    # ---------- 低层 ----------

    def log_message(self, fmt, *args):
        # 不打印查询串与请求体，避免口令/密钥进日志
        sys.stderr.write("%s - %s %s\n" % (self.address_string(), self.command, urlparse(self.path).path))

    def _send(self, status, body: bytes, ctype="application/json; charset=utf-8", headers=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, status, obj, headers=None):
        self._send(status, json.dumps(obj, ensure_ascii=False).encode("utf-8"), headers=headers)

    def _err(self, status, message, code="error"):
        self._json(status, {"ok": False, "code": code, "error": message})

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        if n > 2 * 1024 * 1024:
            raise ValueError("请求体过大")
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            raise ValueError("请求体不是合法 JSON")

    def _cookies(self):
        out = {}
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k.strip()] = v.strip()
        return out

    # ---------- 鉴权 ----------

    def _bearer_ok(self):
        auth = self.headers.get("Authorization") or ""
        if not auth.startswith("Bearer "):
            return False
        return hmac.compare_digest(auth[7:].strip(), CFG.get("token", ""))

    def _sid(self):
        sid = self._cookies().get(COOKIE_NAME)
        if not sid:
            return None
        exp = SESSIONS.get(sid)
        if exp and exp > time.time():
            return sid
        SESSIONS.pop(sid, None)
        return None

    def _authed(self):
        return bool(self._sid()) or self._bearer_ok()

    def _csrf_ok(self):
        if self._bearer_ok():
            return True
        sid = self._sid()
        if not sid:
            return False
        got = self.headers.get("X-CSRF-Token") or ""
        return hmac.compare_digest(got, csrf_for(sid))

    def _guard(self):
        """返回 True 表示可以继续。"""
        if not self._authed():
            self._err(401, "未登录或会话已过期", "unauthorized")
            return False
        return True

    def _guard_write(self):
        if not self._guard():
            return False
        if not self._csrf_ok():
            self._err(403, "CSRF 校验失败", "csrf")
            return False
        return True

    # ---------- 路由分发 ----------

    def do_GET(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        if not parts:
            return self._serve_index()
        if parts[0] != "api":
            return self._err(404, "not found")

        if parts[1:] == ["auth", "status"]:
            sid = self._sid()
            return self._json(200, {
                "ok": True,
                "authenticated": bool(sid or self._bearer_ok()),
                "csrf": csrf_for(sid) if sid else "",
            })

        if not self._guard():
            return

        if parts[1:] == ["routes"]:
            with STATE_LOCK:
                routes, secrets_map = routes_with_keys()
                return self._json(200, {"ok": True, "routes": view(routes, secrets_map)})

        if parts[1:] == ["profiles"]:
            return self._json(200, {"ok": True, "profiles": chrome.list_profiles()})

        if parts[1:] == ["chrome"]:
            return self._json(200, {"ok": True, "detect": chrome.detect(CFG.get("chrome_path", ""))})

        if parts[1:] == ["ollama"]:
            endpoint = (self.headers.get("X-Ollama-Endpoint") or "http://127.0.0.1:11434")
            names, err = engine.ollama_models(endpoint)
            return self._json(200, {"ok": not err, "models": names, "error": err})

        if parts[1:] == ["settings"]:
            return self._json(200, {"ok": True, "settings": {
                "chrome_path": CFG.get("chrome_path", ""),
                "gateway_output": CFG.get("gateway_output", ""),
                "cooldown": CFG.get("cooldown", store.DEFAULT_COOLDOWN),
                "bind": f"{BIND_HOST}:{CFG.get('port')}",
                "token_set": bool(CFG.get("token")),
            }})

        if parts[1:] == ["service"]:
            return self._service_info()

        if parts[1:] == ["gateway", "health"]:
            return self._json(200, gateway_health())

        if parts[1:] == ["service", "logs"]:
            return self._service_logs()

        if parts[1:] == ["changelog"]:
            return self._changelog()

        return self._err(404, "not found")

    def do_POST(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]

        if parts[1:] == ["auth", "login"]:
            return self._login()

        if parts[1:] == ["auth", "logout"]:
            sid = self._sid()
            if sid:
                SESSIONS.pop(sid, None)
            return self._json(200, {"ok": True}, headers={
                "Set-Cookie": f"{COOKIE_NAME}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"
            })

        if not self._guard_write():
            return

        if parts[1:] == ["routes"]:
            return self._save_routes()

        if parts[1:] == ["routes", "test-draft"]:
            return self._test_draft()

        # /api/routes/{id}/<action>[/<sub>]
        if len(parts) >= 4 and parts[1] == "routes":
            rid, action = parts[2], parts[3]
            sub = parts[4] if len(parts) > 4 else ""
            return self._route_action(rid, action, sub)

        if parts[1:] == ["settings"]:
            return self._save_settings()

        if parts[1:] == ["password"]:
            return self._change_password()

        if parts[1:] == ["reload"]:
            with STATE_LOCK:
                routes = store.load_routes()
                res = engine.reload_gateway(routes, CFG, set(store.load_secrets()))
            return self._json(200 if res["ok"] else 500, {"ok": res["ok"], "reload": res})

        if parts[1:] == ["service", "autostart"]:
            return self._service_autostart()

        if parts[1:] == ["service", "restart"]:
            return self._service_restart()

        if parts[1:] == ["service", "exit"]:
            return self._service_exit()

        return self._err(404, "not found")

    def do_PUT(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        if not self._guard_write():
            return
        if parts[1:] == ["settings"]:
            return self._save_settings()
        return self._err(404, "not found")

    # ---------- 静态页 ----------

    def _serve_index(self):
        path = os.path.join(store.STATIC_DIR, "index.html")
        if not os.path.isfile(path):
            return self._err(500, "缺少 static/index.html")
        with open(path, "rb") as f:
            body = f.read()
        self._send(200, body, mimetypes.guess_type(path)[0] or "text/html; charset=utf-8")

    # ---------- 登录 ----------

    def _login(self):
        if login_blocked():
            return self._err(429, "登录尝试过多，请 1 分钟后再试", "rate_limited")
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        password = str(body.get("password") or "")
        token = str(body.get("token") or "")

        ok_pw = bool(password) and store.verify_password(password, CFG.get("password"))
        ok_tk = bool(token) and hmac.compare_digest(token, CFG.get("token", ""))
        if not (ok_pw or ok_tk):
            with STATE_LOCK:
                LOGIN_FAILS.append(time.time())
            return self._err(401, "口令不正确", "bad_credentials")

        with STATE_LOCK:
            LOGIN_FAILS.clear()
        sid = new_sid()
        SESSIONS[sid] = time.time() + SESSION_TTL
        cookie = (f"{COOKIE_NAME}={sid}; Path=/; HttpOnly; SameSite=Strict; "
                  f"Max-Age={SESSION_TTL}")
        return self._json(200, {"ok": True, "csrf": csrf_for(sid)}, headers={"Set-Cookie": cookie})

    def _change_password(self):
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        old = str(body.get("old") or "")
        new = str(body.get("new") or "")
        if not store.verify_password(old, CFG.get("password")):
            return self._err(403, "原密码不对")
        if len(new) < 10:
            return self._err(400, "新密码至少 10 位")
        CFG["password"] = store.hash_password(new)
        store.save_config(CFG)
        if os.path.isfile(store.FIRST_RUN_PATH):
            os.remove(store.FIRST_RUN_PATH)
        return self._json(200, {"ok": True})

    # ---------- 路由操作 ----------

    def _reload_response(self, routes, status=200):
        res = engine.reload_gateway(routes, CFG, set(store.load_secrets()))
        return self._json(status, {"ok": True, "reload": res})

    def _save_routes(self):
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        incoming = body.get("routes")
        if not isinstance(incoming, list):
            return self._err(400, "routes 必须是数组")

        with STATE_LOCK:
            old_routes, secrets_map = routes_with_keys()
            old_by_id = {r["id"]: r for r in old_routes}
            keys = body.get("keys") or {}

            cleaned = []
            for raw in incoming:
                r = {k: v for k, v in dict(raw).items() if not str(k).startswith("_")}
                r.pop("key_hint", None)
                r.pop("key_set", None)
                r.pop("api_key", None)
                r.pop("status", None)
                raw_key = (keys.get(r.get("id")) or "").strip()
                if raw_key:
                    secrets_map[r["id"]] = raw_key
                r = store.normalize_route(r)
                old = old_by_id.get(r["id"])
                # 服务端权威字段：测试结果与健康数据只能由服务端改
                if old:
                    r["last_test"] = old.get("last_test")
                    r["health"] = old.get("health")
                    if r["type"] == "webui":
                        r["login_state"] = old.get("login_state", "unknown")
                        r["debug_port"] = old.get("debug_port") or r.get("debug_port")
                # 导入即测：草稿阶段服务端真测过的结果，按配置指纹认回来
                if r["type"] in ("api", "local"):
                    recalled = engine.recall_test(r, secrets_map.get(r["id"], ""))
                    if recalled and (recalled.get("ok") or not (r.get("last_test") or {}).get("ok")):
                        r["last_test"] = recalled
                r["status"] = engine.status_of(r)
                cleaned.append(r)

            errs = engine.validate(cleaned, secrets_map)
            if errs:
                return self._json(400, {"ok": False, "code": "invalid", "errors": errs})

            # 清理被删路由的密钥
            live_ids = {r["id"] for r in cleaned}
            for rid in list(secrets_map):
                if rid not in live_ids:
                    secrets_map.pop(rid, None)

            store.save_secrets(secrets_map)
            store.save_routes(cleaned)
            res = engine.reload_gateway(cleaned, CFG, set(secrets_map))
            # 记一条变更：记的是「这次真写进网关文件的内容」，不是草稿
            try:
                payload = engine.build_gateway_payload(cleaned, set(secrets_map))
                items = payload.get("routes") or []
                api_items = [it for it in items if it.get("type") == "api"]
                store.append_change({
                    "ts": store.now_iso(),
                    "kind": "save",
                    "routes": len(cleaned),
                    "active": len(items),
                    "pool": [it.get("model") or it.get("name") for it in api_items],
                    "levels": {it.get("model"): it.get("level")
                               for it in api_items if it.get("model")},
                    "reload_ok": bool(res.get("ok")),
                    "path": res.get("path") or "",
                })
            except Exception:
                pass
            return self._json(200 if res["ok"] else 500, {
                "ok": True,
                "reload": res,
                "routes": view(cleaned, secrets_map),
            })

    def _find(self, rid):
        routes, secrets_map = routes_with_keys()
        for r in routes:
            if r["id"] == rid:
                return routes, secrets_map, r
        return routes, secrets_map, None

    def _test_draft(self):
        """草稿测试：不落盘、不改别人的状态，只给这一条盖章，供「导入即测」用。"""
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        raw = body.get("route") if isinstance(body.get("route"), dict) else body
        if not isinstance(raw, dict):
            return self._err(400, "缺少 route")
        r = {k: v for k, v in dict(raw).items() if not str(k).startswith("_")}
        for k in ("key_hint", "key_set", "status", "last_test", "health"):
            r.pop(k, None)
        key = str(body.get("api_key") or r.pop("api_key", "") or "").strip()
        r = store.normalize_route(r)
        if r["type"] not in ("webui", "api", "local"):
            return self._err(400, "类型必须是 webui / api / local")

        with STATE_LOCK:
            if not key and r["type"] == "api":
                key = store.load_secrets().get(r["id"], "")
            res = engine.test_route(r, key)
            engine.remember_test(r, key, res)
        r["last_test"] = res
        return self._json(200, {
            "ok": True,
            "test": res,
            "route": engine.public_route(r, bool(key)),
        })

    def _route_action(self, rid, action, sub):
        with STATE_LOCK:
            routes, secrets_map, route = self._find(rid)
            if not route:
                return self._err(404, "路由不存在（新增的路由请先保存）")

            if action == "test":
                if route.get("type") == "webui" and int(route.get("debug_port") or 0) <= 0:
                    route["debug_port"] = 9300 + (len(routes) % 50)
                res = engine.test_route(route, secrets_map.get(rid, ""))
                engine.remember_test(route, secrets_map.get(rid, ""), res)
                engine.apply_result(route, res, CFG)
                store.save_routes(routes)
                return self._json(200, {"ok": True, "route": engine.public_route(route, bool(secrets_map.get(rid)))})

            if action == "start":
                return self._start(route, routes, secrets_map)

            if action == "login":
                if route.get("type") != "webui":
                    return self._err(400, "只有 webui 型有登录流程")
                if not (route.get("profile_dir") or "").strip():
                    return self._err(400, "先选 profile 目录")
                port = int(route.get("debug_port") or 0) or (9300 + len(routes) % 50)
                route["debug_port"] = port
                if sub == "start":
                    ok, detail = chrome.launch_headful(route["profile_dir"], port, CFG.get("chrome_path", ""))
                    store.save_routes(routes)
                    return self._json(200 if ok else 500, {"ok": ok, "detail": detail})
                if sub == "finish":
                    res = chrome.verify_login(route["profile_dir"], CFG.get("chrome_path", ""),
                                              route.get("check_url", ""))
                    route["login_state"] = res["state"]
                    chrome.kill_port(port)
                    store.save_routes(routes)
                    return self._json(200, {"ok": True, "login": res})
                return self._err(400, "未知登录步骤")

            return self._err(404, "未知操作")

    def _start(self, route, routes, secrets_map):
        rid = route["id"]
        if route.get("type") == "webui":
            port = int(route.get("debug_port") or 0) or (9300 + len(routes) % 50)
            route["debug_port"] = port
            ok, detail = chrome.launch(route.get("profile_dir", ""), port,
                                       bool(route.get("headless", True)),
                                       CFG.get("chrome_path", ""))
            res = engine.test_route(route, "")
            engine.apply_result(route, res, CFG)
            if ok:
                st = chrome.read_login_state(route.get("profile_dir", ""))
                if st.get("state"):
                    route["login_state"] = st["state"]
            store.save_routes(routes)
            return self._json(200, {
                "ok": ok,
                "detail": detail,
                "route": engine.public_route(route, False),
            })

        # api / local：启动 = 预检一次，通了才算可挂
        res = engine.test_route(route, secrets_map.get(rid, ""))
        engine.remember_test(route, secrets_map.get(rid, ""), res)
        engine.apply_result(route, res, CFG)
        store.save_routes(routes)
        return self._json(200 if res["ok"] else 502, {
            "ok": res["ok"],
            "detail": res["detail"],
            "route": engine.public_route(route, bool(secrets_map.get(rid))),
        })

    # ---------- 设置 ----------

    def _save_settings(self):
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        if "chrome_path" in body:
            CFG["chrome_path"] = str(body.get("chrome_path") or "").strip()
        if "gateway_output" in body:
            p = str(body.get("gateway_output") or "").strip()
            if p and not os.path.isdir(os.path.dirname(p) or "."):
                return self._err(400, "网关表目标目录不存在")
            CFG["gateway_output"] = p or os.path.join(store.DATA_DIR, "gateway_routes.json")
        if isinstance(body.get("cooldown"), dict):
            cd = CFG.setdefault("cooldown", dict(store.DEFAULT_COOLDOWN))
            for k in ("fail_threshold", "base_seconds", "max_seconds"):
                if k in body["cooldown"]:
                    try:
                        cd[k] = max(1, int(body["cooldown"][k]))
                    except (TypeError, ValueError):
                        return self._err(400, "冷却参数必须是整数")
        store.save_config(CFG)
        return self._json(200, {"ok": True, "settings": {
            "chrome_path": CFG.get("chrome_path", ""),
            "gateway_output": CFG.get("gateway_output", ""),
            "cooldown": CFG.get("cooldown", store.DEFAULT_COOLDOWN),
        }})

    # ---------- 服务管理 ----------

    def _service_info(self):
        port = CFG.get("port")
        saved_pid = 0
        try:
            with open(PID_FILE, "r", encoding="ascii") as f:
                saved_pid = int((f.read() or "0").strip() or 0)
        except (OSError, ValueError):
            saved_pid = 0
        return self._json(200, {
            "ok": True,
            "running": True,
            "pid": os.getpid(),
            "pid_file": saved_pid,
            "port": port,
            "bind": f"{BIND_HOST}:{port}",
            "url": f"http://{BIND_HOST}:{port}/",
            "started_at": int(STARTED_AT),
            "uptime_seconds": int(time.time() - STARTED_AT),
            "python": sys.executable,
            "autostart": {"installed": autostart_state(), "task": AUTOSTART_TASK},
            "version": version(),
        })

    def _service_autostart(self):
        try:
            body = self._body()
        except ValueError as e:
            return self._err(400, str(e))
        want = bool(body.get("enabled"))
        action = "autostart-on" if want else "autostart-off"
        try:
            rc, out = run_service(action, timeout=30)
        except subprocess.TimeoutExpired:
            return self._err(504, "计划任务操作超时")
        info = {"installed": autostart_state(), "task": AUTOSTART_TASK}
        if rc != 0:
            return self._json(500, {"ok": False, "error": out or f"schtasks rc={rc}", "autostart": info})
        return self._json(200, {"ok": True, "detail": out, "autostart": info})

    def _service_logs(self):
        qs = parse_qs(urlparse(self.path).query)
        name = (qs.get("name") or ["err"])[0].strip().lower()
        path = LOG_FILES.get(name)
        if not path:
            return self._err(400, "未知日志名，只支持 err / out")
        try:
            tail = int((qs.get("tail") or ["300"])[0])
        except ValueError:
            tail = 300
        tail = max(1, min(tail, 2000))

        info = {"ok": True, "name": name, "path": path,
                "lines": [], "size": 0, "mtime": 0, "truncated": False}
        try:
            size = os.path.getsize(path)
            info["size"] = size
            info["mtime"] = int(os.path.getmtime(path))
            with open(path, "rb") as f:
                # 大文件只读尾部 512KB，避免把整个日志读进内存
                if size > 512 * 1024:
                    f.seek(size - 512 * 1024)
                    f.readline()
                raw = f.read()
            lines = decode_log(raw).splitlines()
            info["truncated"] = len(lines) > tail
            info["lines"] = lines[-tail:]
        except FileNotFoundError:
            info["missing"] = True
        except OSError as e:
            return self._err(500, "读取日志失败：" + str(e))
        return self._json(200, info)

    def _changelog(self):
        qs = parse_qs(urlparse(self.path).query)
        try:
            limit = int((qs.get("limit") or ["60"])[0])
        except ValueError:
            limit = 60
        limit = max(1, min(limit, 200))
        try:
            entries = store.load_changes(limit)
        except OSError as e:
            return self._err(500, "读取变更记录失败：" + str(e))
        return self._json(200, {
            "ok": True,
            "path": store.CHANGE_LOG_PATH,
            "limit": limit,
            "entries": list(reversed(entries)),   # 新在前
        })

    def _service_restart(self):
        # 先回包，再让一个脱离的 powershell 停掉本进程并重新拉起
        delayed(0.6, lambda: spawn_service("restart"))
        return self._json(200, {
            "ok": True,
            "restarting": True,
            "url": f"http://{BIND_HOST}:{CFG.get('port')}/",
        })

    def _service_exit(self):
        self._json(200, {"ok": True, "exiting": True})
        delayed(0.5, hard_exit)


# ---------------------------------------------------------------- 入口

def main():
    global CFG
    ap = argparse.ArgumentParser(description="网关路由管理页（仅本机回环）")
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--password", default="", help="首次运行时指定管理员密码")
    args = ap.parse_args()

    if BIND_HOST != "127.0.0.1":
        print("拒绝启动：管理页只允许绑定 127.0.0.1", file=sys.stderr)
        return 2
    if args.port == 8000:
        print("拒绝启动：8000 是正式网关端口，管理页不得占用", file=sys.stderr)
        return 2

    CFG, first_run = bootstrap(args.password or None, args.port)
    CFG["port"] = args.port

    httpd = ThreadingHTTPServer((BIND_HOST, args.port), Handler)
    httpd.daemon_threads = True
    url = f"http://{BIND_HOST}:{args.port}/"
    print(f"路由管理页已启动：{url}", flush=True)
    if not first_run:
        print(f"凭证文件：{store.FIRST_RUN_PATH}" if os.path.isfile(store.FIRST_RUN_PATH) else "使用既有管理员密码登录", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止", flush=True)
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
