"""models.py — 数据模型（纯 dict 工厂函数，无外部依赖）.

相对探针骨架的改动（平台服务器端 v1，手册 v2 §1.3、§3.4）：
- 成员从家庭组记录里拆出来，单独成实体（主键：家庭组 ID＋成员编号；
  编号跟人走，转让不换号，新成员顺延，删掉的编号不复用）。
- 设备（登记）与绑定关系拆成两个实体：devices.json 存设备登记，
  bindings.json 存绑定关系（一台设备同一时间只绑一个家庭组）。
- 家庭组记录加 next_member_no／next_terminal_no 计数器（编号顺延用）。
- 申请单加 idempotency_key（重发去重用）；job 的五阶段名改成接口已定
  口径 uploading／gateway／model／nlp／delivering，加 cancelled 标志。
- 所有记录只存真实入库的数据；演示数不许出现在这里。
- 密码只存哈希（见 users.py），这里不出现任何明文密码字段。
"""
from __future__ import annotations

import time
import uuid


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def new_id(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex[:12]}"


def new_user(phone: str, login_password_hash: str, salt: str,
             admin_password_hash: str, admin_salt: str, family_id: str,
             member_no: int, role: str = "管理员") -> dict:
    return {
        "id": new_id("user"),
        "phone": phone,
        "login_password_hash": login_password_hash,  # PBKDF2 哈希，非明文
        "login_salt": salt,
        "admin_password_hash": admin_password_hash,  # 6 位管理密码的哈希，非明文
        "admin_salt": admin_salt,
        "family_id": family_id,
        "member_no": member_no,  # 该账号在家庭组里的成员编号
        "role": role,  # 管理员 / 成员 / 待审批
        "created_at": now_iso(),
    }


def new_family_group(owner_user_id: str, family_id: str) -> dict:
    return {
        "family_id": family_id,  # 8 位纯数字，服务端生成，终身不变
        "owner_user_id": owner_user_id,
        # 状态：virtual 虚拟组（注册时建）/ solidified 已固化（首个新成员加入时翻转，ID 不变）。
        "status": "virtual",
        "daily_cards": 5,
        "advance_ratio": 0,  # 定稿默认 0（手册 §2.3）
        "next_member_no": 2,  # 成员 1 是注册人；新成员顺延，删掉的编号不复用
        "next_terminal_no": 1,  # 终端编号顺延，解绑后不复用
        "created_at": now_iso(),
    }


def new_member(family_id: str, member_no: int, user_id: str, phone: str,
               role: str = "成员", status: str = "active") -> dict:
    # status: active 正式成员 / pending 待审批 / rejected 已拒绝（行消失，见 families.py）
    return {
        "family_id": family_id,
        "member_no": member_no,
        "user_id": user_id,
        "phone": phone,
        "role": role,  # 管理员 / 成员 / 待审批
        "status": status,
        "created_at": now_iso(),
    }


def member_key(family_id: str, member_no: int) -> str:
    return f"{family_id}|{member_no}"


def new_terminal(device_id: str) -> dict:
    # 设备登记：只记设备本身（类型码取设备 ID 首位），不记绑定关系。
    return {
        "device_id": device_id,  # 16 位纯数字
        "type_code": device_id[0],
        "first_seen_at": now_iso(),
    }


def new_binding(device_id: str, family_id: str, bound_by: str,
                bind_method: str, member_no: int | None,
                terminal_no: int) -> dict:
    return {
        "device_id": device_id,
        "family_id": family_id,
        "member_no": member_no,  # 绑给哪个成员；不填为 None（嵌入式等无成员对应）
        "terminal_no": terminal_no,  # 家庭组内终端编号（最左边编号列），顺延
        "bound_by": bound_by,
        "bind_method": bind_method,  # "scan" | "manual"
        "bound_at": now_iso(),
    }


def new_job(job_id: str, terminal_id: str, family_id: str,
            image_bytes_len: int, model_id: str) -> dict:
    # 答案（answer）只存服务端；未解锁前 GET /solve/{id} 不返回它。
    # stages 取值：pending / processing / done / failed；五阶段名按接口 A2。
    return {
        "job_id": job_id,
        "terminal_id": terminal_id,
        "family_id": family_id,
        "image_bytes": image_bytes_len,
        "model_id": model_id,
        "status": "processing",  # processing | done | failed | cancelled
        "stages": {s: "pending" for s in
                   ["uploading", "gateway", "model", "nlp", "delivering"]},
        "stage": "uploading",  # 当前阶段（接口 A2 字段）
        "correct": None,        # True/False/None（未判出）
        "answer_status": None,  # "answered" | "unanswered" | None（未判/无题/失败前）
        "thinking": None,       # 思路：先下发
        "answer": None,         # 答案：扣在服务端，未解锁不下发
        "raw_model_text": None,  # 模型原文（服务端留存，不直接下发给学生端展示）
        "error": None,          # {"code": ..., "message": ...}，失败时
        "failure_reason": None,  # 接口 A2 字段：recognition_failed / model_unavailable / no_question / process_interrupted（v3 启动扫尾新码）
        "cancelled": False,
        "unlocked": False,
        "mock": False,          # v6 测试模式模拟结果时置 True（字段明标）
        "result_source": "real",  # v6："real" 真实模型 / "mock" 模拟返回
        "created_at": now_iso(),
    }


def new_application(app_id: str, family_id: str, terminal_id: str | None,
                    requested: dict, idempotency_key: str | None = None,
                    question_id: str | None = None) -> dict:
    return {
        "id": app_id,
        "family_id": family_id,
        "terminal_id": terminal_id,
        "question_id": question_id,
        "requested": requested,  # {"grant_type": "count"|"ratio", "value": int} 或 {}
        "status": "pending",     # pending | approved | rejected | withdrawn
        "granted": None,         # 批准时写入 {"grant_type": ..., "value": ..., "cards": int}
        "idempotency_key": idempotency_key,  # 重发去重用（手册 §2.4）
        "created_at": now_iso(),
        "decided_at": None,
    }


def new_card_ledger_entry(family_id: str, kind: str, delta: int,
                          balance_after: int, ref: str) -> dict:
    # kind: deduct | advance | grant | config；每笔留痕，余额不为负。
    return {
        "id": new_id("ledger"),
        "family_id": family_id,
        "kind": kind,
        "delta": delta,
        "balance_after": balance_after,
        "ref": ref,  # job_id / application_id / "manual"
        "created_at": now_iso(),
    }


def new_record(family_id: str, kind: str, detail: dict) -> dict:
    # kind: solve_success | thinking | deduct | advance | grant |
    #       application | solve_failed | skip ...
    # 只记真实发生的事件；第 13 屏三组数全部由此汇总，不用演示数。
    # detail 里可带 terminal_id / member_no，供日志页拼“终端 N／成员 N”。
    return {
        "id": new_id("rec"),
        "family_id": family_id,
        "kind": kind,
        "detail": detail,
        "created_at": now_iso(),
    }


def public_job(job: dict, include_answer: bool = False) -> dict:
    out = {
        "job_id": job["job_id"],
        "status": job["status"],
        "stage": job.get("stage", "uploading"),
        "stage_index": sum(1 for s in
                           ["uploading", "gateway", "model", "nlp", "delivering"]
                           if job["stages"].get(s) == "done"),
        "stages": job["stages"],
        "correct": job["correct"],
        "answer_status": job.get("answer_status"),  # "answered"|"unanswered"|None（未判/无题/失败）
        "thinking": job["thinking"],
        "unlocked": job["unlocked"],
        "model_id": job["model_id"],
        "mock": job.get("mock", False),  # v6：模拟结果时 True，App 可区分
        "result_source": job.get("result_source", "real"),  # v6：real/mock
        "error": job["error"],
        "failure_reason": job.get("failure_reason"),
        "created_at": job["created_at"],
    }
    # 答案只有解锁后才下发；未解锁时连字段都不给，避免客户端误以为有答案。
    if include_answer:
        out["answer"] = job["answer"]
    return out


def format_device_id(device_id: str) -> str:
    """16 位 -> 4-4-4-4 分组显示，冒号紧挨数字 1 的写法由调用方拼。"""
    return " ".join(device_id[i:i + 4] for i in range(0, 16, 4))
