"""开机自启 launcher：起 gateway.py + qw_bridge.py，日志写本目录文件。

由 pythonw.exe 启动（无窗口）；子进程通过文件句柄拿 stdout/stderr，
避免 pythonw 无控制台导致 print 崩溃。
"""
import os
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
os.chdir(BASE)

# launcher 自身也重定向，防意外
_launcher_log = open(os.path.join(BASE, "autorun.log"), "a", buffering=1, encoding="utf-8", errors="replace")
sys.stdout = _launcher_log
sys.stderr = _launcher_log


def _ts():
    import time as _t
    return _t.strftime("%Y-%m-%d %H:%M:%S")


def spawn(script, log_name, env_extra=None, wait_ready=None, timeout_s=30):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    log = open(os.path.join(BASE, log_name), "ab")
    print(f"[{_ts()}] spawn {script} env={env_extra}", flush=True)
    p = subprocess.Popen(
        [sys.executable, "-u", script],
        stdout=log,
        stderr=subprocess.STDOUT,
        cwd=BASE,
        env=env,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    print(f"[{_ts()}] {script} pid={p.pid}", flush=True)
    if wait_ready:
        import urllib.request
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(wait_ready, timeout=2) as r:
                    print(f"[{_ts()}] {script} ready: {r.read().decode('utf-8', 'replace')}", flush=True)
                    break
            except Exception:
                if p.poll() is not None:
                    print(f"[{_ts()}] {script} exited early code={p.returncode}", flush=True)
                    break
                time.sleep(1)
    return p


def main():
    spawn(
        "gateway.py",
        "gateway_autorun.log",
        env_extra={"JT_VLM_BACKEND": "wbapi"},
        wait_ready="http://127.0.0.1:8000/health",
        timeout_s=15,
    )
    spawn(
        "qw_bridge.py",
        "qw_bridge_autorun.log",
        wait_ready="http://127.0.0.1:8001/health",
        timeout_s=60,
    )
    print(f"[{_ts()}] launcher done", flush=True)


if __name__ == "__main__":
    main()
