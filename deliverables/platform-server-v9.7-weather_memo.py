"""weather_memo.py — 锁屏后台小模块：温度 + MEMO（PRD v5.7 / 方案设计定稿 v2）.

依据：PRD v5.7 §5.1、《温度-MEMO-方案设计.md》定稿 v2（2026-11-24 21:00，
NYX 189/192 实测 Open-Meteo 可用、浩爷 188 定 MEMO 答案之书、NYX 186 定缓存 TTL）。

职责（独立小模块，不混主业务）：
1. IP → 城市：本地 IP 库优先（ip_city.json，可选文件，查不到走在线后备）；
   私网/回环地址直接返回空（无法定位，不编）。缓存 24 小时。
2. 城市 → 温度：Open-Meteo 两步（地名→经纬度，再取当前温度），免费不用 Key。
   缓存按城市 30 分钟。NYX 已实测两步都通（北京 2.3°C）。
3. MEMO：weather_memo.json 句子集（答案之书，174 句，浩爷/NYX 定稿）随机取一句。
4. 失败一律返空值：城市 ""、temperature null、memo ""。绝不编数字、绝不抛错阻塞锁屏。

隐私（方案设计 §7）：只用来源 IP 解析城市，不落盘原始 IP、日志不记原始 IP。
测试钩子（方案设计 §8）：环境变量 JIANTI_LOCKSCREEN_CITY 指定城市，
脱离真实 IP 与天气定位即可验证（测试模式专用）。

标准库 only。
"""
from __future__ import annotations

import json
import os
import random
import time
import urllib.parse
import urllib.request

GEO_URL = "https://geocoding-api.open-meteo.com/v1/search"
WX_URL = "https://api.open-meteo.com/v1/forecast"
IP_FALLBACK_URL = "http://ip-api.com/json"          # 在线后备（本地库查不到时）
HTTP_TIMEOUT = 5                                     # 秒；锁屏绝不因等温度卡死
CACHE_WEATHER_TTL = 30 * 60                          # 城市→温度，30 分钟
CACHE_IP_TTL = 24 * 3600                             # IP→城市，24 小时

_HERE = os.path.dirname(os.path.abspath(__file__))
MEMO_FILE = os.path.join(_HERE, "weather_memo.json")
IP_CITY_FILE = os.path.join(_HERE, "ip_city.json")   # 本地 IP 库（可选文件）

_weather_cache: dict = {}   # city -> (expire_ts, temperature_int_or_None)
_condition_cache: dict = {} # city -> (expire_ts, condition_str)  D 任务：天气状况（浩爷 852/NYX 860）
_ip_cache: dict = {}        # ip   -> (expire_ts, city_str)
_memo_sentences: list | None = None

# D 任务（PRD v5.11 / 浩爷 852）：Open-Meteo WMO weather_code -> 中文天气状况，
# ≤5 字；未知/取不到返空串。与温度同一次 Open-Meteo 请求取回，不额外打接口。
WEATHER_CODE_MAP = {
    0: "晴", 1: "晴", 2: "多云", 3: "阴",
    45: "雾", 48: "雾",
    51: "小雨", 53: "小雨", 55: "小雨", 56: "小雨", 57: "小雨",
    61: "小雨", 63: "中雨", 65: "大雨", 66: "大雨", 67: "大雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "小雪",
    80: "小雨", 81: "中雨", 82: "大雨", 85: "小雪", 86: "大雪",
    95: "雷阵雨", 96: "雷阵雨", 99: "雷阵雨",
}


def weathercode_to_condition(code) -> str:
    """WMO weather_code -> 中文天气状况（≤5字）；未知/空返空串，绝不编。"""
    try:
        return WEATHER_CODE_MAP.get(int(code), "")
    except (TypeError, ValueError):
        return ""


def _load_sentences() -> list:
    global _memo_sentences
    if _memo_sentences is None:
        try:
            with open(MEMO_FILE, encoding="utf-8") as f:
                data = json.load(f)
            s = data.get("sentences", [])
            _memo_sentences = [x for x in s if isinstance(x, str) and x.strip()]
        except Exception:
            _memo_sentences = []          # 句子集缺失 -> 恒空串，不抛错
    return _memo_sentences


def random_memo() -> str:
    """答案之书句子集随机取一句；集为空返回空串。"""
    s = _load_sentences()
    return random.choice(s) if s else ""


def _is_private_ip(ip: str) -> bool:
    if not ip:
        return True
    if ip in ("127.0.0.1", "::1", "0.0.0.0"):
        return True
    if ip.startswith(":"):                # IPv6 本机
        return True
    parts = ip.split(".")
    if len(parts) != 4:
        return True                       # 非 IPv4 点分 -> 不当公网处理
    try:
        a, b = int(parts[0]), int(parts[1])
    except ValueError:
        return True
    if a == 10 or a == 127:
        return True
    if a == 172 and 16 <= b <= 31:
        return True
    if a == 192 and b == 168:
        return True
    if a == 169 and b == 254:             # 链路本地
        return True
    if a == 100 and 64 <= b <= 127:       # CGNAT
        return True
    return False


def _local_ip_lookup(ip: str) -> str:
    """本地 IP 库 ip_city.json（可选）：{"1.2.3.0/24": "城市", ...} 前缀匹配。"""
    try:
        with open(IP_CITY_FILE, encoding="utf-8") as f:
            table = json.load(f)
    except Exception:
        return ""
    best = ""
    best_len = -1
    for prefix, city in table.items():
        if not isinstance(city, str) or not city:
            continue
        p = prefix.split("/")[0].strip()
        pl = len(p.split("."))
        if ip.startswith(p + ".") and pl > best_len:
            best, best_len = city, pl
    return best


def _online_ip_lookup(ip: str) -> str:
    """在线后备（本地库查不到才走）：ip-api.com 免费口，不记 IP 不落盘。"""
    try:
        url = IP_FALLBACK_URL + "?" + urllib.parse.urlencode({"ip": ip, "lang": "zh-CN"})
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT) as r:
            data = json.loads(r.read().decode("utf-8"))
        city = data.get("city") or ""
        return city if isinstance(city, str) else ""
    except Exception:
        return ""


def ip_to_city(ip: str) -> str:
    """来源 IP → 城市。私网/查不到返回 ""。缓存 24h。不落盘、不记日志。"""
    if not ip or _is_private_ip(ip):
        return ""
    now = time.time()
    hit = _ip_cache.get(ip)
    if hit and hit[0] > now:
        return hit[1]
    city = _local_ip_lookup(ip) or _online_ip_lookup(ip)
    _ip_cache[ip] = (now + CACHE_IP_TTL, city)
    return city


def warm_city_for_ip(ip: str) -> str:
    """登录／注册时预热（547 ABB 部分，NYX 528／529 定稿）：按本次连接源 IP
    查一次城市，记进 24 小时内存缓存；后面 /lockscreen 同一出口 IP 直接
    命中缓存。内网 IP 查不到城市返空串，不写有效城市。永不抛异常。"""
    try:
        return ip_to_city(ip)
    except Exception:
        return ""


def _geo_city_coords(city: str):
    """Open-Meteo 地名接口：城市 → (lat, lon)；失败 None。不用 Key。"""
    url = GEO_URL + "?" + urllib.parse.urlencode(
        {"name": city, "count": 1, "language": "zh", "format": "json"})
    with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT) as r:
        data = json.loads(r.read().decode("utf-8"))
    res = (data.get("results") or [])
    if not res:
        return None
    it = res[0]
    lat, lon = it.get("latitude"), it.get("longitude")
    if lat is None or lon is None:
        return None
    return lat, lon


def _wx_weather(lat, lon):
    """Open-Meteo 天气接口：当前 (温度整数摄氏度, weather_code)；失败 (None, None)。
    D 任务：温度与天气状况同一次请求取回，不额外打接口。不用 Key。"""
    url = WX_URL + "?" + urllib.parse.urlencode(
        {"latitude": lat, "longitude": lon,
         "current": "temperature_2m,weather_code",
         "timezone": "Asia/Shanghai"})
    with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT) as r:
        data = json.loads(r.read().decode("utf-8"))
    cur = data.get("current") or {}
    t = cur.get("temperature_2m")
    temperature = None if t is None else int(round(float(t)))
    return temperature, cur.get("weather_code")


def city_temperature(city: str):
    """城市 → 当前温度（int 摄氏度）或 None。成功缓存 30 分钟；失败只缓存 60 秒，
    避免一次网络抖动把 null 钉死半小时，也不会失败时狂打接口。
    D 任务：顺带把天气状况 condition 写进同 TTL 的缓存，供 city_condition 读。"""
    if not city:
        return None
    now = time.time()
    hit = _weather_cache.get(city)
    if hit and hit[0] > now:
        return hit[1]
    temperature = None
    code = None
    try:
        coords = _geo_city_coords(city)
        if coords:
            temperature, code = _wx_weather(*coords)
    except Exception:
        temperature, code = None, None    # 查不到/接口失败 -> null，不编数字
    ttl = CACHE_WEATHER_TTL if temperature is not None else 60
    _weather_cache[city] = (now + ttl, temperature)
    _condition_cache[city] = (now + ttl, weathercode_to_condition(code))
    return temperature


def city_condition(city: str) -> str:
    """城市 → 天气状况中文（≤5字）或空串。与温度共用缓存，取不到返空不阻塞。"""
    if not city:
        return ""
    now = time.time()
    hit = _condition_cache.get(city)
    if hit and hit[0] > now:
        return hit[1]
    # 缓存未命中但温度缓存有效：说明是旧进程/旧缓存，无 code -> 空串
    return ""


def lockscreen_payload(ip: str, device_id: str | None = None) -> dict:
    """GET /lockscreen 主体。失败一律空值：{"city":"","temperature":null,"memo":""}。

    device_id 仅作设备标识（方案设计 §5：温度按来源 IP、MEMO 随机，与设备无关）。
    测试钩子：JIANTI_LOCKSCREEN_CITY 指定城市即按该城市取温度（方案设计 §8）。
    """
    city = ""
    try:
        forced = os.environ.get("JIANTI_LOCKSCREEN_CITY", "").strip()
        if forced:
            city = forced                       # 测试模式：指定城市，脱离真实 IP
        else:
            city = ip_to_city(ip)
    except Exception:
        city = ""
    temperature = None
    condition = ""
    memo = ""
    try:
        temperature = city_temperature(city)   # 顺带填 _condition_cache
    except Exception:
        temperature = None
    try:
        condition = city_condition(city)        # D 任务：天气状况，取不到空串不阻塞
    except Exception:
        condition = ""
    try:
        memo = random_memo()
    except Exception:
        memo = ""
    return {"city": city or "", "temperature": temperature,
            "condition": condition, "memo": memo}


def usage_style(ip: str) -> dict:
    """/usage 沿用现口径（temperature 字符串 "---°C"/"N°C"，memo 空串兜底）。"""
    payload = lockscreen_payload(ip)
    t = payload.get("temperature")
    temperature = ("---°C" if t is None else "%d°C" % t)
    return {"temperature": temperature, "memo": payload.get("memo") or ""}
