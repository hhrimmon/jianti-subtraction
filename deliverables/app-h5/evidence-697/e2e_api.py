# -*- coding: utf-8 -*-
"""v8 真后端全链路数据面实测：注册(已有号跳过)→登录→查卡→usage→solve→poll→扣卡→usage变化。"""
import json, urllib.request, urllib.error, time

BASE = "http://192.168.0.108:8000"
PHONE = "18601109233"
PW = "1"
ADMIN_PW = "111111"
DEVICE = "2864190752386419"


def req(path, token=None, body=None, method=None, raw=None, headers=None):
    h = dict(headers or {})
    data = None
    if raw is not None:
        data = raw
        h["Content-Type"] = "image/jpeg"
    elif body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    if token:
        h["Authorization"] = "Bearer " + token
    r = urllib.request.Request(BASE + path, data=data, headers=h,
                               method=method or ("POST" if data else "GET"))
    try:
        resp = urllib.request.urlopen(r, timeout=15)
        return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:300]


print("=== 1. login ===")
st, res = req("/auth/login", body={"phone": PHONE, "password": PW})
print(st, json.dumps(res, ensure_ascii=False)[:200])
token = res.get("token") if isinstance(res, dict) else None
if not token:
    print("LOGIN FAILED, abort"); raise SystemExit
H = {"X-Terminal-Id": DEVICE}

print("\n=== 1b. verify admin password ===")
st, res = req("/auth/admin-password/verify", token, body={"admin_password": ADMIN_PW})
print(st, json.dumps(res, ensure_ascii=False)[:200])

print("\n=== 2. cards/check (before) ===")
st, res = req("/cards/check", token, body={"question_id": "probe-1"}, headers=H)
print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 3. usage (before) ===")
st, res = req("/usage", token)
print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 4. records (before) ===")
st, res = req("/records", token)
print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 5. solve (post fake jpeg) ===")
fake_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 200 + b"\xff\xd9"
SH = dict(H); SH["Idempotency-Key"] = "probe-solve-%d" % int(time.time())
st, res = req("/solve", token, raw=fake_jpeg, headers=SH)
print(st, json.dumps(res, ensure_ascii=False)[:400])
job_id = res.get("job_id") if isinstance(res, dict) else None

if job_id:
    print("\n=== 6. poll solve ===")
    for i in range(20):
        time.sleep(0.5)
        st, r2 = req("/solve/" + str(job_id), token, headers=H)
        if isinstance(r2, dict) and r2.get("status") in ("done", "completed", "finished", "success"):
            print("poll done at", i, json.dumps(r2, ensure_ascii=False)[:400]); break
        if i in (0, 5, 10, 19):
            print("poll", i, st, json.dumps(r2, ensure_ascii=False)[:200])
    print("\n=== 7. cards/deduct ===")
    DH = dict(H); DH["Idempotency-Key"] = "probe-deduct-%s" % str(job_id)
    st, res = req("/cards/deduct", token, body={"job_id": str(job_id), "question_index": 0}, headers=DH)
    print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 7b. cards/check (after deduct) ===")
st, res = req("/cards/check", token, body={"question_id": "probe-1"}, headers=H)
print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 8. usage (after) ===")
st, res = req("/usage", token)
print(st, json.dumps(res, ensure_ascii=False)[:300])

print("\n=== 9. records (after) ===")
st, res = req("/records", token)
print(st, json.dumps(res, ensure_ascii=False)[:300])
