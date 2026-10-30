# -*- coding: utf-8 -*-
import random  # 滑块拖拽轨迹抖动
"""千问网页版桥接服务（Playwright + 本机 Chrome + 本机独立浏览器 profile）

    GET  /health  -> {"ok":true,"bridge":"qw-web-6-twoseg","profile":"zone1","desktop":"headless|hidden|visible"}
    POST /solve   -> body 原始 JPEG，返回 {"ok":true,"text":...,"upload_ms":N,"answer_ms":N,"shots":[...]}

行为约定：
  1. Chrome 以 --headless=new 运行（无窗口，规避虚拟桌面遮挡导致的 1x1 视口塌缩）；
     每次导航后用 CDP Emulation.setDeviceMetricsOverride 把布局视口钉死 1280x900。
  2. 每次请求前检测登录态：掉线时若有窗口则移到主桌面等扫码（headless 下只能等恢复）。
  3. 答案截图按题分两段：第N题·思路 / 第N题·过程与答案；截图用克隆节点到临时
     白底容器做元素级截图，不修改原 DOM（改 DOM 会触发 React 重渲染使容器失稳）。
"""

import base64
import ctypes
import json
import os
import queue
import re
import tempfile
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import psutil
from playwright.sync_api import sync_playwright

PROFILE = os.environ.get("QW_PROFILE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "chrome_profile", "zone1"))
CHROME = os.environ.get("QW_CHROME", r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe")
HOME = os.environ.get("QW_HOME", "https://www.qianwen.com/")
# 提示词随机微扰 2.0（风控缓解）：句子级措辞池 + 中段约束句顺序随机打乱。
# 每次请求现场组装：首句池选 1 + 7 组约束各选 1 条并打乱次序 + 尾句池选 1，
# 降低「每次都是同一段提示词」的行为特征。
# 不变量（逐字保留，分段正则 Q_RE/G_PAT/_kind_of 依赖，不许改）：
#   【第N题·思路】/【第N题·过程与答案】两段结构、题号对应、思路不写答案、
#   括号小题独立成块、禁整卷总思路（拍的题按课做的，没有全卷可总结）、
#   第一行直接从第1题开始。
_P_OPEN = [
    "把图片里的每道题都解答。",
    "请把图片中的题目逐题解答，一道都不要漏。",
    "图片里有几道题就解几道题。",
    "按图片逐题给出解答。",
]
_P_FIRST = [
    "禁止写整卷总思路、开头总述、「本组题目考查什么」之类的总起段落，第一行就直接从第1题开始。",
    "不要整卷总思路，不要开头总述，也不要「本组题目考查什么」这类总起内容，输出第一行就是第1题。",
    "严禁整卷总思路、开头总述或「本组题目考查什么」式的开场，开头第一行必须是第1题。",
]
_P_STRUCT = [
    "严格按题分块，每道题只用两个小标题：【第N题·思路】和【第N题·过程与答案】，标题独占一行。",
    "每题固定用两个小标题分块：【第N题·思路】、【第N题·过程与答案】，标题各占一行。",
    "每道题分两块，小标题只用【第N题·思路】与【第N题·过程与答案】，标题单独占一行。",
]
_P_SUBQ = [
    "只要题目里出现 (1)(2)(3) 这类括号小题号，就必须把每道小题当作独立题目分块，"
    "标题写【第N题(k)·思路】和【第N题(k)·过程与答案】，k 为括号序号，不许多个小题合并在同一块。",
    "题目若带 (1)(2)(3) 式括号小题号，每个小题独立成块，"
    "标题为【第N题(k)·思路】与【第N题(k)·过程与答案】，k 即括号序号，绝不允许合块。",
    "遇到 (1)(2)(3) 这样的括号小题号时，必须把每个小题当成独立题目，"
    "标题用【第N题(k)·思路】与【第N题(k)·过程与答案】，k 取括号序号，禁止多个小题合并。",
]
_P_ORDER = [
    "每题先写思路，再写过程与答案，答案只写在过程与答案的最后，思路里不写答案；解完一题再解下一题。",
    "顺序是先思路、再过程与答案，答案落在过程末尾，思路部分不出现答案；一题解完再写下一题。",
    "写法上先思路后过程与答案，答案置于过程结尾，思路不写答案；逐题推进。",
]
_P_FILL = [
    "填空题和选择题：思路只写一句话；过程与答案里不写过程，直接给答案。",
    "填空、选择题：思路一句带过，过程与答案部分省略过程、直接写答案。",
    "填空题、选择题：思路一句话即可，过程与答案里直接写答案、不写过程。",
]
_P_BIG = [
    "大题：过程与答案里给完整计算过程，最后写答案。",
    "大题要在过程与答案中写全计算步骤，末尾落答案。",
    "大题：写完整计算过程再给出答案。",
]
_P_NUM = [
    "题号沿用图片原题号；没有题号的按从上到下、从左到右编为第1题、第2题……",
    "编号照抄图片原题号；原图没编号时按从上到下、从左到右记作第1题、第2题……",
    "题号跟图片里的一致；图上没题号就按从上到下、从左到右依次编为第1题、第2题……",
]
_P_END = [
    "禁止把全卷的思路或答案汇总成整体栏目；不要寒暄，不要解释规则，不要多余的话。",
    "不要把全卷思路或答案合并成整体栏目；不客套，不解释规则，不写多余内容。",
    "全卷思路或答案概览一概不要；不寒暄、不解释、不啰嗦。",
]
# 显式设置 QW_PROMPT 时只用环境变量这一套（调试/回归用）
PROMPT_ENV = os.environ.get("QW_PROMPT")


def pick_prompt():
    """每次请求现场组装提示词，返回 (提示词文本, 指纹)。"""
    if PROMPT_ENV:
        return PROMPT_ENV, "env"
    mid = [random.choice(g) for g in
           (_P_FIRST, _P_STRUCT, _P_SUBQ, _P_ORDER, _P_FILL, _P_BIG, _P_NUM)]
    random.shuffle(mid)
    p = random.choice(_P_OPEN) + "".join(mid) + random.choice(_P_END)
    return p, "%08x" % (zlib.crc32(p.encode("utf-8")) & 0xFFFFFFFF)


PORT = int(os.environ.get("QW_PORT", "8001"))
BRIDGE_VERSION = "qw-web-18-selfheal"
WAIT_ANSWER_S = 240
WAIT_LOGIN_S = int(os.environ.get("QW_WAIT_LOGIN", "300"))
HIDDEN_DESKTOP_NUMBER = int(os.environ.get("QW_DESKTOP", "2"))

CDP_PORT = int(os.environ.get("QW_CDP_PORT", "9222"))
CDP_URL = "http://127.0.0.1:%d" % CDP_PORT

# 账号组（风控切组）：滑块出现即停当前组、切下一组，冷却 30 分钟，勿连刷
ZONES = [z.strip() for z in os.environ.get("QW_ZONES", "zone1,zone2,zone3").split(",") if z.strip()]
PROFILE_ROOT = os.path.dirname(PROFILE.rstrip("\\/")) or PROFILE
ZONE_COOLDOWN_S = int(os.environ.get("QW_ZONE_COOLDOWN", "1800"))
# 未登录 / 启动失败：只短暂跳过，不按风控 30 分钟锁死
ZONE_UNLOGGED_COOLDOWN_S = int(os.environ.get("QW_ZONE_UNLOGGED_COOLDOWN", "300"))
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
ZONE_STATE = os.path.join(BASE_DIR, "zone_state.json")
INC_LOG = os.path.join(BASE_DIR, "bridge_incidents.jsonl")
CHROME_SPAWN_LOG = os.path.join(BASE_DIR, "chrome_spawn.log")


def _now_s():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def load_zone_state():
    """账号组冷却跨重启保留：以前只在内存里，重启即失效。"""
    try:
        # utf-8-sig：脚本/PS 写出来的文件常带 BOM，别让 BOM 把冷却状态吞掉
        with open(ZONE_STATE, "r", encoding="utf-8-sig") as f:
            raw = json.load(f)
        out = {}
        for z, until in raw.items():
            if isinstance(until, (int, float)) and until > time.time():
                out[z] = float(until)
        return out
    except Exception:
        return {}


def save_zone_state(blocked):
    try:
        alive = {z: t for z, t in blocked.items() if t > time.time()}
        with open(ZONE_STATE, "w", encoding="utf-8") as f:
            json.dump(alive, f, ensure_ascii=False, indent=2)
    except Exception as exc:  # noqa: BLE001
        print("[%s] 冷却状态落盘失败: %s" % (_now_s(), exc), flush=True)


def incident(kind, detail):
    """结构化故障记录：每次关键事件一行，便于事后查根因，不靠记忆。"""
    rec = {"ts": _now_s(), "kind": kind, "detail": detail}
    try:
        with open(INC_LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception:
        pass
    print("[%s] 事件 %s：%s" % (rec["ts"], kind, detail), flush=True)


USER32 = ctypes.windll.user32
CHROME_CLASS = "Chrome_WidgetWin_1"


# ---------------------------------------------------------------- 窗口 / 桌面
class DesktopCtl:
    """用 pyvda 把 Chrome 窗口在虚拟桌面之间搬运；headless 下没有窗口，全部空转。"""

    def __init__(self, desktop_number=HIDDEN_DESKTOP_NUMBER):
        self.ok = False
        self.reason = ""
        self.desktop_number = desktop_number
        self.main = None
        self.hidden = None
        try:
            import pyvda
            self.pyvda = pyvda
            desktops = pyvda.get_virtual_desktops()
            if len(desktops) < desktop_number:
                pyvda.VirtualDesktop.create()
                time.sleep(1.2)
                desktops = pyvda.get_virtual_desktops()
            if len(desktops) < desktop_number:
                raise RuntimeError("虚拟桌面数量不足（%d）" % len(desktops))
            self.main = desktops[0]
            self.hidden = desktops[desktop_number - 1]
            probe = pyvda.VirtualDesktop.current()
            if probe is None:
                raise RuntimeError("VirtualDesktop.current() 返回空")
            self.ok = True
        except Exception as exc:  # noqa: BLE001
            self.reason = "%s: %s" % (type(exc).__name__, exc)

    def _chrome_pids(self):
        pids = set()
        try:
            for child in psutil.Process(os.getpid()).children(recursive=True):
                if child.name().lower() == "chrome.exe":
                    pids.add(child.pid)
        except Exception:
            pass
        try:
            for p in psutil.process_iter(["pid", "name", "cmdline"]):
                try:
                    if p.info["name"] and p.info["name"].lower() == "chrome.exe":
                        cmd = " ".join(p.info["cmdline"] or [])
                        if PROFILE_ROOT.lower() in cmd.lower() or "--remote-debugging-port=%d" % CDP_PORT in cmd:
                            pids.add(p.info["pid"])
                except Exception:
                    continue
        except Exception:
            pass
        return pids

    def find_window(self, timeout=15):
        pids = self._chrome_pids()
        if not pids:
            return None
        found = []
        CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def cb(hwnd, _):
            if not USER32.IsWindowVisible(hwnd):
                return True
            pid = ctypes.c_ulong()
            USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value not in pids:
                return True
            cls = ctypes.create_unicode_buffer(64)
            USER32.GetClassNameW(hwnd, cls, 64)
            if cls.value != CHROME_CLASS:
                return True
            title = ctypes.create_unicode_buffer(256)
            USER32.GetWindowTextW(hwnd, title, 256)
            if title.value:
                found.append((hwnd, title.value))
            return True

        t_end = time.time() + timeout
        while time.time() < t_end:
            found.clear()
            USER32.EnumWindows(CB(cb), 0)
            if found:
                for hwnd, title in found:
                    if "千问" in title:
                        return hwnd
                return found[-1][0]
            time.sleep(0.5)
        return None

    def to_hidden(self, hwnd):
        if not self.ok or not hwnd:
            return False
        try:
            self.pyvda.AppView(hwnd).move(self.hidden)
            time.sleep(0.4)
            return self.pyvda.AppView(hwnd).desktop.number == self.hidden.number
        except Exception as exc:  # noqa: BLE001
            self.ok = False
            self.reason = "move_to_hidden: %s" % exc
            return False

    def to_main_front(self, hwnd):
        if not self.ok or not hwnd:
            return False
        try:
            app = self.pyvda.AppView(hwnd)
            app.move(self.main)
            time.sleep(0.4)
            app = self.pyvda.AppView(hwnd)
            app.switch_to()
            app.set_focus()
            USER32.SetForegroundWindow(hwnd)
            return True
        except Exception as exc:  # noqa: BLE001
            self.ok = False
            self.reason = "move_to_main: %s" % exc
            return False


# ---------------------------------------------------------------- 登录态判定
_CN_NUM = {"零": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}

def _cn_int(s):
    """'3'/'三'/'十二' → int；解析失败返回 0。"""
    s = (s or "").strip()
    if s.isdigit():
        return int(s)
    if s in _CN_NUM:
        return _CN_NUM[s]
    if "十" in s:
        a, _, b = s.partition("十")
        return (_CN_NUM.get(a, 1) if a else 1) * 10 + _CN_NUM.get(b, 0)
    return 0

def is_logged_in(page):
    if "passport" in page.url:
        return False
    try:
        txt = page.inner_text("body")
    except Exception:
        return False
    if "扫码登录" in txt or "二维码" in txt:
        return False
    if re.search(r"Qwen\d{3,}", txt):
        return True
    try:
        if page.query_selector('button:has-text("登录")') or page.query_selector('a:has-text("登录")'):
            return False
        return page.query_selector("[contenteditable='true']") is not None
    except Exception:
        return False


REFUSE_RE = re.compile(r"(无法回答|不能回答|无法提供|不便回答|换个话题|换个问题|抱歉[，,]?\s*我)")
_HEAD_Q = re.compile(r"第\s*[0-9一二三四五六七八九十]+\s*题")


def is_soft_refusal(text):
    """网页端账号级软拒答：短文本、无题块标题、含固定拒答说法。

    长答案里出现「无法提供」可能是正常讲解，不用这个判据。
    """
    t = (text or "").strip()
    if not t or len(t) > 160:
        return False
    if _HEAD_Q.search(t):
        return False
    return bool(REFUSE_RE.search(t))


class BrowserWorker(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.tasks = queue.Queue()
        self.pw = None
        self.browser = None
        self.ctx = None
        self.page = None
        self.hwnd = None
        self.own_browser = False
        self.desktop = DesktopCtl()
        self.mode = "hidden" if self.desktop.ok else "headless-fallback"
        self.zone_idx = 0
        self.zone_blocked = load_zone_state()  # zone -> 冷却截止 ts（落盘，重启不失效）
        for _z in [z.strip() for z in os.environ.get("QW_BLOCK_ZONES", "").split(",") if z.strip()]:
            self.zone_blocked[_z] = time.time() + ZONE_COOLDOWN_S
            incident("zone_block_from_env", {"zone": _z, "minutes": ZONE_COOLDOWN_S // 60})
        self.profile_used = ZONES[self.zone_idx]
        self.last_login_prompt = 0

    # ---- 对外 ----
    def solve(self, jpeg_bytes, tmp_path, timeout=330):
        fut = {"event": threading.Event(), "result": None, "error": None}
        self.tasks.put((jpeg_bytes, tmp_path, fut))
        if not fut["event"].wait(timeout):
            raise RuntimeError("桥接超时（%ss）" % timeout)
        if fut["error"]:
            raise RuntimeError(fut["error"])
        return fut["result"]

    def info(self):
        try:
            page_ok = bool(self.page and not self.page.is_closed())
            if page_ok:
                self.page.evaluate("() => 1")
        except Exception:
            page_ok = False
        return {
            "bridge": BRIDGE_VERSION,
            "profile": self.profile_used,
            "zones": ZONES,
            "zones_blocked": {z: int(t - time.time())
                              for z, t in self.zone_blocked.items() if t > time.time()},
            "browser": page_ok,
            "desktop": self.mode,
            "desktop_reason": self.desktop.reason or None,
            "headless": self.mode.startswith("headless"),
        }

    # ---- 账号组（风控切组） ----
    def _profile(self):
        return os.path.join(PROFILE_ROOT, ZONES[self.zone_idx])

    def _chrome_args(self):
        return [
            "--user-data-dir=" + self._profile(),
            "--remote-debugging-port=%d" % CDP_PORT,
            "--headless=new",
            "--window-size=1280,900",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-blink-features=AutomationControlled",
            "about:blank",
        ]

    def _kill_chrome(self):
        """杀当前账号组的 Chrome 整树（按 --user-data-dir 精确匹配，不误伤其他浏览器）。"""
        import subprocess
        prof = self._profile().lower()
        for p in psutil.process_iter(["pid", "name", "cmdline"]):
            try:
                name = (p.info["name"] or "").lower()
                cmd = " ".join(p.info["cmdline"] or []).lower()
                if "chrome" in name and prof in cmd and "--type=" not in cmd:
                    subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.info["pid"])],
                                   capture_output=True)
                    print("[%s] 已杀账号组 Chrome pid=%d" % (time.strftime("%H:%M:%S"), p.info["pid"]),
                          flush=True)
            except Exception:
                continue

    def _rotate_zone(self):
        """切到下一个未冷却账号组：杀当前 Chrome、新 profile 重拉、快速验登录。"""
        for step in range(1, len(ZONES) + 1):
            nxt = (self.zone_idx + step) % len(ZONES)
            z = ZONES[nxt]
            if time.time() < self.zone_blocked.get(z, 0):
                print("[%s] 账号组 %s 冷却中，跳过" % (time.strftime("%H:%M:%S"), z), flush=True)
                continue
            self._kill_chrome()
            self._reset_browser()
            self.zone_idx = nxt
            self.profile_used = z
            try:
                self._launch()
                self._pin_viewport()
                if is_logged_in(self.page):
                    print("[%s] 已切到账号组 %s（登录正常）" % (time.strftime("%H:%M:%S"), z), flush=True)
                    return True
                print("[%s] 账号组 %s 未登录千问，跳过" % (time.strftime("%H:%M:%S"), z), flush=True)
            except Exception as e:
                print("[%s] 账号组 %s 启动失败: %s" % (time.strftime("%H:%M:%S"), z, e), flush=True)
            # 未登录/启动失败只短暂跳过（5 分钟），别按风控的 30 分钟锁死，
            # 否则用户扫码登录后还得干等半小时。
            self.zone_blocked[z] = time.time() + ZONE_UNLOGGED_COOLDOWN_S
            save_zone_state(self.zone_blocked)
        self._kill_chrome()
        self._reset_browser()
        return False

    def _on_captcha(self, reason):
        """滑块出现即停当前账号组（冷却）并切组；无组可切则保持无浏览器状态。"""
        cur = ZONES[self.zone_idx]
        self.zone_blocked[cur] = time.time() + ZONE_COOLDOWN_S
        save_zone_state(self.zone_blocked)
        incident("captcha_stop_zone", {"zone": cur, "reason": reason,
                                       "cooldown_min": ZONE_COOLDOWN_S // 60})
        print("[%s] 风控事件（%s）：账号组 %s 冷却 %d 分钟" % (
            time.strftime("%H:%M:%S"), reason, cur, ZONE_COOLDOWN_S // 60), flush=True)
        if self._rotate_zone():
            print("[%s] 切组完成，当前账号组 %s" % (time.strftime("%H:%M:%S"), ZONES[self.zone_idx]),
                  flush=True)
        else:
            print("[%s] 无可用账号组（全部冷却或未登录），后续请求快速失败" % time.strftime("%H:%M:%S"),
                  flush=True)

    # ---- 浏览器生命周期 ----
    def _reset_browser(self):
        try:
            if self.own_browser and self.ctx:
                self.ctx.close()
        except Exception:
            pass
        self.browser = None
        self.ctx = None
        self.page = None
        self.hwnd = None

    def _clear_profile_locks(self):
        """删掉上次异常退出留下的单实例锁。

        Chrome 报 "Lock file can not be created! Error code: 5" 基本都是这个原因：
        上一个同 profile 的 Chrome 没退干净，锁文件还在，新进程建不了锁。
        """
        removed = []
        prof = self._profile()
        for name in ("SingletonLock", "SingletonCookie", "SingletonSocket", "lockfile"):
            p = os.path.join(prof, name)
            try:
                if os.path.exists(p) or os.path.islink(p):
                    os.remove(p)
                    removed.append(name)
            except Exception as exc:  # noqa: BLE001
                print("[%s] 删锁失败 %s: %s" % (_now_s(), p, exc), flush=True)
        return removed

    def _spawn_chrome(self):
        """手动 spawn Chrome（TCP CDP），避开 launch_persistent_context 的管道锁。

        顺序关键：先清掉仍占着同一 user-data-dir 的旧 Chrome → 再删残留 Singleton 锁
        → 最后才拉新 Chrome。反过来做就会撞上 SingletonLock 创建失败。
        """
        import subprocess
        self.own_browser = True
        self._kill_chrome()
        removed = self._clear_profile_locks()
        if removed:
            incident("chrome_lock_cleared", {"profile": self._profile(), "files": removed})
        try:
            log = open(CHROME_SPAWN_LOG, "ab")
            log.write(("\n=== %s spawn profile=%s\n" % (_now_s(), self._profile())).encode("utf-8"))
            log.flush()
            subprocess.Popen([CHROME] + self._chrome_args(), close_fds=True,
                             stdout=log, stderr=log)
        except Exception as exc:  # noqa: BLE001
            incident("chrome_spawn_error", {"error": str(exc)})
            return False
        t_start, t_end = time.time(), time.time() + 20
        while time.time() < t_end:
            try:
                import urllib.request
                with urllib.request.urlopen(CDP_URL + "/json/version", timeout=2):
                    incident("chrome_spawn_ok", {"profile": self._profile(),
                                                 "elapsed_s": round(time.time() - t_start, 1)})
                    return True
            except Exception:
                time.sleep(0.8)
        incident("chrome_spawn_timeout", {"profile": self._profile(), "log": CHROME_SPAWN_LOG})
        return False

    def _connect_cdp(self):
        self.browser = self.pw.chromium.connect_over_cdp(CDP_URL)
        self.ctx = self.browser.contexts[0]
        self.page = self.ctx.pages[0] if self.ctx.pages else self.ctx.new_page()
        self.page.goto(HOME, wait_until="domcontentloaded", timeout=45000)
        self.page.wait_for_timeout(2500)
        self.hwnd = self.desktop.find_window()
        if self.hwnd:
            self.desktop.to_hidden(self.hwnd)
            if not self.desktop.ok:
                self.mode = "visible"
        elif self.mode != "headless-fallback":
            self.mode = "headless"

    def _launch(self):
        if self.pw is None:
            self.pw = sync_playwright().start()
        # 优先 CDP：已有 Chrome 直接连；没有就自己 spawn 一个再连
        try:
            self._connect_cdp()
            return
        except Exception:
            pass
        try:
            if self._spawn_chrome():
                self._connect_cdp()
                return
        except Exception as exc:  # noqa: BLE001
            incident("cdp_connect_after_spawn_fail", {"error": str(exc)})
        # spawn 失败常见于上次残留锁 + 半启动的 Chrome，清一遍再来一次
        try:
            self._kill_chrome()
            self._clear_profile_locks()
            if self._spawn_chrome():
                self._connect_cdp()
                return
        except Exception as exc:  # noqa: BLE001
            incident("cdp_retry_failed", {"error": str(exc)})
        incident("launch_fallback_persistent", {"profile": self._profile()})
        # 兜底：launch_persistent_context（沙箱/管道锁场景，仅备用）
        headless = self.mode == "headless-fallback"
        self.own_browser = True
        self.ctx = self.pw.chromium.launch_persistent_context(
            self._profile(),
            headless=headless,
            executable_path=CHROME,
            args=[
                "--no-first-run",
                "--no-default-browser-check",
                "--disable-blink-features=AutomationControlled",
                "--start-maximized",
            ],
            viewport=None if headless else {"width": 1280, "height": 900},
        )
        self.page = self.ctx.pages[0] if self.ctx.pages else self.ctx.new_page()
        self.page.goto(HOME, wait_until="domcontentloaded", timeout=45000)
        self.page.wait_for_timeout(2500)
        if not headless:
            self.hwnd = self.desktop.find_window()
            if self.hwnd and not self.desktop.to_hidden(self.hwnd):
                self.mode = "headless-fallback"
                self._reset_browser()
                self._launch()

    def _pin_viewport(self):
        """把布局视口钉死为 1280x900。必须在每次导航后调用——

        对初始 about:blank 施加的仿真会在首次真实导航（进程切换）时被 Chrome 丢弃，
        之后页面布局视口会塌缩成 1x1，所有元素挤在左上角导致点击全部失败。
        """
        try:
            sess = self.ctx.new_cdp_session(self.page)
            sess.send("Emulation.setDeviceMetricsOverride",
                      {"width": 1280, "height": 900, "deviceScaleFactor": 1, "mobile": False})
        except Exception:
            pass

    def _ensure_page(self):
        if self.page is not None:
            try:
                self.page.goto(HOME, wait_until="domcontentloaded", timeout=45000)
                self.page.wait_for_timeout(1200)
                self._pin_viewport()
                return self.page
            except Exception:
                self._reset_browser()
        self._launch()
        self._pin_viewport()
        return self.page

    def _wait_login(self):
        """掉线时把窗口请回主桌面，等用户扫码；登录成功后回隐藏桌面。"""
        page = self.page
        if self.mode != "headless-fallback" and self.hwnd:
            self.desktop.to_main_front(self.hwnd)
            self.last_login_prompt = time.time()
            print("[%s] 千问登录态失效，已把窗口移到主桌面，等待扫码…" % time.strftime("%H:%M:%S"), flush=True)
        else:
            print("[%s] 千问登录态失效（headless 无窗口可弹），等待登录态恢复…" % time.strftime("%H:%M:%S"), flush=True)
        t_end = time.time() + WAIT_LOGIN_S
        while time.time() < t_end:
            page.wait_for_timeout(2000)
            try:
                page.goto(HOME, wait_until="domcontentloaded", timeout=30000)
                self._pin_viewport()
            except Exception:
                pass
            page.wait_for_timeout(1500)
            if is_logged_in(page):
                print("[%s] 检测到登录成功，恢复运行" % time.strftime("%H:%M:%S"), flush=True)
                if self.mode != "headless-fallback" and self.hwnd:
                    self.desktop.to_hidden(self.hwnd)
                return True
        return False

    # ---- 单次问答 ----
    def _ask(self, jpeg_path, t_start):
        """返回 (答案文本, 上传毫秒, 收到答案毫秒, 分段截图)。

        上传毫秒：请求开始 -> 图片发给千问网页版并确认发出；
        收到答案毫秒：确认发出 -> 答案流式输出稳定后取回。
        """
        # 全部账号组冷却中时快速失败（停号原因可能是滑块，也可能是账号级拒答；不连刷）
        now0 = time.time()
        cur_zone = ZONES[self.zone_idx]
        if now0 < self.zone_blocked.get(cur_zone, 0):
            eta = min(t for z, t in self.zone_blocked.items() if t > now0)
            blocked_names = "、".join(z for z, t in self.zone_blocked.items() if t > now0)
            raise RuntimeError("账号组冷却中（已停用 %s），约 %d 分钟后恢复，请稍后再拍"
                               % (blocked_names, max(1, int((eta - now0) / 60))))
        page = self._ensure_page()
        if not is_logged_in(page):
            if not self._wait_login():
                raise RuntimeError("千问掉线且等待扫码超时（%ss）" % WAIT_LOGIN_S)
            page = self._ensure_page()

        prompt, pfp = pick_prompt()
        print("[%s] 提示词指纹 %s len=%d" % (time.strftime("%H:%M:%S"), pfp, len(prompt)),
              flush=True)

        # 先移除可能遮挡的弹窗
        try:
            page.evaluate("() => { document.querySelectorAll('.baxia-dialog, .baxia-dialog-mask').forEach(el => el.remove()); }")
            page.wait_for_timeout(300)
        except Exception:
            pass
        try:
            page.get_by_text("新对话", exact=True).first.click(timeout=4000)
            page.wait_for_timeout(2500)  # 等页面完全初始化
            print("[%s] 新对话后 url=%s" % (time.strftime("%H:%M:%S"), page.url), flush=True)
        except Exception as e:
            print("[%s] 新对话点击失败: %s" % (time.strftime("%H:%M:%S"), e), flush=True)

        page.click('button[aria-label="添加附件"]', timeout=15000)
        page.wait_for_timeout(700)
        with page.expect_file_chooser(timeout=20000) as fc:
            page.get_by_text("上传图片", exact=True).click(timeout=8000)
        fc.value.set_files(jpeg_path)
        page.wait_for_timeout(3000)  # 等图片上传完成
        t_up = time.time()
        while time.time() - t_up < 60:
            page.wait_for_timeout(500)
            btn = page.query_selector('button[aria-label="发送消息"]')
            if btn and btn.is_enabled():
                break

        ed = page.query_selector("[contenteditable='true']")
        if ed is None:
            raise RuntimeError("找不到输入框")
        ed.click()
        page.keyboard.type(prompt, delay=8)
        page.wait_for_timeout(400)

        # 发送并确认：跳 /chat/ 路由或输入框被清空才算真发出，否则重试
        sent = False
        t_sent = time.time()
        for attempt in range(3):
            t_b = time.time()
            while time.time() - t_b < 30:
                btn = page.query_selector('button[aria-label="发送消息"]')
                if btn and btn.is_enabled():
                    break
                page.wait_for_timeout(500)
            try:
                page.click('button[aria-label="发送消息"]', timeout=8000)
            except Exception:
                pass
            t_v = time.time()
            while time.time() - t_v < 8:
                page.wait_for_timeout(800)
                try:
                    if "/chat/" in page.url:
                        sent = True
                        break
                    ed2 = page.query_selector("[contenteditable='true']")
                    if ed2 is not None and not ed2.inner_text().strip():
                        sent = True
                        break
                except Exception:
                    pass
            if sent:
                t_sent = time.time()
                break
            print("[%s] 发送未确认，重试 %d/3" % (time.strftime("%H:%M:%S"), attempt + 2), flush=True)
        if not sent:
            raise RuntimeError("发送消息未成功（重试 3 次均未确认）")
        upload_ms = int((t_sent - t_start) * 1000)

        # 等答案：以最后一个 markdown 块的内容增长为信号。
        # 整页 inner_text 在流式输出时可能不变（虚拟化渲染），不能当完成判据。
        # 完成判据：内容稳定 8s 且 题块完整（每个【·思路】都有配对的【·过程与答案】、
        # 最后一个标题不是思路）；长输出中途停顿不再被 6s 稳定窗误判为答完。
        last, stable, text = "", 0, ""
        slide_fails = 0   # 连续拖拽未过次数
        slide_events = 0  # 滑块事件总数（含只点重试）
        t0 = time.time()
        while time.time() - t0 < WAIT_ANSWER_S:
            page.wait_for_timeout(1000)
            cur = ""
            for blk in reversed(page.query_selector_all("[class*='markdown']")):
                try:
                    cur = blk.inner_text().strip()
                except Exception:
                    continue
                if cur:
                    break
            if cur.startswith(prompt[:10]):
                cur = ""  # 用户消息气泡，不是答案
            if cur != last:
                stable = 0
                last = cur
            else:
                stable += 1
            # 风控滑块快速失败纪律（不拖满 240s、不复活死请求、滑块即停号切组）：
            #   拖过 → 该对话已被拦死不补答案，立即失败，请重拍（新对话/新账号组）；
            #   连拖 3 次未过 → 立即失败；滑块事件累计 5 次未决 → 立即失败。
            now_ts = time.time()
            if now_ts - self._last_slide >= 8:
                st = self._try_slide_captcha(page)
                if st != "none":
                    self._last_slide = now_ts
                    slide_events += 1
                    print("[%s] 滑块事件 %d（结果=%s 连拖未过=%d）"
                          % (time.strftime("%H:%M:%S"), slide_events, st, slide_fails), flush=True)
                    if st == "passed":
                        self._on_captcha("滑块已拖过但对话拦死")
                        raise RuntimeError("千问风控滑块拦答：滑块虽已拖过，但该对话不补答案，"
                                           "按死请求处理，请重拍（已停当前账号组并切组）")
                    if st == "failed":
                        slide_fails += 1
                    # 一次拖不过就停号切组，不再硬拖三次（越拖风控越死）
                    if slide_fails >= 1:
                        self._on_captcha("滑块拖拽未过，立即停号切组")
                        raise RuntimeError("千问风控滑块拦截：拖拽 1 次未通过即停手，未出答案"
                                           "（行为风控，非题目问题）。已停当前账号组并切组，"
                                           "下一位拍题会走下一组")
                    if slide_events >= 2:
                        self._on_captcha("滑块事件累计 2 次未决")
                        raise RuntimeError("千问风控滑块反复出现，未出答案。已停当前账号组并切组")
            if cur and stable >= 8:
                heads = [ln.strip() for ln in cur.splitlines() if self.Q_RE.search(ln)]
                kinds = [self._kind_of(h) for h in heads]
                n_think = sum(1 for k in kinds if k == "思路")
                n_ans = sum(1 for k in kinds if k == "过程与答案")
                # 有题只出思路没出过程、或最后一个标题是思路 → 还没答完，继续等
                incomplete = n_think > n_ans or (len(kinds) > 0 and kinds[-1] == "思路")
                # 检查是否包含多题关键词，防止模型只答第一题就停
                has_more = any(k in cur for k in ("第6题", "第7题", "拔高", "考点拔高"))
                # 题数对账：模型首行报【共N题】。底数题号数或含小题的完整题号数任一达到 N 即算齐
                m_tot = re.search(r"共\s*([0-9一二三四五六七八九十]+)\s*题", cur)
                if m_tot:
                    base_q, full_q = set(), set()
                    for h in heads:
                        mh = self.Q_RE.search(h)
                        if mh:
                            base_q.add(mh.group(1))
                            full_q.add("%s(%s)" % (mh.group(1), mh.group(2)) if mh.group(2) else mh.group(1))
                    if max(len(base_q), len(full_q)) < _cn_int(m_tot.group(1)):
                        has_more = True
                if not incomplete and not has_more:
                    text = cur
                    break
                if stable >= 20:  # 硬上限：稳定 20s 仍不完整也收，避免干等
                    text = cur
                    break
        else:
            raise RuntimeError("等回答超时（%ss）" % WAIT_ANSWER_S)
        if not text:
            raise RuntimeError("已出结果但提取为空")
        if is_soft_refusal(text):
            cur = ZONES[self.zone_idx]
            self.zone_blocked[cur] = time.time() + ZONE_COOLDOWN_S
            save_zone_state(self.zone_blocked)
            incident("soft_refusal_stop_zone", {"zone": cur, "snippet": text[:80]})
            raise RuntimeError("网页端拒答（账号级软风控）：已停用账号组 %s 并冷却 %d 分钟，"
                               "未自动切组，等人工决定后再用"
                               % (cur, ZONE_COOLDOWN_S // 60))
        n_heads = len([ln for ln in text.splitlines() if self.Q_RE.search(ln)])
        shots = self._take_shots(page)
        answer_ms = int((time.time() - t_sent) * 1000)
        print("[%s] 三数：AI 题块标题=%d，切出 shot=%d 张" % (time.strftime("%H:%M:%S"), n_heads, len(shots)), flush=True)
        return text, upload_ms, answer_ms, shots

    # ---- 原图分段截图：每题两段（思路 / 过程与答案），克隆节点做元素级截图 ----
    G_PAT = re.compile(r"^(解题思路|思路|核心解题步骤|解题步骤|解题过程|过程与答案|过程|标准答案|参考答案|答案)$")
    Q_RE = re.compile(r"第\s*([0-9一二三四五六七八九十]+)\s*题\s*(?:[（(]\s*([0-9]+)\s*[）)])?")
    _last_slide: float = 0.0

    def _try_slide_captcha(self, page) -> str:
        """千问风控滑块。返回 "none"（无验证码）/ "attempt"（只点了重试）/ "passed" / "failed"。
        失败时记 dist、起止坐标、原因，供风控诊断。8 秒节流由调用方控制。"""
        sels_retry = ["text=请点我重试", "[class*='retry']", "[class*='refresh']"]
        sels_handle = ["#nc_1_n1z", "[id*='nc_'][class*='btn']", "[class*='nc_iconfont']"]
        sels_track = ["[class*='nc-scale']", "[class*='scale'][class*='slide']", "[id*='nc_'][class*='scale']"]

        def find(sels):
            for f in [page] + list(page.frames):
                for s in sels:
                    try:
                        el = f.query_selector(s)
                        if el and el.is_visible():
                            return el
                    except Exception:
                        continue
            return None

        handle = find(sels_handle)
        if handle is None:
            retry = find(sels_retry)
            if retry is not None:
                try:
                    retry.click()
                except Exception:
                    pass
                print("[%s] 滑块未现 handle，只点了重试" % time.strftime("%H:%M:%S"), flush=True)
                return "attempt"
            return "none"
        x0 = y0 = dist = None
        try:
            retry = find(sels_retry)
            if retry is not None:
                try:
                    retry.click()
                    page.wait_for_timeout(2000)
                except Exception:
                    pass
                # 点重试后 nc 组件整体重载，原 handle 已过期，必须重找
                handle = find(sels_handle)
                if handle is None:
                    print("[%s] 点重试后 handle 未复现" % time.strftime("%H:%M:%S"), flush=True)
                    return "attempt"
            hb = handle.bounding_box()
            track = find(sels_track)
            if track is not None:
                tb = track.bounding_box()
                dist = tb["width"] - hb["width"] - 6
            else:
                dist = 320
            x0, y0 = hb["x"] + hb["width"] / 2, hb["y"] + hb["height"] / 2
            print("[%s] 滑块定位 handle=%s dist=%.0f from=(%.0f,%.0f) to=(%.0f,%.0f)"
                  % (time.strftime("%H:%M:%S"), hb, dist, x0, y0, x0 + dist, y0), flush=True)
            page.mouse.move(x0, y0)
            page.mouse.down()
            page.wait_for_timeout(150)
            moved = 0.0
            while moved < dist:
                remain = dist - moved
                step = max(3, min(26, remain * 0.18)) * (0.8 + random.random() * 0.4)
                moved += step
                page.mouse.move(x0 + moved, y0 + random.uniform(-2.5, 2.5))
                page.wait_for_timeout(25 + int(random.random() * 50))
            # 末端停顿即松手（与手动一次过的 cdp_drag 轨迹一致；过冲回正会被判机器行为）
            page.wait_for_timeout(200)
            page.mouse.up()
            print("[%s] 检测到滑块验证码，已自动拖拽" % time.strftime("%H:%M:%S"), flush=True)
            page.wait_for_timeout(2500)
            # 拖后校验：滑块仍在 → 本次失败，记 dist/坐标/原因供诊断
            still = find(sels_handle)
            if still is not None:
                try:
                    dbg = page.inner_text("body")[:120].replace("\n", " ")
                    # 页面文本可能含 GBK 控制台打不出的字符，先降级防诊断日志自身炸掉
                    dbg = dbg.encode("gbk", "replace").decode("gbk", "replace")
                except Exception:
                    dbg = "?"
                print("[%s] 滑块拖拽未通过 dist=%.0f from=(%.0f,%.0f) to=(%.0f,%.0f) 原因=拖后滑块仍在 body[%s]"
                      % (time.strftime("%H:%M:%S"), dist, x0, y0, x0 + dist, y0, dbg), flush=True)
                return "failed"
            print("[%s] 滑块已通过 dist=%.0f from=(%.0f,%.0f)" % (time.strftime("%H:%M:%S"), dist, x0, y0),
                  flush=True)
            return "passed"
        except Exception as e:
            print("[%s] 滑块拖拽异常 dist=%s from=(%s,%s) 原因=%s"
                  % (time.strftime("%H:%M:%S"),
                     "%.0f" % dist if dist is not None else "?",
                     "%.0f" % x0 if x0 is not None else "?",
                     "%.0f" % y0 if y0 is not None else "?", e), flush=True)
            return "failed"

    # （原 _shot_segment 为 @staticmethod，见文件后部）
    BG_RE = re.compile(r"拔高\s*([0-9一二三四五六七八九十]+)")

    @staticmethod
    def _kind_of(s):
        """只分两段：思路 / 过程与答案。"""
        if "思路" in s:
            return "思路"
        if "过程" in s or "步骤" in s or "解答" in s or "答案" in s:
            return "过程与答案"
        return None

    def _segment(self, items):
        """items: [(child_index, text)] → [{"qid", "kind", "idxs"}]，按题号/栏目标题切。

        只认两种段：思路、过程与答案。同题同种段重复出现时并入最近一段；
        无题号内容并入当前段；答案标题不单独成段（属于过程与答案范畴）。
        """
        segments = []
        section_kind = None
        cur = None
        auto_q = 0  # 无题号兜底编号
        # 预扫：是否存在带栏目词（思路/过程/答案…）的题号标题。
        # 有（标记模式）→ 整卷段里的"第N题：考查…"是摘要，并入整卷；
        # 没有（裸标题模式）→"第N题"一律是真题头，段种按思路处理（单块可见不锁）。
        marker_mode = False
        for _, t in items:
            h0 = t.split("\n", 1)[0].strip().strip("【】#*　 ") if t else ""
            if h0 and self.Q_RE.search(h0) and self._kind_of(h0):
                marker_mode = True
                break
        last_main_qid = None  # 最近大题号（第N题，不带括号）：裸 (k) 小题头继承，否则 (1)(2)(3) 全堆一卡
        sub_head_re = re.compile(r"^[（(]\s*(\d{1,2})\s*[）)]")
        for idx, t in items:
            head = t.split("\n", 1)[0].strip().strip("【】#*　 ") if t else ""
            qid = None
            sub_bare = False
            if head:
                m = self.Q_RE.search(head)
                if m:
                    qid = "第%s题" % m.group(1)
                    last_main_qid = qid
                    if m.group(2):
                        qid = "%s(%s)" % (qid, m.group(2))
                else:
                    m = self.BG_RE.search(head)
                    if m:
                        qid = "拔高%s" % m.group(1)
                    else:
                        ms = sub_head_re.match(head)
                        if ms and last_main_qid:
                            qid = "%s(%s)" % (last_main_qid, ms.group(1))
                            sub_bare = True
            if qid:
                kind = self._kind_of(head)
                if sub_bare and kind is None:
                    # 裸 (k) 无栏目词：继承当前段栏目（思路段里的 (k) 是思路，过程段里的是过程）
                    kind = (cur["kind"] if cur is not None else None) or section_kind or "思路"
                # 整卷总思路里的"第N题：考查…"摘要行不成题段：无栏目词、当前在整卷段、且全文为标记模式 → 并入整卷
                if kind is None and cur is not None and cur["qid"] is None and marker_mode:
                    cur["idxs"].append(idx)
                    continue
                if kind is None and not marker_mode:
                    kind = "思路"
                kind = kind or section_kind or "过程与答案"
                # 同题同种段已存在（如模型把答案单列成段）→ 并入最近一段
                target = None
                for prev in reversed(segments):
                    if prev["qid"] == qid and prev["kind"] == kind:
                        target = prev
                        break
                if target is not None:
                    target["idxs"].append(idx)
                    cur = target
                    continue
                cur = {"qid": qid, "kind": kind, "idxs": [idx]}
                segments.append(cur)
                continue
            if head and self.G_PAT.match(head):
                kind = self._kind_of(head)
                section_kind = kind
                # 无题号栏目标题（如"标准答案"）不新建整卷段，并入当前题段
                if cur is not None:
                    cur["idxs"].append(idx)
                    continue
                cur = {"qid": None, "kind": kind, "idxs": [idx]}
                segments.append(cur)
                continue
            if cur is None:
                cur = {"qid": None, "kind": section_kind or "思路", "idxs": [idx]}
                segments.append(cur)
            else:
                cur["idxs"].append(idx)
        segs = [s for s in merged_auto(segments, auto_q) if s["idxs"]]
        # 同一大题已有括号小题块（第N题(k)）时，丢弃题级重复块：模型常在小题答完后又按题级总结一遍
        has_sub = set()
        for s in segs:
            m = re.match(r"^第(.+)题\(\d+\)$", s["qid"] or "")
            if m:
                has_sub.add(m.group(1))
        out = []
        for seg in segs:
            m2 = re.match(r"^第(.+)题$", seg["qid"] or "")
            if m2 and m2.group(1) in has_sub:
                continue
            label = "%s·%s" % (seg["qid"], seg["kind"]) if seg["qid"] else "整卷·%s" % seg["kind"]
            out.append({"label": label, "idxs": seg["idxs"]})
        # 保底：第一个题号标记之前的前置段（整卷总思路/总述）直接去掉——不成卡、
        # 不计题数、不显示，只留证据日志；全丢光时兜底保留原列表
        dropped = [s["label"] for s in out if s["label"].startswith("整卷·")]
        kept = [s for s in out if not s["label"].startswith("整卷·")]
        if dropped:
            print("[%s] 前置段已丢弃（不成卡只留证据）: %s" % (time.strftime("%H:%M:%S"), ", ".join(dropped)), flush=True)
        if kept:
            out = kept
        return out

    def _take_shots(self, page):
        """按题把回答分两段截图。返回 [{"label", "png_b64"}]；失败时整段一张。"""
        blocks = page.query_selector_all("[class*='markdown']")
        if not blocks:
            return []
        container = blocks[-1]
        try:
            holder = container
            kids = holder.query_selector_all(":scope > *")
            depth = 0
            while len(kids) <= 1 and kids and depth < 3:
                nxt = kids[0].query_selector_all(":scope > *")
                if not nxt:
                    break
                holder = kids[0]
                kids = nxt
                depth += 1
            if not kids:
                return []
            items = []
            for i, el in enumerate(kids):
                try:
                    t = el.inner_text().strip()
                except Exception:
                    t = ""
                items.append((i, t))
            shots = []
            for seg in [s for s in self._segment(items) if s["idxs"]][:16]:
                png = self._shot_segment(page, holder, seg["idxs"])
                if png:
                    shots.append({"label": seg["label"], "png_b64": png})
            if not shots:
                data = container.screenshot(type="png", timeout=15000)
                shots = [{"label": "答案", "png_b64": base64.b64encode(data).decode("ascii")}]
            return shots
        except Exception:
            return []

    @staticmethod
    def _shot_segment(page, holder, idxs):
        """把本段子节点克隆进临时白底容器（zoom 1.5 放大），截整页 clip 后删除。

        不修改原 DOM（隐藏兄弟节点会触发 React 重渲染，容器被替换导致截图失败）；
        临时容器盖在页面最上层，截图里不会有输入框、工具栏、侧边栏；
        用 page.screenshot(clip) 而不是 element.screenshot，避免 element 稳定等待超时。
        """
        wrap = None
        try:
            wrap = holder.evaluate_handle(
                "(h, keep) => {"
                " const w = document.createElement('div');"
                " w.style.cssText = 'position:absolute;left:0;top:0;z-index:2147483647;"
                "background:#ffffff;padding:10px 12px;box-sizing:border-box;"
                "width:426px;zoom:1.5;';"
                " const ks = h.children;"
                " keep.forEach(i => { if (ks[i]) w.appendChild(ks[i].cloneNode(true)); });"
                " document.body.appendChild(w);"
                " return w; }", idxs)
            box = wrap.as_element().bounding_box()
            if not box:
                return ""
            data = page.screenshot(clip=box, type="png", timeout=15000)
            return base64.b64encode(data).decode("ascii") if data else ""
        except Exception:
            return ""
        finally:
            try:
                if wrap is not None:
                    wrap.evaluate("w => w.remove()")
            except Exception:
                pass

    def run(self):
        while True:
            item = self.tasks.get()
            if item is None:
                return
            jpeg_bytes, tmp_path, fut = item
            try:
                with open(tmp_path, "wb") as f:
                    f.write(jpeg_bytes)
                t0 = time.time()
                text, upload_ms, answer_ms, shots = self._ask(tmp_path, t0)
                fut["result"] = {"text": text, "latency_ms": int((time.time() - t0) * 1000),
                                 "upload_ms": upload_ms, "answer_ms": answer_ms,
                                 "shots": shots}
            except Exception as exc:  # noqa: BLE001
                self._reset_browser()
                fut["error"] = "千问桥接失败：%s" % exc
            finally:
                try:
                    os.remove(tmp_path)
                except Exception:
                    pass
                fut["event"].set()


def merged_auto(segments, auto_q):
    """无题号段沿用当前顺序；题号缺失的整卷段保持原样（不二次编号）。"""
    return segments


# ---------------------------------------------------------------- HTTP
WORKER = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[%s] %s" % (time.strftime("%H:%M:%S"), fmt % args), flush=True)

    def _send(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/health"):
            out = {"ok": True}
            out.update(WORKER.info())
            self._send(200, out)
        else:
            self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if not self.path.startswith("/solve"):
            self._send(404, {"ok": False, "error": "not found"})
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            jpeg = self.rfile.read(n)
            if not jpeg:
                self._send(400, {"ok": False, "error": "empty body"})
                return
            tmp = os.path.join(tempfile.gettempdir(), "qw_up_%d.jpg" % int(time.time() * 1000))
            result = WORKER.solve(jpeg, tmp)
            out = {"ok": True}
            out.update(result)
            self._send(200, out)
        except Exception as exc:  # noqa: BLE001
            import traceback
            traceback.print_exc()
            self._send(500, {"ok": False, "error": str(exc)})


def _already_running(port):
    import socket
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1.5)
        s.close()
        return True
    except OSError:
        return False


def main():
    global WORKER
    if _already_running(PORT):
        # 单实例：端口已被占用说明已有桥接在跑，再起一个只会抢同一个端口、请求随机落旧进程。
        print("端口 %d 上已有桥接在跑，本实例退出" % PORT, flush=True)
        return
    WORKER = BrowserWorker()
    WORKER.start()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("千问桥接已启动：http://127.0.0.1:%d/solve profile=%s mode=%s"
          % (PORT, WORKER.profile_used, WORKER.mode), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
