"""服务看门狗：常驻，管 8000 网关与 8001 桥接的存活；挂了就拉起来。

为什么要它
----------
1) 之前只有网关有计划任务（JianTiGatewayManual），桥接完全没有守护：进程被外部终止
   （无崩溃事件、无 traceback）后一直缺位，只能手工拉。夜里就是这么丢的。
2) 手工拉的进程挂在临时控制台上，控制台/作业对象一关就整树被回收（stub 无声消失同因）。
   所以这里一律用 pythonw + DETACHED_PROCESS + CREATE_NO_WINDOW 起子进程，脱离控制台。

怎么跑
------
计划任务 JianTiWatchdog 每 2 分钟跑一次本脚本；已在跑就直接退出（单实例锁）。
真正干活的是常驻循环：每 60s 探一次 /health，连续 2 次探不到就重启对应服务。
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

BASE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(BASE, "watchdog.log")
LOCK = os.path.join(BASE, "watchdog.lock")

PYW = os.environ.get("JT_PYTHONW") or os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
DETACHED_PROCESS = 0x00000008
CREATE_NO_WINDOW = 0x08000000

GATEWAY = "http://127.0.0.1:8000/health"
BRIDGE = "http://127.0.0.1:8001/health"
CHECK_EVERY_S = 60
DOWN_ROUNDS_TO_RESTART = 2


def log(msg):
    line = "[%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass
    print(line, flush=True)


def _pid_alive(pid):
    try:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"],
                             capture_output=True, text=True, timeout=10)
        return str(pid) in (out.stdout or "")
    except Exception:
        return False


def acquire_single_instance():
    try:
        if os.path.exists(LOCK):
            with open(LOCK, "r", encoding="utf-8") as f:
                old = int((f.read() or "0").strip() or 0)
            if old and old != os.getpid() and _pid_alive(old):
                return False
    except Exception:
        pass
    try:
        with open(LOCK, "w", encoding="utf-8") as f:
            f.write(str(os.getpid()))
    except Exception:
        pass
    return True


def probe(url, timeout=6):
    """返回 (http_ok, body_dict|None)。"""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
        try:
            return True, json.loads(body)
        except Exception:
            return True, {"raw": body[:200]}
    except Exception:
        return False, None


def spawn(script, log_name, env_extra=None):
    env = os.environ.copy()
    env.pop("JT_SOLVE_FAKE", None)
    if env_extra:
        env.update(env_extra)
    try:
        f = open(os.path.join(BASE, log_name), "ab")
        p = subprocess.Popen(
            [PYW, "-u", script],
            cwd=BASE, env=env,
            stdout=f, stderr=subprocess.STDOUT,
            creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
            close_fds=True,
        )
        log("spawn %s pid=%d" % (script, p.pid))
        return p.pid
    except Exception as exc:  # noqa: BLE001
        log("spawn %s 失败: %s" % (script, exc))
        return 0


def port_listening(port, host="127.0.0.1", timeout=1.5):
    """端口是否仍在监听。用来区分「进程忙」和「进程真没了」。"""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.close()
        return True
    except OSError:
        return False


def main():
    if not acquire_single_instance():
        return
    log("看门狗启动 pid=%d" % os.getpid())
    down = {"gateway": 0, "bridge": 0}
    while True:
        ok_g, body_g = probe(GATEWAY)
        if not ok_g or not (body_g or {}).get("ok"):
            down["gateway"] += 1
            log("网关探活失败（第 %d 次）" % down["gateway"])
            if down["gateway"] >= DOWN_ROUNDS_TO_RESTART:
                if port_listening(8000):
                    # 端口还在监听说明进程活着，只是忙（解题中 /health 会排队）。
                    # 这时候再拉一个会造成多实例抢同一个端口、请求随机落旧进程，所以不拉。
                    log("8000 仍在监听，判定为忙而非宕机，不重复拉起")
                else:
                    spawn("gateway.py", "gateway_watchdog.log", {"JT_VLM_BACKEND": "wbapi"})
                down["gateway"] = 0
                time.sleep(12)
                continue
        else:
            down["gateway"] = 0

        ok_b, body_b = probe(BRIDGE)
        if not ok_b or not (body_b or {}).get("ok"):
            down["bridge"] += 1
            log("桥接探活失败（第 %d 次）" % down["bridge"])
            if down["bridge"] >= DOWN_ROUNDS_TO_RESTART:
                if port_listening(8001):
                    log("8001 仍在监听，判定为忙而非宕机，不重复拉起")
                else:
                    spawn("qw_bridge.py", "qw_bridge_watchdog.log")
                down["bridge"] = 0
                time.sleep(25)
                continue
        else:
            down["bridge"] = 0
            if body_b.get("browser") is False:
                # 浏览器是按需拉起的（没请求时本来就是 false），这里只记录不重启，
                # 免得看门狗把健康的桥接反复掐死。Chrome 自愈在桥接内部 _launch 完成。
                log("桥接在跑，浏览器未拉起（按需延迟，正常）")
        time.sleep(CHECK_EVERY_S)


if __name__ == "__main__":
    main()
