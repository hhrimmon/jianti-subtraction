"""gateway.py — 接入网关 8000：平台唯一入口（路由 / 鉴权 / 限流，v7）.

测试模式（v6 Nyx 498/500；v7 Nyx 515 加演示账号）：JIANTI_TEST_MODE=1
才开，默认关；开时格式合法的 16 位设备号按已绑定处理、无真 Key 的
/solve 走模拟模型、演示账号那套凭证走通注册／登录全链路（发码成功、
注册幂等、登录未建号现场建号、管理密码放行，仅限演示账号）；关时
绑定 403、model_not_connected 503、演示账号发码 503／登录 401／
重复注册 409 与 v6 一字不差，见 TEST_MODE。

只做路由、鉴权、限流；业务逻辑在 cards / applications / binding /
users / families / judge / records / modelconfig 各模块里，网关不自己算业务。
标准库 only：http.server.ThreadingHTTPServer。

相对探针骨架的改动（手册 v2 §1–§5，接口文档）：
- /solve 改为异步：POST 建 job 后立即返回 202，判题在后台线程跑；
  终端按接口 A2 轮询 GET /solve/{job_id}（1 秒一次、最多 60 秒、
  超时 50401 由客户端判定；响应里带 poll 提示）。五阶段名按已定口径
  uploading／gateway／model／nlp／delivering。
- 幂等键：/solve、/cards/deduct、/cards/advance、POST /applications、
  approve、reject 要求 Idempotency-Key 头；同一键重复提交只执行一次，
  重复返回第一次的结果（缺键 400 idempotency_key_missing）。
  v2：幂等预检与执行业务同在 store.lock 内原子完成（判题线程也在锁内起），
  同键并发只执行一次，不重复建 job／扣卡／建单／批卡（见 TESTLOG.md §6）。
- 管理密码门禁：管理类接口（改卡规则、审批、改服务器配置、选模型、
  家庭组管理、解绑、用量／记录、待拉取通知）要求该会话已通过一次
  6 位管理密码校验（进管理输一次，管理内不再重复输）；未校验 403
  admin_password_required。解绑另要求角色为管理员。
- 服务器配置与模型选择按家庭组分（主键家庭组 ID），不再全局单份。
- 新增端点：GET /applications（待批列表）、GET /terminals（已绑定列表）、
  GET／POST /cards/rules、POST /auth/send-code（短信 stub，503）、
  /auth/register|login|admin-password/verify|reset|passwords、
  家庭组接口（GET /family、POST /family/invite、成员通过／拒绝／转让／删除）、
  GET /config/environment、POST /config/environment/memo、
  POST /solve/{job_id}/cancel（取消未决任务）、POST /solve/{job_id}/skip
  （跳过记录）、POST /models/mcp/login（C3 别名）。
- 错误体仍为骨架口径 {"error": {"code": ..., "message": ...}}；
  接口文档的数字码信封（code/message/data）与本口径冲突时以骨架为准
  （接口文档自述“骨架复核时如冲突以骨架为准”，见 ASSUMPTIONS.md V1-A14）。
- 日志只记方法／路径／状态，不记请求体（请求体可能含密码）。

鉴权：除 /health、/lockscreen、/config/environment、
/users/register、/users/login、/auth/register、/auth/login、
/auth/send-code、/auth/admin-password/reset 外，
一律要 Authorization: Bearer <注册/登录返回的 token>。
/solve 另要 X-Terminal-Id，且该终端须已绑定到 token 所属家庭组。
终端 token 发放方式未定（U22）：v1 暂用家庭账号 token（见 ASSUMPTIONS.md）。

限流：每 IP 每分钟通用 120 次；每终端每分钟 /solve 10 次；
超了 429（骨架自定占位，见 ASSUMPTIONS.md V1-A12），429 响应带
Retry-After: 5 头（v3，Nyx 裁决 RECHECK #10）。
启动扫尾（v3，RECHECK #15）：启动时扫一遍 jobs，processing 且
created_at 超 2×POLL_TIMEOUT（120 秒）的标 failed，failure_reason
process_interrupted，并补一条终态 usage 记录。
keep-alive 修复（v5，Nyx 房间消息 id 329 定法②）：HTTP/1.1 默认
keep-alive，同一连接上鉴权 401／403 在 _read_json() 之前抛错时，
请求体还躺在 socket 缓冲里，下一个请求会从残留 body 处解析而 400。
v5 在 _dispatch 的异常出口、发错误响应前按 Content-Length 把未读
body 排空（_drain_body）；_read_json() 与 /solve 的 rfile.read 各记
已读字节数，已读完的不再排。/solve 图片 body 只在业务里读，入口
不预读、不进内存。
"""
from __future__ import annotations

import json
import os
import re
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import applications
import binding
import cards
import families
import judge
import modelconfig
import records as records_mod
import users
import weather_memo
from models import new_job, new_record, public_job
from push import pending_pushes
from server_config import (GATEWAY_PORT, JPEG_MAGIC, MAX_UPLOAD_BYTES,
                           MEMO_MAX_LENGTH, OFFICIAL_SERVER_HOST,
                           OFFICIAL_SERVER_URL, POLL_INTERVAL_SECONDS,
                           POLL_TIMEOUT_CODE, POLL_TIMEOUT_SECONDS,
                           RATE_LIMIT_GENERAL_PER_MIN,
                           RATE_LIMIT_SOLVE_PER_MIN,
                           RATE_LIMIT_WINDOW_SECONDS)
from store import SnapshotCorruptError, Store

VERSION = "9.7.0"
SERVICE_NAME = "jianti-platform-server-v9"

# 测试模式（v6，Nyx 房间消息 id 498/500；v7 加演示账号，Nyx id 515）：
# 显式开关，默认关。JIANTI_TEST_MODE=1 才开；开时：格式合法的 16 位
# 设备号（含演示值 2864190752386419 与 App 生成的 deviceId）按已绑定
# 处理、不再 403，无真 Key 的 /solve 走模拟模型，演示账号那套凭证
# 走通注册／登录全链路（仅限演示账号，见 users.py）；关时一切行为
# 与 v6 一字不差。开关只影响测试行为，绝不默认开、绝不绕过生产校验。
TEST_MODE = os.environ.get("JIANTI_TEST_MODE", "").strip().lower() in (
    "1", "true", "yes", "on")

# 限流数值可由环境变量覆盖（仅用于测试；默认值见 server_config.py）。
RATE_LIMIT_GENERAL = int(os.environ.get(
    "JIANTI_RATE_LIMIT_GENERAL_PER_MIN", str(RATE_LIMIT_GENERAL_PER_MIN)))
RATE_LIMIT_SOLVE = int(os.environ.get(
    "JIANTI_RATE_LIMIT_SOLVE_PER_MIN", str(RATE_LIMIT_SOLVE_PER_MIN)))

store = None  # 由 run() 创建（快照损坏时 run() 直接退出，不建网关）
_rate_lock = threading.Lock()
_rate_buckets: dict[str, list[float]] = {}


def _rate_ok(key: str, limit: int) -> bool:
    now = time.time()
    with _rate_lock:
        bucket = [t for t in _rate_buckets.get(key, [])
                  if now - t < RATE_LIMIT_WINDOW_SECONDS]
        if len(bucket) >= limit:
            _rate_buckets[key] = bucket
            return False
        bucket.append(now)
        _rate_buckets[key] = bucket
        return True


class ApiError(Exception):
    def __init__(self, code: str, message: str, status: int = 400):
        super().__init__(message)
        self.code, self.message, self.status = code, message, status


def _err_from(exc) -> ApiError:
    if isinstance(exc, ApiError):
        return exc
    for cls in (cards.CardError, applications.ApplicationError,
                binding.BindingError, users.UserError,
                modelconfig.ModelConfigError):
        if isinstance(exc, cls):
            return ApiError(exc.code, exc.message, exc.status)
    if isinstance(exc, OSError):
        return ApiError("internal_error",
                        "服务端内部错误：快照写入失败。", 500)
    return ApiError("internal_error", f"服务端内部错误：{exc}", 500)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # v5：本请求已从 rfile 读走的 body 字节数（keep-alive 排空用）。
    # 同一连接上同一 Handler 实例连续处理多个请求，_dispatch 入口清零。
    _body_read = 0

    # -- 工具 --
    def log_message(self, *args):  # 安静：不刷日志到 stdout
        pass

    def _send(self, status: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # v3（RECHECK #10）：429 限流响应带 Retry-After: 5，终端等 5 秒
        # 再由用户手动重试一次；其它状态码不带这个头。
        if status == 429:
            self.send_header("Retry-After", "5")
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, exc: Exception) -> None:
        e = _err_from(exc)
        if e.status >= 500:
            print(f"[gateway] {e.code}: {e.message}", file=sys.stderr, flush=True)
        self._send(e.status, {"error": {"code": e.code, "message": e.message}})

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        self._body_read += len(raw)  # v5：记已读，供异常出口排空时扣减
        try:
            obj = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise ApiError("invalid_json", "请求体不是合法 JSON。")
        if not isinstance(obj, dict):
            raise ApiError("invalid_json", "请求体须为 JSON 对象。")
        return obj

    def _drain_body(self) -> None:
        """v5（Nyx 329 定法②）：排空本请求未读的 body。

        HTTP/1.1 keep-alive 下，鉴权 401／403 等错误在 _read_json()
        之前抛出时 body 还在 socket 里；不排空就发错误响应，同一连接
        的下一个请求会从残留 body 处开始解析而 400。只在 _dispatch
        的异常出口调用；body 已读完时剩余为 0，不多读。尽力而为：
        读的过程中出错（对端已断开、Content-Length 非法等）直接忽略，
        不影响错误响应本身发出。
        """
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            return
        remaining = length - self._body_read
        while remaining > 0:
            try:
                chunk = self.rfile.read(min(65536, remaining))
            except (OSError, ValueError):
                return
            if not chunk:
                return
            self._body_read += len(chunk)
            remaining -= len(chunk)

    def _bearer_token(self) -> str | None:
        auth = self.headers.get("Authorization") or ""
        m = re.match(r"^Bearer\s+(\S+)$", auth)
        return m.group(1) if m else None

    def _auth_user(self) -> tuple[dict, dict, str]:
        """Bearer token -> (user, session, token)；失败 401。"""
        token = self._bearer_token()
        if not token:
            raise ApiError("unauthorized", "缺少 Authorization: Bearer token。", 401)
        sess = store.data["sessions"].get(token)
        user = store.data["users"].get(sess["user_id"]) if sess else None
        if user is None:
            raise ApiError("unauthorized", "token 无效或已失效。", 401)
        return user, sess, token

    def _require_admin_session(self) -> tuple[dict, dict, str]:
        """管理类接口：该会话须已通过一次 6 位管理密码校验。"""
        user, sess, token = self._auth_user()
        if not sess.get("admin_verified"):
            raise ApiError("admin_password_required",
                           "请先输一次六位管理密码再进管理。", 403)
        return user, sess, token

    def _require_admin_role(self) -> tuple[dict, dict, str]:
        user, sess, token = self._require_admin_session()
        if user.get("role") != "管理员":
            raise ApiError("forbidden", "只有管理员能做这个操作。", 403)
        return user, sess, token

    def _rate(self, key: str, limit: int) -> None:
        if not _rate_ok(key, limit):
            raise ApiError("rate_limited", "请求过于频繁，请稍后再试。", 429)

    # -- 幂等键 --
    def _idem_key(self, family_id: str, endpoint: str) -> str:
        key = self.headers.get("Idempotency-Key") or ""
        if not key.strip():
            raise ApiError("idempotency_key_missing",
                           "缺少 Idempotency-Key 请求头。")
        return f"{family_id}|{endpoint}|{key.strip()}"

    def _idem_lookup(self, ikey: str):
        return store.data["idempotency"].get(ikey)

    def _idem_store(self, ikey: str, status: int, body: dict) -> None:
        with store.lock:
            store.data["idempotency"][ikey] = {"status": status, "body": body}
            store.save()

    # -- 路由 --
    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_DELETE(self):
        self._dispatch("DELETE")

    def _dispatch(self, method: str):
        self._body_read = 0  # v5：新请求开始，已读 body 字节数清零
        try:
            parsed = urlparse(self.path)
            path = parsed.path.rstrip("/") or "/"
            query = parse_qs(parsed.query)
            # 通用限流（按 IP；/solve 另按终端限流）
            self._rate(f"ip:{self.client_address[0]}", RATE_LIMIT_GENERAL)
            if method == "GET" and path == "/health":
                return self._send(200, {
                    "ok": True, "service": SERVICE_NAME, "version": VERSION,
                    "gateway_port": GATEWAY_PORT,
                    "model_connected": judge.is_connected(),
                    "test_mode": TEST_MODE,
                    "snapshot": "ok",
                    "official_server": OFFICIAL_SERVER_HOST})
            if method == "GET" and path == "/lockscreen":
                # PRD v5.7 锁屏模块：真实温度 + MEMO（weather_memo 后台小模块）。
                # 公开口不鉴权；失败一律空值，不阻塞锁屏；device_id 仅作设备标识。
                # A 合并（NYX 528/529 定稿）：城市只按本次连接源 IP 反查，
                # 不取 X-Forwarded-For（客户端可伪造），不取终端上报的 IP。
                device_id = (query.get("device_id", [""])[0] or "").strip()
                return self._send(200, weather_memo.lockscreen_payload(
                    self.client_address[0], device_id))
            if method == "GET" and path == "/config/environment":
                return self._send(200, records_mod.environment_info(store))
            if method == "GET" and path == "/models":
                user, _, _ = self._require_admin_session()
                return self._send(200, modelconfig.get_models_view(
                    store, user["family_id"], judge.is_connected()))
            if method == "GET" and path == "/usage":
                user, _, _ = self._require_admin_session()
                days = self._days_param(query)
                summary = records_mod.usage_summary(
                    store, user["family_id"], days=days)
                # 方案设计 v2 §5：/usage 沿用现口径继续带回温度/MEMO，
                # 数据源改 weather_memo（真实温度按来源 IP 城市；MEMO 随机一句）。
                # A 合并：同 /lockscreen，只用连接源 IP，不取 X-Forwarded-For。
                summary.update(weather_memo.usage_style(self.client_address[0]))
                return self._send(200, summary)
            if method == "GET" and path == "/records":
                user, _, _ = self._require_admin_session()
                days = self._days_param(query, required=False)
                return self._send(200, records_mod.list_records(
                    store, user["family_id"], days=days))
            if method == "GET" and path == "/config/server":
                user = self._auth_user()[0]
                return self._send(200, self._server_config_view(user["family_id"]))
            if method == "GET" and path == "/push/pending":
                user, _, _ = self._require_admin_session()
                return self._send(200, {"pending": pending_pushes(
                    store, user["family_id"])})
            if method == "GET" and path == "/terminals":
                user, _, _ = self._require_admin_session()
                return self._send(200, {"items": binding.list_terminals(
                    store, user["family_id"])})
            if method == "GET" and path == "/applications":
                user, _, _ = self._require_admin_session()
                status = query.get("status", ["pending"])[0]
                if status not in ("pending", "approved", "rejected", "withdrawn"):
                    raise ApiError("invalid_status", "status 非法。")
                page = int(query.get("page", ["1"])[0])
                page_size = int(query.get("page_size", ["20"])[0])
                return self._send(200, applications.list_applications(
                    store, user["family_id"], status=status,
                    page=page, page_size=page_size))
            if method == "GET" and path == "/cards/rules":
                user = self._auth_user()[0]
                return self._send(200, cards.check_cards(store, user["family_id"]))
            if method == "GET" and path == "/family":
                user, _, _ = self._require_admin_session()
                return self._send(200, families.get_family(store, user["family_id"]))
            m = re.match(r"^/solve/([\w-]+)$", path)
            if method == "GET" and m:
                return self._get_solve(m.group(1))
            if method == "POST" and path == "/solve":
                return self._post_solve()
            m = re.match(r"^/solve/([\w-]+)/cancel$", path)
            if method == "POST" and m:
                return self._cancel_solve(m.group(1))
            m = re.match(r"^/solve/([\w-]+)/skip$", path)
            if method == "POST" and m:
                return self._skip_question(m.group(1))
            if method == "POST" and path == "/users/register":
                b = self._read_json()
                out = users.register(
                    store, b.get("phone"), b.get("login_password"),
                    b.get("admin_password"), b.get("sms_code"),
                    invite_code=b.get("invite_code"), test_mode=TEST_MODE)
                self._warm_city_cache()
                return self._send(201, out)
            if method == "POST" and path == "/auth/register":
                b = self._read_json()
                out = users.register(
                    store, b.get("phone"), b.get("password"),
                    b.get("admin_password"), b.get("code"),
                    invite_code=b.get("invite_code"), test_mode=TEST_MODE)
                self._warm_city_cache()
                return self._send(201, out)
            if method == "POST" and path == "/auth/send-code":
                b = self._read_json()
                return self._send(200, self._send_code_stub(b))
            if method == "POST" and path == "/users/login":
                b = self._read_json()
                out = users.login(
                    store, b.get("phone"), b.get("login_password"),
                    test_mode=TEST_MODE)
                self._login_auto_bind(out)
                self._warm_city_cache()
                return self._send(200, out)
            if method == "POST" and path == "/auth/login":
                b = self._read_json()
                out = users.login(
                    store, b.get("phone"), b.get("password"),
                    test_mode=TEST_MODE)
                self._login_auto_bind(out)
                self._warm_city_cache()
                return self._send(200, out)
            if method == "POST" and path in ("/users/admin-password/verify",
                                             "/auth/admin-password/verify"):
                user, _, token = self._auth_user()
                b = self._read_json()
                return self._send(200, users.verify_admin_password(
                    store, user["id"], b.get("admin_password"), token=token,
                    test_mode=TEST_MODE))
            if method == "POST" and path == "/auth/admin-password/reset":
                b = self._read_json()
                return self._send(200, users.reset_admin_password(
                    store, b.get("phone"), b.get("new_admin_password"),
                    b.get("code")))
            if method == "POST" and path == "/auth/passwords":
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                return self._send(200, self._change_passwords(user, b))
            if method == "POST" and path == "/cards/check":
                user = self._auth_user()[0]
                b = self._read_json()
                return self._send(200, cards.check_cards(
                    store, user["family_id"],
                    question_id=b.get("question_id"),
                    batch_id=b.get("batch_id")))
            if method == "POST" and path == "/cards/deduct":
                user = self._auth_user()[0]
                ikey = self._idem_key(user["family_id"], "deduct")
                b = self._read_json()
                # v2：幂等预检→扣卡→写记录同锁内完成（store.lock 是 RLock，
                # 业务函数内部再加锁不死锁）；请求体在锁外读完。
                with store.lock:
                    hit = self._idem_lookup(ikey)
                    if hit is None:
                        out = cards.deduct_card(
                            store, user["family_id"], job_id=b.get("job_id"),
                            batch_id=b.get("batch_id"),
                            question_index=b.get("question_index"))
                        self._idem_store(ikey, 200, out)
                        hit = {"status": 200, "body": out}
                return self._send(hit["status"], hit["body"])
            if method == "POST" and path == "/cards/advance":
                user = self._auth_user()[0]
                ikey = self._idem_key(user["family_id"], "advance")
                # v2：幂等预检→预支→写记录同锁内完成（同上）。
                with store.lock:
                    hit = self._idem_lookup(ikey)
                    if hit is None:
                        out = cards.advance_card(store, user["family_id"])
                        self._idem_store(ikey, 200, out)
                        hit = {"status": 200, "body": out}
                return self._send(hit["status"], hit["body"])
            if method == "POST" and path in ("/cards/config", "/cards/rules"):
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                daily = b.get("daily_cards", b.get("daily_total"))
                ratio = b.get("advance_ratio")
                return self._send(200, cards.set_card_rules(
                    store, user["family_id"],
                    daily_cards=daily, advance_ratio=ratio))
            if method == "POST" and path == "/applications":
                user = self._auth_user()[0]
                ikey = self._idem_key(user["family_id"], "applications")
                b = self._read_json()
                self._check_terminal_header_matches(b.get("terminal_id"))
                # v2：幂等预检→建单→写记录同锁内完成（同上）。
                with store.lock:
                    hit = self._idem_lookup(ikey)
                    if hit is None:
                        app = applications.create_application(
                            store, user["family_id"],
                            terminal_id=b.get("terminal_id"),
                            requested=b.get("requested") or {},
                            idempotency_key=self.headers.get("Idempotency-Key"),
                            question_id=b.get("question_id"))
                        self._idem_store(ikey, 201, app)
                        hit = {"status": 201, "body": app}
                return self._send(hit["status"], hit["body"])
            m = re.match(r"^/applications/([\w-]+)/approve$", path)
            if method == "POST" and m:
                user, _, _ = self._require_admin_session()
                ikey = self._idem_key(user["family_id"], f"approve|{m.group(1)}")
                b = self._read_json()
                # v2：归属校验＋幂等预检→审批→写记录同锁内完成（同上）。
                with store.lock:
                    self._check_app_family(m.group(1), user["family_id"])
                    hit = self._idem_lookup(ikey)
                    if hit is None:
                        out = applications.approve_application(
                            store, m.group(1),
                            b.get("grant_type"),
                            b.get("value", b.get("grant_value")),
                            approver_member_no=user.get("member_no"))
                        self._idem_store(ikey, 200, out)
                        hit = {"status": 200, "body": out}
                return self._send(hit["status"], hit["body"])
            m = re.match(r"^/applications/([\w-]+)/reject$", path)
            if method == "POST" and m:
                user, _, _ = self._require_admin_session()
                ikey = self._idem_key(user["family_id"], f"reject|{m.group(1)}")
                # v2：归属校验＋幂等预检→不批→写记录同锁内完成（同上）。
                with store.lock:
                    self._check_app_family(m.group(1), user["family_id"])
                    hit = self._idem_lookup(ikey)
                    if hit is None:
                        out = applications.reject_application(store, m.group(1))
                        self._idem_store(ikey, 200, out)
                        hit = {"status": 200, "body": out}
                return self._send(hit["status"], hit["body"])
            m = re.match(r"^/applications/([\w-]+)/withdraw$", path)
            if method == "POST" and m:
                user = self._auth_user()[0]
                self._check_app_family(m.group(1), user["family_id"])
                return self._send(200, applications.withdraw_application(
                    store, m.group(1)))
            if method == "POST" and path == "/terminals/bind":
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                term = binding.bind_terminal(
                    store, (b.get("device_id") or "").replace(" ", ""),
                    user["family_id"], bound_by=user["id"], method="manual",
                    member_no=b.get("member_no"))
                return self._send(201, term)
            if method == "POST" and path == "/terminals/bind-scan":
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                # v4（Nyx 284①）：scan_payload 或 qr_payload 都认——
                # 文档 B5 要 qr_payload、App 发 qr_payload，v3 只读
                # scan_payload 会把 App 发来的载荷当空串 422。
                # 两者都空仍传 ""，parse_scan_payload 照旧 422。
                term = binding.bind_scan(
                    store, b.get("scan_payload") or b.get("qr_payload") or "",
                    user["family_id"],
                    bound_by=user["id"], member_no=b.get("member_no"))
                return self._send(201, term)
            m = re.match(r"^/terminals/(\d{16})$", path)
            if method == "DELETE" and m:
                user, _, _ = self._require_admin_role()
                b = store.data["bindings"].get(m.group(1))
                if b is None or b["family_id"] != user["family_id"]:
                    raise ApiError("terminal_not_found", "该终端未绑定。", 404)
                out = binding.unbind_terminal(store, m.group(1))
                return self._send(200, out)
            if method == "POST" and path == "/models/select":
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                return self._send(200, modelconfig.select_model(
                    store, user["family_id"], b.get("model_id"),
                    b.get("line", "official")))
            if method == "POST" and path in ("/models/mcp-login",
                                             "/models/mcp/login"):
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                return self._send(200, modelconfig.mcp_login(
                    store, user["family_id"], b.get("site_url"),
                    b.get("account"), b.get("password"),
                    captcha_code=b.get("captcha_code")))
            if method == "POST" and path == "/models/my-model":
                self._require_admin_session()
                modelconfig.my_model_rejected()
            if method == "POST" and path == "/config/server":
                user, _, _ = self._require_admin_session()
                b = self._read_json()
                return self._send(200, self._set_server_config(
                    user["family_id"], b))
            if method == "POST" and path == "/config/environment/memo":
                # PRD v5.7 作废：MEMO 改答案之书随机下发，家长不可改（方案设计 v2 §5）。
                # 显式 410，明确告知作废原因，不再走家长改 MEMO 流程。
                self._require_admin_session()
                return self._send(410, {"error": {"code": "memo_deprecated",
                    "message": "PRD v5.7：MEMO 改为服务端答案之书句子集随机下发，"
                               "家长不可改，本接口已作废。"}})
            if method == "POST" and path == "/family/invite":
                user, _, _ = self._require_admin_session()
                return self._send(200, families.create_invite(store, user))
            m = re.match(r"^/family/members/(\d+)/approve$", path)
            if method == "POST" and m:
                user, _, _ = self._require_admin_role()
                return self._send(200, families.approve_member(
                    store, user, int(m.group(1))))
            m = re.match(r"^/family/members/(\d+)/reject$", path)
            if method == "POST" and m:
                user, _, _ = self._require_admin_role()
                return self._send(200, families.reject_member(
                    store, user, int(m.group(1))))
            if method == "POST" and path == "/family/transfer":
                user, _, _ = self._require_admin_role()
                b = self._read_json()
                return self._send(200, families.transfer_admin(
                    store, user, b.get("to_member_no")))
            m = re.match(r"^/family/members/(\d+)$", path)
            if method == "DELETE" and m:
                user, _, _ = self._require_admin_role()
                return self._send(200, families.delete_member(
                    store, user, int(m.group(1))))
            raise ApiError("not_found", f"没有这个接口：{method} {path}", 404)
        except Exception as exc:  # noqa: BLE001 — 统一转错误体
            # v5：先排空未读 body 再发错误响应，否则 keep-alive 下
            # 同一连接的下一个请求会从残留 body 处解析而 400。
            self._drain_body()
            try:
                self._send_error(exc)
            except (BrokenPipeError, ConnectionResetError):
                pass

    # -- 小工具 --
    @staticmethod
    def _days_param(query, required: bool = True):
        """range=7d|30d|90d（接口 A9／B11）或 days=7（骨架兼容）。"""
        range_raw = query.get("range", [None])[0]
        if range_raw is not None:
            from records import RANGE_DAYS
            if range_raw not in RANGE_DAYS:
                raise ApiError("invalid_range", "range 只能是 7d、30d 或 90d。")
            return RANGE_DAYS[range_raw]
        days_raw = query.get("days", [None])[0]
        if days_raw is None:
            return None if not required else 7
        try:
            days = int(days_raw)
        except ValueError:
            raise ApiError("invalid_range", "days 须为整数。")
        if days <= 0:
            raise ApiError("invalid_range", "days 须大于 0。")
        return days

    def _check_terminal_header_matches(self, terminal_id):
        header_tid = self.headers.get("X-Terminal-Id")
        if header_tid and terminal_id and header_tid != terminal_id:
            raise ApiError("terminal_mismatch",
                           "请求体 terminal_id 与 X-Terminal-Id 不一致。")

    @staticmethod
    def _check_app_family(app_id: str, family_id: str) -> None:
        """审批／撤回前校验申请单属于调用者家庭组（不泄露别家单号是否存在）。"""
        app = store.data["applications"].get(app_id)
        if app is None or app["family_id"] != family_id:
            raise ApiError("application_not_found", "申请单不存在。", 404)

    def _change_passwords(self, user: dict, b: dict) -> dict:
        ptype = b.get("type")
        if ptype == "login":
            return users.change_login_password(
                store, user["id"], b.get("old_password"),
                b.get("new_password"), b.get("new_password_confirm"))
        if ptype == "admin":
            return users.change_admin_password(
                store, user["id"], b.get("new_admin_password"))
        raise ApiError("invalid_password_type",
                       "type 只能是 login（登录密码）或 admin（管理密码）。")

    def _warm_city_cache(self) -> None:
        """登录／注册成功后预热城市缓存（547 ABB 部分，NYX 528／529 定稿）。

        按本次登录连接的源 IP 查一次城市，记进 24 小时缓存；后面
        /lockscreen 同一出口 IP 直接用缓存，不每次重查。
        只用连接源 IP，绝不取终端上报的 IP，不单独查出口 IP。
        失败静默（内网 IP 本来就查不到城市），绝不影响登录／注册。
        """
        try:
            weather_memo.warm_city_for_ip(self.client_address[0])
        except Exception:
            pass

    def _login_auto_bind(self, out: dict) -> None:
        """登录自动绑（浩爷 2026-11-25 定 A，NYX 279/281）：登录成功后若请求
        带 X-Terminal-Id，把该设备绑到登录账号家庭组；结果以附加字段
        terminal_auto_bind 放进登录响应体（旧客户端忽略即可）。绑定任何
        异常都不影响登录成功；未带该头时完全无感（B1/B7 等旧链路零变化）。
        """
        tid = (self.headers.get("X-Terminal-Id") or "").strip()
        if not tid:
            return
        try:
            out["terminal_auto_bind"] = binding.auto_bind_on_login(
                store, tid, out.get("family_id"),
                out.get("member_no"), out.get("user_id"))
        except Exception as e:                  # 双保险：绝不影响登录响应
            out["terminal_auto_bind"] = {"status": "error",
                                         "message": str(e)[:120]}

    def _send_code_stub(self, b: dict) -> dict:
        """B14 发短信验证码：短信通道未接，stub 明标，503，不发短信。
        v7 测试模式例外（Nyx 515）：演示账号发码直接成功，不真发短信；
        验证码 App 端已写死，响应里不带码值。"""
        phone = b.get("phone") or ""
        if not re.match(r"^\d{11}$", phone):
            raise ApiError("invalid_phone", "手机号须为 11 位数字。")
        if TEST_MODE and users.is_demo_phone(phone):
            return {"sent": True, "test_mode": True, "sms_code_stub": True,
                    "message": "测试模式：演示账号验证码已发送"
                               "（stub，不真发短信）。"}
        raise ApiError(
            "sms_not_configured",
            "短信通道未接通：本版不发短信；注册／重置只做验证码格式检查"
            "（stub），真实短信通道定了再接。", 503)

    def _set_memo(self, family_id: str, b: dict) -> dict:
        memo = b.get("memo")
        if not isinstance(memo, str) or len(memo) > MEMO_MAX_LENGTH:
            raise ApiError("invalid_memo",
                           f"MEMO 须为不超过 {MEMO_MAX_LENGTH} 字的字符串。")
        with store.lock:
            store.data["memo"] = memo
            store.save()
        return {"memo": memo}

    # -- /solve（异步 + 轮询）--
    def _post_solve(self):
        user = self._auth_user()[0]
        terminal_id = self.headers.get("X-Terminal-Id") or ""
        if not terminal_id:
            raise ApiError("missing_terminal_id", "缺少 X-Terminal-Id 头。")
        ikey = self._idem_key(user["family_id"], "solve")
        self._rate(f"solve:{terminal_id}", RATE_LIMIT_SOLVE)
        b = store.data["bindings"].get(terminal_id)
        bound = b is not None and b["family_id"] == user["family_id"]
        if not bound and TEST_MODE:
            # v6 测试模式（Nyx 498）：格式合法的 16 位设备号（含演示值
            # 2864190752386419 与 App 生成的 deviceId）都按已绑定处理，
            # 不再 403 terminal_not_bound；只认格式、不写绑定记录、不落盘。
            # 格式本身仍按 binding.validate_device_id 的白名单校验
            # （首位 0–4），非法格式仍按未绑定 403，生产校验一字不动。
            try:
                binding.validate_device_id(terminal_id)
                bound = True
            except binding.BindingError:
                bound = False
        if not bound:
            raise ApiError("terminal_not_bound",
                           "终端未绑定到当前家庭账号，网关拒绝继续。", 403)
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_UPLOAD_BYTES:
            raise ApiError("payload_too_large",
                           f"图片超过 8MB 上限（{length} 字节）。", 413)
        if length <= 0:
            raise ApiError("empty_image", "没有收到图片数据。")
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip()
        if ctype not in ("image/jpeg", "application/octet-stream"):
            raise ApiError("unsupported_media_type",
                           "只收 raw JPEG（Content-Type: image/jpeg）。", 415)
        image = self.rfile.read(length)
        self._body_read += len(image)  # v5：记已读，供异常出口排空时扣减
        if len(image) > MAX_UPLOAD_BYTES:
            raise ApiError("payload_too_large", "图片超过 8MB 上限。", 413)
        if not image.startswith(JPEG_MAGIC):
            raise ApiError("unsupported_media_type",
                           "不是 JPEG 数据（缺少 JPEG 文件头）。", 415)
        # v2：查幂等→建 job→写幂等记录整段在 store.lock 内原子完成，
        # 判题线程也在锁内起——同键并发只建一个 job（见 TESTLOG.md §6）。
        # 请求体在锁外读完（socket 读不进锁）；响应在锁外发（socket 写不进锁）。
        view = None
        created = False
        with store.lock:
            hit = self._idem_lookup(ikey)
            if hit is not None:
                job = store.data["jobs"].get(hit["body"]["job_id"])
                if job is not None:
                    view = self._solve_view(job)
            if view is None:
                job_id = f"job-{uuid.uuid4().hex[:12]}"
                cfg = modelconfig._config(store, user["family_id"])
                job = new_job(job_id, terminal_id, user["family_id"],
                              len(image), cfg["selected_model"])
                job["stages"]["uploading"] = "done"
                job["stages"]["gateway"] = "done"
                job["stage"] = "model"
                job["stages"]["model"] = "processing"
                store.data["jobs"][job_id] = job
                store.data["idempotency"][ikey] = {
                    "status": 202, "body": {"job_id": job_id}}
                # 判题线程锁内起：job 与幂等记录已同时可见，不会漏跑。
                # 快照仍在锁外写：先起线程再写快照，快照写失败（500）时
                # job 也能跑完，不会永远卡在 processing（见接口 A2）。
                t = threading.Thread(target=_run_judge,
                                     args=(job_id, image), daemon=True)
                t.start()
                created = True
                view = self._solve_view(job)
        if created:
            store.save()
        return self._send(202, view)

    def _get_solve(self, job_id: str):
        user = self._auth_user()[0]
        job = store.data["jobs"].get(job_id)
        if job is None:
            raise ApiError("job_not_found", "题目不存在。", 404)
        if job["family_id"] != user["family_id"]:
            raise ApiError("forbidden", "不是你家的题目。", 403)
        return self._send(200, self._solve_view(job))

    def _cancel_solve(self, job_id: str):
        user = self._auth_user()[0]
        with store.lock:
            job = store.data["jobs"].get(job_id)
            if job is None:
                raise ApiError("job_not_found", "题目不存在。", 404)
            if job["family_id"] != user["family_id"]:
                raise ApiError("forbidden", "不是你家的题目。", 403)
            if job["status"] != "processing":
                raise ApiError("job_not_cancellable",
                               f"任务已是 {job['status']} 状态，不能取消。", 409)
            job["cancelled"] = True
            job["status"] = "cancelled"
            job["stages"]["model"] = "pending"
            store.save()
        return self._send(200, {"job_id": job_id, "status": "cancelled"})

    def _skip_question(self, job_id: str):
        user = self._auth_user()[0]
        b = self._read_json()
        with store.lock:
            job = store.data["jobs"].get(job_id)
            if job is None:
                raise ApiError("job_not_found", "题目不存在。", 404)
            if job["family_id"] != user["family_id"]:
                raise ApiError("forbidden", "不是你家的题目。", 403)
            qi = b.get("question_index", 0)
            store.data["records"].append(new_record(
                user["family_id"], "skip",
                {"job_id": job_id, "question_index": qi,
                 "terminal_id": job["terminal_id"]}))
            store.save()
        return self._send(200, {"skipped": True, "job_id": job_id,
                                "question_index": qi})

    def _solve_view(self, job: dict) -> dict:
        view = public_job(job, include_answer=False)
        # 测试模式 mock 结果不能冒充真实判题：显式回传标记，App 端必须提示
        if job.get("mock") or job.get("result_source") == "mock":
            view["mock"] = True
            view["result_source"] = "mock"
        questions = job.get("questions") or []
        view["questions"] = [{
            "index": i,
            # v8（Nyx 538，A4）：轮询 question 视图补 question_id；
            # question_index 按接口 A2 从 1 起（index 仍保留 0 起，供老客户端）。
            "question_id": q.get("question_id"),
            "question_index": i + 1,
            "answer_status": q.get("answer_status"),
            "correct": q.get("correct"),
            "thinking": q.get("thinking"),
            "unlocked": q.get("unlocked", False),
            "answer": q.get("answer") if q.get("unlocked") else None,
        } for i, q in enumerate(questions)]
        view["answer"] = job.get("answer") if job.get("unlocked") else None
        # 轮询提示：1 秒一次、最多 60 秒、超时 50401（由终端判定，见接口 A2）。
        try:
            created = time.mktime(time.strptime(
                job["created_at"], "%Y-%m-%dT%H:%M:%S"))
            elapsed = max(0, int(time.time() - created))
        except (ValueError, TypeError):
            elapsed = 0
        view["elapsed_seconds"] = elapsed
        view["poll"] = {"interval_seconds": POLL_INTERVAL_SECONDS,
                        "timeout_seconds": POLL_TIMEOUT_SECONDS,
                        "timeout_code": POLL_TIMEOUT_CODE}
        return view

    # -- 模型 / 配置视图（按家庭组分）--
    def _server_config_view(self, family_id: str) -> dict:
        cfg = store.data["server_configs"].get(family_id) or {
            "mode": "official", "custom_address": None}
        return {
            "mode": cfg["mode"],  # official | custom，二选一互斥，默认 official
            "official_server": OFFICIAL_SERVER_HOST,
            "official_url": OFFICIAL_SERVER_URL,
            "custom_address": cfg.get("custom_address"),
            "note": "服务器配置按家庭组存；板子下次连接拉取（已定：拉取），"
                    "不主动推。",
        }

    def _set_server_config(self, family_id: str, body: dict) -> dict:
        mode = body.get("mode", body.get("server_mode"))
        if mode not in ("official", "custom"):
            raise ApiError("invalid_server_mode",
                           "服务器模式只能是 official（官方）或 custom（自有），二选一。")
        custom = body.get("custom_address", body.get("custom_ip"))
        if mode == "custom" and not custom:
            raise ApiError("missing_custom_address",
                           "选自有服务器须手填地址。")
        if mode == "official":
            custom = None  # 互斥：选官方即清空自有地址
        with store.lock:
            store.data["server_configs"][family_id] = {
                "mode": mode, "custom_address": custom,
                "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S",
                                            time.localtime())}
            store.save()
        return self._server_config_view(family_id)


def _run_judge(job_id: str, image: bytes) -> None:
    """后台判题线程：五阶段打点；失败不编答案；取消则停。"""
    delay = float(os.environ.get("JIANTI_JUDGE_DELAY_SECONDS", "0") or 0)
    job = store.data["jobs"].get(job_id)
    if job is None:
        return
    # 测试用延迟（仅用于测取消分支，默认 0；见 README）。
    waited = 0.0
    while waited < delay:
        time.sleep(0.2)
        waited += 0.2
        if store.data["jobs"].get(job_id, {}).get("cancelled"):
            return
    with store.lock:
        job = store.data["jobs"].get(job_id)
        if job is None or job.get("cancelled") or job["status"] != "processing":
            return
    try:
        if TEST_MODE and not judge.is_connected():
            # v6 测试模式（Nyx 500）：无真 Key 时不走 model_not_connected
            # 失败，改走模拟模型（mock_judge_result），让 App 走完结果屏、
            # 扣卡、使用记录全链路；有真 Key 时仍走真实判题，模拟不插手。
            result = judge.mock_judge_result()
        else:
            result = judge.judge_image(image, job["model_id"])
    except judge.ModelNotConnectedError as e:
        _fail_job(job_id, e.payload, "model_unavailable")
        return
    except judge.JudgeError as e:
        _fail_job(job_id, e.payload, "model_unavailable")
        return
    with store.lock:
        job = store.data["jobs"].get(job_id)
        if job is None or job.get("cancelled") or job["status"] != "processing":
            return
        results = result.get("results", [])
        # 判题原文落盘（浩爷 2026-11-26 定 B）：judge 两条后端均已返回
        # raw_text，成功/空题分支都存进 job，供 no_question 定根因
        # （是模型没见图还是提示词/解析偏差）。失败抛异常路径另记 error。
        job["raw_model_text"] = result.get("raw_text")
        if not results:
            job["status"] = "failed"
            job["stages"]["model"] = "done"
            job["stages"]["nlp"] = "failed"
            job["error"] = {"code": "no_question",
                            "message": "照片里没有识别出可判的数学题。"}
            job["failure_reason"] = "no_question"
            store.data["records"].append(new_record(
                job["family_id"], "solve_failed",
                {"job_id": job_id, "error": "no_question",
                 "terminal_id": job["terminal_id"]}))
            _save_quietly()
            return
        job["stages"]["model"] = "done"
        job["stages"]["nlp"] = "done"
        if result.get("mock"):
            # v6 测试模式：模拟结果明确标注，App 可凭 mock/result_source
            # 区分，不得冒充真实模型输出。
            job["mock"] = True
            job["result_source"] = "mock"
        # answer_status（NYX 924，ABB 定）：先判 answered 再判对错。
        # answered=true（有作答）-> "answered"，correct 照模型给；
        # answered=false（没作答）-> "unanswered"，correct 强制空（不贴对错），
        # thinking（思路）照给；answer 仍按解锁口径扣在服务端。
        # 模型没报 answered（旧提示词／桥侧未同步）时默认当作有作答，
        # 保持老行为不变。no_question 分支在上面已 return，不动。
        job["questions"] = []
        for i, r in enumerate(results):
            answered = r.get("answered", True)
            if isinstance(answered, str):
                answered = answered.strip().lower() not in (
                    "false", "0", "no")
            else:
                answered = bool(answered)
            job["questions"].append({
                "index": i,
                # v8（Nyx 538，A4）：本批内题 ID，轮询视图与扣卡返回都凭它；
                # 格式 job_id-q<从 1 起的题号>，建 job 时一次定死、落快照，
                # 同一批同一题始终同一个值（接口 A2 的 question_id 定义）。
                "question_id": f"{job_id}-q{i + 1}",
                "answer_status": "answered" if answered else "unanswered",
                "correct": r.get("correct") if answered else None,
                "thinking": r.get("thinking"),
                "answer": r.get("answer"),
                "unlocked": False,
            })
        first = job["questions"][0]
        job["answer_status"] = first["answer_status"]
        job["correct"], job["thinking"], job["answer"] = (
            first["correct"], first["thinking"], first["answer"])
        job["stages"]["delivering"] = "done"
        job["stage"] = "delivering"
        job["status"] = "done"
        store.data["records"].append(new_record(
            job["family_id"], "solve_success",
            {"job_id": job_id, "questions": len(results),
             "terminal_id": job["terminal_id"]}))
        store.data["records"].append(new_record(
            job["family_id"], "thinking",
            {"job_id": job_id, "questions": len(results),
             "terminal_id": job["terminal_id"]}))
        _save_quietly()


def _fail_job(job_id: str, error_payload: dict, failure_reason: str) -> None:
    with store.lock:
        job = store.data["jobs"].get(job_id)
        if job is None or job.get("cancelled") or job["status"] != "processing":
            return
        job["status"] = "failed"
        job["stages"]["model"] = "failed"
        job["error"] = error_payload
        job["failure_reason"] = failure_reason
        store.data["records"].append(new_record(
            job["family_id"], "solve_failed",
            {"job_id": job_id, "error": error_payload["code"],
             "terminal_id": job["terminal_id"]}))
        _save_quietly()


def _save_quietly() -> None:
    """后台线程写快照：失败记 stderr（不静默吞掉），内存仍是权威状态。"""
    try:
        store.save()
    except OSError as e:
        print(f"[gateway] 快照写入失败：{e}", file=sys.stderr, flush=True)


# 僵尸 job 判定：processing 超过 2×POLL_TIMEOUT（120 秒）仍未终结——
# 判题线程已不在（进程重启过），再等也不会有结果。
STALE_JOB_MAX_AGE_SECONDS = 2 * POLL_TIMEOUT_SECONDS


def reap_stale_processing_jobs(store_obj) -> list[str]:
    """启动扫尾（v3，RECHECK #15）：扫一遍 jobs，把 processing 且
    created_at 超 120 秒的僵尸 job 标 failed，failure_reason 用新码
    process_interrupted（不撞已定六个码），并补一条终态 usage 记录
    （solve_failed，error=process_interrupted）。created_at 解析不出的
    不动（等判题线程／取消处理）。返回被标 failed 的 job_id 列表。"""
    now = time.time()
    reaped: list[str] = []
    with store_obj.lock:
        for job in list(store_obj.data["jobs"].values()):
            if job.get("status") != "processing":
                continue
            try:
                created = time.mktime(time.strptime(
                    job["created_at"], "%Y-%m-%dT%H:%M:%S"))
            except (ValueError, TypeError):
                continue
            if now - created <= STALE_JOB_MAX_AGE_SECONDS:
                continue
            job["status"] = "failed"
            job["stages"]["model"] = "failed"
            job["error"] = {"code": "process_interrupted",
                            "message": "服务端重启前该任务未完成，已标记失败，请重拍。"}
            job["failure_reason"] = "process_interrupted"
            store_obj.data["records"].append(new_record(
                job["family_id"], "solve_failed",
                {"job_id": job["job_id"], "error": "process_interrupted",
                 "terminal_id": job["terminal_id"]}))
            reaped.append(job["job_id"])
        if reaped:
            store_obj.save()
    return reaped


def run(host: str = "127.0.0.1", port: int = GATEWAY_PORT,
        data_dir: str | None = None):
    global store
    try:
        store = Store(data_dir=data_dir)
    except SnapshotCorruptError as e:
        print(f"启动失败：{e}", file=sys.stderr, flush=True)
        sys.exit(2)
    # v3 启动扫尾：先清僵尸 job 再起网关（RECHECK #15）。
    reaped = reap_stale_processing_jobs(store)
    if reaped:
        print(f"启动扫尾：{len(reaped)} 个僵尸 job 标 failed"
              "（process_interrupted）", file=sys.stderr, flush=True)
    server = ThreadingHTTPServer((host, port), Handler)
    print(f"jianti platform server v{VERSION} listening on "
          f"http://{host}:{port} (model_connected={judge.is_connected()} "
          f"test_mode={TEST_MODE})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    run(host=os.environ.get("JIANTI_HOST", "127.0.0.1"),
        port=int(os.environ.get("JIANTI_PORT", str(GATEWAY_PORT))))


