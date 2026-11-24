"""binding.py — 终端绑定 / 解绑：设备号 <-> 家庭账号（平台服务器端 v1）.

规则（手册 v2 §1.5、§3.5，PRD 5.3／6.4）：
- 设备 ID 16 位纯数字 = 1 位类型码 + 15 位设备号；显示 4-4-4-4 分组。
  首位白名单（Nyx 189③，RECHECK #12，v3 已实现）：只认 0–4
  （0 其他／未知、1 嵌入式、2 安卓、3 苹果、4 鸿蒙），首位 5–9
  （预留号）绑定直接拒 400 invalid_device_id。`DEVICE_TYPE_NAMES`
  里 5–9 的“预留”标注留着当文档说明。
- 服务端唯一性校验是强制规则：
  - 设备已绑到别的家庭组 -> 拒绝绑定，409 device_id_duplicate；
  - 已绑到本家庭组 -> 返回“已绑定”（already_bound=true），不重复加行；
  - 不存在 -> 按新设备登记（类型码以设备上报／扫码解出的首位为准），再建绑定。
  服务端不替设备编 ID，不许终端自动顺延。
- 扫码绑定必须接真实扫码数据：载荷里解析不出合法设备 ID 就报 422，
  绝不顺延编号、绝不按类型码默认成嵌入式；固定二维码、写死前缀＋尾号
  自增的分支不许存在（包括“演示用”的保留）。
- 二维码载荷兼容两种形式：带 JianTi- 前缀连续串与纯 16 位数字。
- 解绑：只解绑定关系，设备登记保留（重绑不用重新登记）；解绑权限
  暂按接口文档“只有管理员能解绑”执行（PRD 口径待 Nyx 确认，
  见 ASSUMPTIONS.md V1-A7），权限由网关层在调本模块前校验。
- 终端编号 terminal_no 在家庭组内顺延（第 15 屏最左边编号列），
  解绑后不复用。
"""
from __future__ import annotations

import re

from models import format_device_id, new_binding, new_terminal
from server_config import DEVICE_ID_LENGTH, DEVICE_TYPE_NAMES

_DEVICE_RE = re.compile(r"^[0-4]\d{15}$")


class BindingError(Exception):
    def __init__(self, code: str, message: str, status: int = 400):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def validate_device_id(device_id: str) -> str:
    if not isinstance(device_id, str) or not _DEVICE_RE.match(device_id):
        raise BindingError("invalid_device_id",
                           "设备 ID 须为 16 位纯数字，首位类型码只认 0–4"
                           "（0 其他／未知、1 嵌入式、2 安卓、3 苹果、4 鸿蒙），"
                           "首位 5–9 为预留号，不许绑定。")
    return device_id


def device_type_name(device_id: str) -> str:
    return DEVICE_TYPE_NAMES.get(device_id[0], "未知")


def parse_scan_payload(payload: str) -> str:
    """从真实扫码载荷里解析设备 ID。

    接受两种已在原型里出现的载荷形式：
    - 纯 16 位数字：`1040059832773436`
    - 带前缀连续串：`JianTi-1040059832773436`（板子 v113，见对齐 M4）
    其他形式（空、二维码 SVG、固定演示码、无设备号）一律 422，
    绝不顺延编号、绝不按类型码默认成嵌入式。
    容忍空格（Nyx 189②，v3 已实现）：设备号常以 4-4-4-4 分组显示，
    扫码载荷里可能带空格（如 `JianTi-2864 1907 5238 6419`），先去空格再解析。
    """
    if not isinstance(payload, str) or not payload.strip():
        raise BindingError("scan_no_data",
                           "没有真实扫码数据，无法绑定。", 422)
    text = payload.strip().replace(" ", "")
    m = re.search(r"(\d{16})", text)
    if not m:
        raise BindingError("scan_no_device_id",
                           "扫码数据中没有合法的 16 位设备 ID，无法绑定。", 422)
    return validate_device_id(m.group(1))


def public_terminal(store, device_id: str, binding: dict | None = None) -> dict:
    b = binding if binding is not None else store.data["bindings"].get(device_id)
    term = store.data["terminals"].get(device_id)
    return {
        "device_id": device_id,
        "device_id_display": format_device_id(device_id),
        "device_type": device_type_name(device_id),
        "family_id": b["family_id"] if b else None,
        "member_no": b.get("member_no") if b else None,
        "terminal_no": b.get("terminal_no") if b else None,
        "bind_method": b.get("bind_method") if b else None,
        "bound_at": b.get("bound_at") if b else None,
        "already_bound": b is not None,
        "registered_at": term.get("first_seen_at") if term else None,
    }


def bind_terminal(store, device_id: str, family_id: str, bound_by: str,
                  method: str, member_no: int | None = None) -> dict:
    with store.lock:
        validate_device_id(device_id)
        if family_id not in store.data["families"]:
            raise BindingError("family_not_found", "家庭组不存在。", 404)
        if member_no is not None:
            from models import member_key
            m = store.data["members"].get(member_key(family_id, member_no))
            if m is None or m["status"] != "active":
                raise BindingError("member_not_found",
                                   "绑定的成员不存在或不是正式成员。")
        existing = store.data["bindings"].get(device_id)
        if existing is not None:
            if existing["family_id"] == family_id:
                # 已绑到本家庭组：返回“已绑定”，不重复加行（手册 §1.5）。
                return public_terminal(store, device_id, existing)
            raise BindingError(
                "device_id_duplicate",
                f"设备 ID {format_device_id(device_id)} 已绑定到别的家庭组，"
                "重复设备 ID 直接拒绝。",
                409)
        # 新设备：先登记设备（类型码以码的首位为准），再建绑定。
        if device_id not in store.data["terminals"]:
            store.data["terminals"][device_id] = new_terminal(device_id)
        fam = store.data["families"][family_id]
        terminal_no = fam["next_terminal_no"]
        fam["next_terminal_no"] += 1
        binding = new_binding(device_id, family_id, bound_by, method,
                              member_no, terminal_no)
        store.data["bindings"][device_id] = binding
        store.save()
        out = public_terminal(store, device_id, binding)
        out["already_bound"] = False
        return out


def bind_scan(store, scan_payload: str, family_id: str, bound_by: str,
              member_no: int | None = None) -> dict:
    """扫码绑定：先解析真实扫码数据，解析不出就 422，不编 ID。"""
    device_id = parse_scan_payload(scan_payload)
    return bind_terminal(store, device_id, family_id, bound_by, "scan",
                         member_no=member_no)


def unbind_terminal(store, device_id: str) -> dict:
    """解绑：只删绑定关系，设备登记保留；解绑后该设备再调业务接口按拒收处理。"""
    with store.lock:
        if device_id not in store.data["bindings"]:
            raise BindingError("terminal_not_found", "该终端未绑定。", 404)
        b = store.data["bindings"].pop(device_id)
        store.save()
        return {"unbound": True, "device_id": device_id,
                "terminal_no": b.get("terminal_no")}


def list_terminals(store, family_id: str) -> list:
    """第 15 屏已绑定终端列表：按终端编号排序。"""
    with store.lock:
        items = [b for b in store.data["bindings"].values()
                 if b["family_id"] == family_id]
        items.sort(key=lambda b: b.get("terminal_no") or 0)
        out = []
        for b in items:
            out.append({
                "terminal_no": b["terminal_no"],
                "device_id": b["device_id"],
                "device_id_display": format_device_id(b["device_id"]),
                "device_type": device_type_name(b["device_id"]),
                "member_no": b.get("member_no"),
                "bind_method": b.get("bind_method"),
                "bound_at": b.get("bound_at"),
            })
        return out


def auto_bind_on_login(store, device_id, family_id: str, member_no,
                       bound_by: str) -> dict:
    """登录自动绑（浩爷 2026-11-25 定 A，NYX 279/281）：App 登录带
    X-Terminal-Id，后台把该设备绑到登录账号家庭组。

    口径（浩爷定）：
    - 终端以首个绑定为准：设备已绑到别的家庭组 -> 不抢绑，登录照常成功，
      返回 bound_elsewhere，等用户先解绑再扫码（扫码重复时 bind_terminal
      本就抛 409 device_id_duplicate，App 据此提醒）。
    - 设备未绑 -> 绑到当前家庭组（method="login"，member_no 取登录成员）。
    - 已绑本家庭组 -> already，幂等不重复加行。
    - 任何失败都不影响登录本身：本函数永不抛错，状态进返回体供 App 提示。
    /solve 的 A1 查绑定口径不动（NYX 275/279）。
    """
    try:
        if not device_id or not str(device_id).strip():
            return {"status": "skipped", "reason": "no_terminal_id"}
        device_id = str(device_id).strip()
        try:
            validate_device_id(device_id)
        except BindingError as e:
            return {"status": "invalid_device_id", "message": e.message}
        existing = store.data["bindings"].get(device_id)
        if existing is not None:
            if existing["family_id"] == family_id:
                return {"status": "already",
                        "device_id_display": format_device_id(device_id)}
            return {"status": "bound_elsewhere",
                    "device_id_display": format_device_id(device_id),
                    "message": "该终端已绑定到别的家庭组：先解绑，再扫码绑定。"}
        out = bind_terminal(store, device_id, family_id, bound_by,
                            "login", member_no=member_no)
        return {"status": "bound", "device_id_display":
                out.get("device_id_display"),
                "terminal_no": out.get("terminal_no")}
    except BindingError as e:
        return {"status": "bind_rejected", "code": e.code,
                "message": e.message}
    except Exception as e:                      # 兜底：登录永不被绑定拖垮
        return {"status": "error", "message": str(e)[:120]}
