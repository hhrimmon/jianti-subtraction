"""Chrome / Edge 探测、无头启动、有头登录窗口、登录状态存 profile 目录。

不使用 Playwright，只调浏览器自身命令行，零依赖。
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

from store import PROFILES_DIR, now_iso, read_json, write_json_atomic

_LOGIN_STATE_FILE = "_login_state.json"

_WIN_PATHS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]

_MAC_PATHS = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
]

_LINUX_PATHS = [
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "/snap/bin/chromium",
    "/usr/bin/microsoft-edge",
    "/usr/bin/microsoft-edge-stable",
]

# 只有 Chromium 系支持 CDP；Firefox 不在列表里，找不到时如实说明
_WHICH_NAMES = ("google-chrome", "google-chrome-stable", "chrome",
                "chromium", "chromium-browser", "msedge", "microsoft-edge")


def _kind_of(exe: str) -> str:
    name = os.path.basename(exe).lower()
    if "edge" in name:
        return "edge"
    if "chrom" in name:
        return "chromium"
    return "chromium"


def _common_paths():
    if os.name == "nt":
        return _WIN_PATHS
    if sys.platform == "darwin":
        home = os.path.expanduser("~")
        return _MAC_PATHS + [p.replace("/Applications/", home + "/Applications/", 1) for p in _MAC_PATHS]
    return _LINUX_PATHS


def _from_registry():
    """App Paths 注册表：覆盖非常规安装位置。"""
    try:
        import winreg
    except ImportError:
        return []
    found = []
    for exe in ("chrome.exe", "msedge.exe"):
        for root, path in (
            (winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{exe}"),
            (winreg.HKEY_CURRENT_USER, rf"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{exe}"),
        ):
            try:
                with winreg.OpenKey(root, path) as k:
                    val, _ = winreg.QueryValueEx(k, "")
                    if val and os.path.isfile(val):
                        found.append(val)
            except OSError:
                continue
    return found


def detect(override: str = ""):
    """返回 {path, source, tried, kind}。默认自动找，用户设置优先。"""
    if override and os.path.isfile(override):
        return {"path": override, "source": "设置覆盖", "tried": [], "kind": _kind_of(override)}

    tried = []
    for key, label in (("GW_CHROME_PATH", "环境变量 GW_CHROME_PATH"), ("CHROME_PATH", "环境变量 CHROME_PATH")):
        p = (os.environ.get(key) or "").strip()
        if not p:
            continue
        tried.append(p)
        if os.path.isfile(p):
            return {"path": p, "source": label, "tried": tried, "kind": _kind_of(p)}

    for p in _common_paths():
        tried.append(p)
        if os.path.isfile(p):
            return {"path": p, "source": "常见安装位置", "tried": tried, "kind": _kind_of(p)}

    if os.name == "nt":
        for key in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
            base = os.environ.get(key) or ""
            if not base:
                continue
            for rel in (r"Google\Chrome\Application\chrome.exe", r"Microsoft\Edge\Application\msedge.exe"):
                p = os.path.join(base, rel)
                tried.append(p)
                if os.path.isfile(p):
                    return {"path": p, "source": f"环境变量 {key}", "tried": tried, "kind": _kind_of(p)}

        for p in _from_registry():
            tried.append(p)
            return {"path": p, "source": "注册表 App Paths", "tried": tried, "kind": _kind_of(p)}

    for name in _WHICH_NAMES:
        p = shutil.which(name)
        if p:
            tried.append(p)
            return {"path": p, "source": "PATH", "tried": tried, "kind": _kind_of(p)}

    return {"path": "", "source": "未找到", "tried": tried, "kind": "",
            "hint": "未找到 Chromium 系浏览器（Chrome / Edge / Chromium）；Firefox 不支持 CDP，需手动填一次路径"}


def devtools_up(port: int, timeout: float = 1.0) -> bool:
    url = f"http://127.0.0.1:{int(port)}/json/version"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def port_busy(port: int) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{int(port)}/json/version", timeout=0.6):
            return True
    except urllib.error.URLError:
        return False
    except Exception:
        return True


def _spawn(args, cwd=None):
    flags = 0
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    return subprocess.Popen(args, cwd=cwd, creationflags=flags,
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def _wait_devtools(port: int, seconds: float = 14.0):
    end = time.time() + seconds
    while time.time() < end:
        if devtools_up(port, 0.8):
            return True
        time.sleep(0.4)
    return False


def launch(profile_dir: str, port: int, headless: bool, chrome_path: str):
    """拉起浏览器给网关用。返回 (ok, detail)。"""
    exe = (chrome_path or "").strip()
    if not exe or not os.path.isfile(exe):
        d = detect()
        exe = d["path"]
    if not exe:
        return False, "未找到 Chrome/Edge，请在设置里指定一次路径"
    if not profile_dir:
        return False, "未选 profile 目录"
    os.makedirs(profile_dir, exist_ok=True)

    if devtools_up(port):
        return True, f"已在运行（调试端口 {port}）"

    args = [
        exe,
        f"--user-data-dir={profile_dir}",
        f"--remote-debugging-port={int(port)}",
        "--remote-debugging-address=127.0.0.1",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-background-networking",
        "--disable-popup-blocking",
        "--disable-features=Translate,MediaRouter",
    ]
    if headless:
        args += ["--headless=new", "--disable-gpu", "--window-size=1280,900"]

    try:
        _spawn(args)
    except OSError as e:
        return False, f"启动失败：{e}"

    if _wait_devtools(port):
        return True, f"已启动（调试端口 {port}，{'无头' if headless else '有头'}）"
    return False, f"已拉进程但调试端口 {port} 未就绪，检查该端口是否被占用"


def launch_headful(profile_dir: str, port: int, chrome_path: str):
    """有头窗口给用户手动登录，同一个 profile 目录。"""
    return launch(profile_dir, port, headless=False, chrome_path=chrome_path)


def kill_port(port: int) -> bool:
    """只杀监听该调试端口的那个 pid，不误伤其它 Chrome。"""
    if os.name != "nt":
        # POSIX：lsof 精确定位监听该端口的 pid
        try:
            out = subprocess.run(["lsof", "-ti", f"tcp:{int(port)}"],
                                 capture_output=True, text=True, timeout=8).stdout
        except Exception:
            return False
        killed = False
        for pid in (out or "").split():
            try:
                os.kill(int(pid), signal.SIGTERM)
                killed = True
            except Exception:
                continue
        return killed
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "TCP"],
                             capture_output=True, text=True, timeout=8).stdout
    except Exception:
        return False
    pids = set()
    for line in out.splitlines():
        if f":{int(port)} " in line and "LISTENING" in line.upper():
            pid = line.split()[-1]
            if pid.isdigit():
                pids.add(pid)
    killed = False
    for pid in pids:
        try:
            subprocess.run(["taskkill", "/PID", pid, "/F"],
                           capture_output=True, timeout=8)
            killed = True
        except Exception:
            continue
    return killed


# ---------------------------------------------------------------- 登录状态

def login_state_path(profile_dir: str) -> str:
    return os.path.join(profile_dir, _LOGIN_STATE_FILE)


def read_login_state(profile_dir: str) -> dict:
    return read_json(login_state_path(profile_dir), {}) or {}


def write_login_state(profile_dir: str, state: str, method: str, detail: str):
    if not profile_dir:
        return
    os.makedirs(profile_dir, exist_ok=True)
    write_json_atomic(login_state_path(profile_dir), {
        "state": state,
        "method": method,
        "detail": detail,
        "at": now_iso(),
    })


def profile_has_cookies(profile_dir: str) -> bool:
    for rel in (os.path.join("Network", "Cookies"), "Cookies"):
        p = os.path.join(profile_dir, rel)
        if os.path.isfile(p) and os.path.getsize(p) > 0:
            return True
    return False


_LOGIN_MARKERS = ("sign in", "log in", "login", "登录", "sign-in", "password", "验证")


def verify_login(profile_dir: str, chrome_path: str, check_url: str):
    """登录完成后校验：用该 profile 无头抓一次 check_url 的 DOM，看是否还停在登录页。

    没配 check_url 时只如实记录「已保存、未验证」，不编造结论。
    """
    if not profile_dir or not os.path.isdir(profile_dir):
        return {"state": "unknown", "detail": "profile 目录不存在"}
    if not profile_has_cookies(profile_dir):
        return {"state": "expired", "detail": "profile 里没有 cookie，看起来没登录成功"}

    if not check_url:
        msg = "登录已保存（未配置校验 URL，未验证登录态）"
        write_login_state(profile_dir, "logged_in", "manual", msg)
        return {"state": "logged_in", "detail": msg}

    exe = chrome_path or detect()["path"]
    if not exe:
        msg = "登录已保存（未找到浏览器，跳过校验）"
        write_login_state(profile_dir, "logged_in", "manual", msg)
        return {"state": "logged_in", "detail": msg}

    args = [exe, f"--user-data-dir={profile_dir}", "--headless=new",
            "--disable-gpu", "--no-first-run", "--virtual-time-budget=9000",
            "--dump-dom", check_url]
    try:
        proc = subprocess.run(args, capture_output=True, timeout=45)
    except subprocess.TimeoutExpired:
        msg = "登录已保存（校验超时，未验证登录态）"
        write_login_state(profile_dir, "logged_in", "manual", msg)
        return {"state": "logged_in", "detail": msg}
    except Exception as e:
        msg = f"登录已保存（校验失败：{e}）"
        write_login_state(profile_dir, "logged_in", "manual", msg)
        return {"state": "logged_in", "detail": msg}

    dom = (proc.stdout or b"").decode("utf-8", "replace").lower()
    if not dom.strip():
        msg = "登录已保存（校验未取到页面，可能是 profile 被占用）"
        write_login_state(profile_dir, "logged_in", "manual", msg)
        return {"state": "logged_in", "detail": msg}

    if any(m in dom for m in _LOGIN_MARKERS) and len(dom) < 4000:
        msg = "校验显示仍停在登录页，可能已过期"
        write_login_state(profile_dir, "expired", "probe", msg)
        return {"state": "expired", "detail": msg}

    msg = f"校验通过：{check_url} 不再要求登录"
    write_login_state(profile_dir, "logged_in", "probe", msg)
    return {"state": "logged_in", "detail": msg}


# ---------------------------------------------------------------- profile 列表

def _user_data_roots():
    """各平台 Chrome / Edge / Chromium 的 User Data 根目录，带可读标签。"""
    home = os.path.expanduser("~")
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA") or ""
        if not local:
            return []
        return [
            (os.path.join(local, r"Google\Chrome\User Data"), "Chrome"),
            (os.path.join(local, r"Microsoft\Edge\User Data"), "Edge"),
        ]
    if sys.platform == "darwin":
        base = os.path.join(home, "Library", "Application Support")
        return [
            (os.path.join(base, "Google", "Chrome"), "Chrome"),
            (os.path.join(base, "Microsoft Edge"), "Edge"),
            (os.path.join(base, "Chromium"), "Chromium"),
        ]
    cfg = os.path.join(home, ".config")
    return [
        (os.path.join(cfg, "google-chrome"), "Chrome"),
        (os.path.join(cfg, "chromium"), "Chromium"),
        (os.path.join(cfg, "microsoft-edge"), "Edge"),
    ]


def list_profiles():
    """列出可直接复用的已登录 profile 目录。"""
    roots = [(p, label) for p, label in _user_data_roots()] + [(PROFILES_DIR, "本地")]
    roots = [(p, label) for p, label in roots if p]

    out = []
    seen = set()
    for root, tag in roots:
        if not os.path.isdir(root):
            continue
        try:
            names = sorted(os.listdir(root))
        except OSError:
            continue
        for name in names:
            full = os.path.join(root, name)
            if not os.path.isdir(full) or full in seen:
                continue
            if name in ("Guest Profile", "System Profile"):
                continue  # 不可复用的临时 profile
            if name != "Default" and not re.match(r"^Profile \d+$", name):
                if root != PROFILES_DIR:
                    continue
            seen.add(full)
            st = read_login_state(full)
            out.append({
                "path": full,
                "label": f"{tag} · {name}",
                "has_cookies": profile_has_cookies(full),
                "login_state": st.get("state", "unknown"),
                "login_at": st.get("at", ""),
            })
    return out
