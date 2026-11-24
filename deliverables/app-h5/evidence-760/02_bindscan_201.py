# -*- coding: utf-8 -*-
"""扫码绑定端到端 201 证据：注册隔离账号 -> 登录 -> admin verify -> bind-scan。
   输出仅含状态码与设备号/绑定方式，不含 token/口令。"""
import urllib.request, json, time, random

B = "http://127.0.0.1:8000"
PHONE = "139" + str(int(time.time()))[-8:]
PWD = "Test@" + PHONE[-4:]
ADMIN = "%06d" % random.randint(100000, 999999)
CODE = "%04d" % random.randint(1000, 9999)

def req(p, m="GET", d=None, t=None):
    h = {"Content-Type": "application/json"}
    if t:
        h["Authorization"] = "Bearer " + t
    r = urllib.request.Request(B + p, data=json.dumps(d).encode() if d is not None else None, headers=h, method=m)
    try:
        x = urllib.request.urlopen(r, timeout=8)
        return x.status, json.loads(x.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")

s, d = req("/auth/register", "POST", {"phone": PHONE, "password": PWD, "admin_password": ADMIN, "code": CODE})
print("REGISTER", s, "phone_tail", PHONE[-4:])
s, d = req("/auth/login", "POST", {"phone": PHONE, "password": PWD})
t = d.get("token")
print("LOGIN", s, "has_token", bool(t))
s, v = req("/auth/admin-password/verify", "POST", {"admin_password": ADMIN}, t)
print("ADMIN_VERIFY", s, "verified", v.get("verified"))
for payload in ["JianTi-4000000000000201", "JianTi-4000000000000202"]:
    s, r = req("/terminals/bind-scan", "POST", {"qr_payload": payload}, t)
    print("BIND_SCAN", payload, s, "device_id=" + str(r.get("device_id")), "bind_method=" + str(r.get("bind_method")))
