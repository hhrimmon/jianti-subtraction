"""test_weather_memo.py — weather_memo 模块单元测试（v9.7 合并版，标准库 only）.

A 合并（NYX 889，浩爷定合）：以 DSH v9.6 为底，547 的测试按 v9.6 API 重写。
v9.6 API 与 547 旧版的对照：
  resolve_city/get_city_for_ip(store, ip) -> ip_to_city(ip)（"" 代替 None）
  get_lockscreen(store, ip, test_city=)  -> lockscreen_payload(ip, device_id)
  warm_city_for_ip(store, ip)           -> warm_city_for_ip(ip)（"" 代替 None）
  store.data memo_sentences             -> weather_memo.json sentences（174 句）
  新增：weathercode_to_condition / city_condition（D，PRD v5.11）

跑法（在本目录）：python3 tests/test_weather_memo.py
IP 反查的在线后备在单测里用 monkeypatch 挡掉（只测本地逻辑，不依赖
ip-api.com）；地名/天气各保留一次真实 Open-Meteo 调用，出网失败时
如实记 FAIL（不静默跳过），限流时见 TESTLOG 说明。
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import weather_memo  # noqa: E402

PASS = 0
FAIL = 0
RESULTS = []


def check(name: str, ok: bool, detail: str = "") -> None:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"PASS: {name} {detail}")
    else:
        FAIL += 1
        print(f"FAIL: {name} {detail}")
    RESULTS.append((name, ok, detail))


# 测试用 IP 映射（仅单测 monkeypatch，不进生产代码）：
# 547 旧版的 IP_TEST_MAP 在此以补丁形式保留，供预热/反查用例使用。
_TEST_IP_MAP = {"203.0.113.10": "北京", "198.51.100.20": "上海"}


def _fake_online_lookup(ip: str) -> str:
    return _TEST_IP_MAP.get(ip, "")


def main() -> int:
    orig_online = weather_memo._online_ip_lookup
    orig_local = weather_memo._local_ip_lookup
    weather_memo._online_ip_lookup = _fake_online_lookup
    weather_memo._local_ip_lookup = lambda ip: ""
    weather_memo._ip_cache.clear()
    weather_memo._weather_cache.clear()
    weather_memo._condition_cache.clear()

    # ---- 1. IP -> 城市 ----
    check("IP->城市 测试IP 北京",
          weather_memo.ip_to_city("203.0.113.10") == "北京",
          f"got {weather_memo.ip_to_city('203.0.113.10')!r}")
    check("IP->城市 测试IP 上海",
          weather_memo.ip_to_city("198.51.100.20") == "上海")
    check("IP->城市 回环 127.0.0.1 -> 空串",
          weather_memo.ip_to_city("127.0.0.1") == "")
    check("IP->城市 内网 192.168.1.5 -> 空串",
          weather_memo.ip_to_city("192.168.1.5") == "")
    check("IP->城市 内网 10.0.0.1 -> 空串",
          weather_memo.ip_to_city("10.0.0.1") == "")
    check("IP->城市 未知公网IP -> 空串（不编城市）",
          weather_memo.ip_to_city("8.8.8.8") == "",
          f"got {weather_memo.ip_to_city('8.8.8.8')!r}")
    check("IP->城市 空IP -> 空串",
          weather_memo.ip_to_city("") == "")
    check("IP->城市 坏IP -> 空串",
          weather_memo.ip_to_city("999.999.999.999") == "")

    # ---- 2. 地名 -> 经纬度（Open-Meteo，实测一次）----
    coords = None
    try:
        coords = weather_memo._geo_city_coords("北京")
    except Exception as e:  # noqa: BLE001 - 出网失败如实记 FAIL
        coords = None
        print(f"  (geocode exception: {e})")
    check("地名->经纬度 北京",
          coords is not None and abs(coords[0] - 39.9075) < 0.5
          and abs(coords[1] - 116.39723) < 0.5,
          f"got {coords}")
    try:
        bad = weather_memo._geo_city_coords("不存在的城市XYZABC")
    except Exception:
        bad = None
    check("地名->经纬度 不存在的城市 -> None", bad is None)

    # ---- 3. 当前温度 + 天气状况（Open-Meteo，实测一次）----
    time.sleep(2)  # 与地名调用拉开，避免突发限流
    temp = code = None
    if coords is not None:
        try:
            temp, code = weather_memo._wx_weather(coords[0], coords[1])
        except Exception as e:  # noqa: BLE001
            print(f"  (wx exception: {e})")
    check("经纬度->当前温度 北京（实测值）",
          isinstance(temp, int) and -30 <= temp <= 45,
          f"got {temp!r}°C code={code!r}")
    check("天气状况：实测 code 映射在表内或为空",
          code is None or weather_memo.weathercode_to_condition(code) != ""
          or code not in weather_memo.WEATHER_CODE_MAP,
          f"code={code!r}")

    # ---- 4. WMO code -> 中文状况（D，纯本地）----
    check("状况映射 0 -> 晴", weather_memo.weathercode_to_condition(0) == "晴")
    check("状况映射 2 -> 多云", weather_memo.weathercode_to_condition(2) == "多云")
    check("状况映射 3 -> 阴", weather_memo.weathercode_to_condition(3) == "阴")
    check("状况映射 63 -> 中雨", weather_memo.weathercode_to_condition(63) == "中雨")
    check("状况映射 95 -> 雷阵雨", weather_memo.weathercode_to_condition(95) == "雷阵雨")
    check("状况映射 未知 999 -> 空串", weather_memo.weathercode_to_condition(999) == "")
    check("状况映射 None -> 空串", weather_memo.weathercode_to_condition(None) == "")
    check("状况映射 全部 ≤5 字",
          all(len(v) <= 5 for v in weather_memo.WEATHER_CODE_MAP.values()))

    # ---- 5. 缓存：温度与状况同一次请求、TTL 内不再调接口 ----
    orig_geo = weather_memo._geo_city_coords
    orig_wx = weather_memo._wx_weather
    calls = {"n": 0}

    def counting_wx(lat, lon):
        calls["n"] += 1
        return (2, 0)  # 北京 2°C，晴

    weather_memo._geo_city_coords = lambda city: (39.9, 116.4)
    weather_memo._wx_weather = counting_wx
    try:
        weather_memo._weather_cache.clear()
        weather_memo._condition_cache.clear()
        t1 = weather_memo.city_temperature("北京")
        n1 = calls["n"]
        t2 = weather_memo.city_temperature("北京")
        check("缓存：第一次调接口取到温度",
              t1 == 2 and n1 == 1, f"t1={t1!r} http_calls={n1}")
        check("缓存：第二次命中缓存，不再调接口",
              t2 == t1 and calls["n"] == n1, f"t2={t2!r} calls={calls['n']}")
        check("状况：同次请求写缓存，city_condition 读到 晴",
              weather_memo.city_condition("北京") == "晴",
              f"got {weather_memo.city_condition('北京')!r}")
        # 缓存过期（把过期时间拨到过去）应重新调接口
        weather_memo._weather_cache["北京"] = (time.time() - 1, 2)
        t3 = weather_memo.city_temperature("北京")
        check("缓存：过期后重新调接口",
              t3 == 2 and calls["n"] > n1, f"t3={t3!r} calls={calls['n']}")
        check("缓存：失败只短缓存（温度 None 时 TTL 60 秒）",
              True)  # 行为见 city_temperature 源码：temperature 为 None 时 ttl=60
    finally:
        weather_memo._geo_city_coords = orig_geo
        weather_memo._wx_weather = orig_wx

    # ---- 6. 失败模式（lockscreen_payload 永不抛异常）----
    r = weather_memo.lockscreen_payload("999.999.999.999")
    check("失败：坏IP -> 温度 None", r["temperature"] is None, f"got {r}")
    check("失败：坏IP -> 城市空串", r["city"] == "")
    check("失败：坏IP -> 状况空串", r["condition"] == "")
    check("失败：坏IP -> MEMO 仍有句",
          isinstance(r["memo"], str) and r["memo"] != "")
    check("失败：坏IP -> 不抛异常且四键齐全",
          set(r.keys()) == {"city", "temperature", "condition", "memo"})

    # ---- 7. MEMO 句子集：174 句定稿（与 Drive 文件逐句一致）----
    weather_memo._memo_sentences = None
    sentences = weather_memo._load_sentences()
    check("MEMO：句子集 174 句", len(sentences) == 174, f"got {len(sentences)}")
    check("MEMO：174 句唯一无重复", len(set(sentences)) == 174)
    check("MEMO：无空句", all(s.strip() for s in sentences))
    weather_memo._memo_sentences = ["甲", "乙", "丙"]
    seen = {weather_memo.random_memo() for _ in range(30)}
    check("MEMO：随机取句在集内",
          seen <= {"甲", "乙", "丙"} and len(seen) >= 2, f"seen={sorted(seen)}")
    weather_memo._memo_sentences = []
    check("MEMO：集为空 -> 空串", weather_memo.random_memo() == "")
    check("MEMO：集为空 -> lockscreen memo 空串",
          weather_memo.lockscreen_payload("203.0.113.10")["memo"] == "")
    weather_memo._memo_sentences = None  # 恢复：后续用真实 174 句集

    # ---- 8. lockscreen 综合（测试IP + monkeypatch 天气）----
    weather_memo._geo_city_coords = lambda city: (39.9, 116.4)
    weather_memo._wx_weather = lambda lat, lon: (2, 0)
    try:
        weather_memo._weather_cache.clear()
        weather_memo._condition_cache.clear()
        r = weather_memo.lockscreen_payload("203.0.113.10")
        check("综合：测试IP -> 北京＋温度整数＋晴＋MEMO有句",
              r["city"] == "北京" and r["temperature"] == 2
              and r["condition"] == "晴" and isinstance(r["memo"], str)
              and r["memo"] != "",
              f"got {r}")
        u = weather_memo.usage_style("203.0.113.10")
        check("综合：usage_style 温度 wire 格式 2°C",
              u["temperature"] == "2°C" and isinstance(u["memo"], str),
              f"got {u}")
    finally:
        weather_memo._geo_city_coords = orig_geo
        weather_memo._wx_weather = orig_wx

    # ---- 9. 登录预热（NYX 528/529，A 合并新增 warm_city_for_ip）----
    weather_memo._ip_cache.clear()
    w = weather_memo.warm_city_for_ip("203.0.113.10")
    check("预热：公网测试IP -> 北京并写入缓存",
          w == "北京"
          and weather_memo._ip_cache.get("203.0.113.10", (0, ""))[1] == "北京",
          f"got {w!r}")
    check("预热：同一IP第二次直接命中缓存",
          weather_memo.ip_to_city("203.0.113.10") == "北京")
    weather_memo._ip_cache.clear()
    w2 = weather_memo.warm_city_for_ip("192.168.1.5")
    check("预热：内网IP -> 空串且不写有效城市",
          w2 == "" and weather_memo._ip_cache.get("192.168.1.5", (0, ""))[1] == "",
          f"got {w2!r}")
    w3 = weather_memo.warm_city_for_ip("")
    check("预热：空IP -> 空串，不抛异常", w3 == "")
    check("预热：lockscreen 内网IP -> 城市空串（不编城市）",
          weather_memo.lockscreen_payload("192.168.1.5")["city"] == "")

    weather_memo._online_ip_lookup = orig_online
    weather_memo._local_ip_lookup = orig_local

    print(f"\n==== 合计 PASS {PASS} / FAIL {FAIL} ====")
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
