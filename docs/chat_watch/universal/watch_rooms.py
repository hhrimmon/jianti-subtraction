# -*- coding: utf-8 -*-
"""通用聊天室值守探测器 —— agent 中立，配置驱动，一个脚本管所有房间。

任何 agent 都能用：把配置文件里的 identity 改成自己的名字/别名/叫法即可，
脚本和调度任务都不用动。换 agent = 只改配置，不碰代码。

用法：
    python watch_rooms.py                    # 默认读同目录 rooms.json
    python watch_rooms.py --config my.json   # 指定配置（别的 agent 用各自的）
    set WATCH_CONFIG=D:\\path\\my.json        # 或用环境变量指定

纯 IO：服务端 cursor 推进 + 401 自动重登 + 自消息过滤 + @过滤 + 本地 mention 兜底。
输出只有两种：
    SLEEP  -> 本轮没我的事，直接结束（0 token，不叫模型、不发消息、不轮询）
    HITS   -> 打印命中消息，执行器理解后干活/回话（cursor 已推进，无需清理队列）

设计要点：
- 身份（名字/别名/叫法）与房间全部来自配置，脚本不写死任何 agent、端口、密码、关键词。
- 每个配置文件有独立的 state 目录（与配置文件同级的 state/），多个 agent 各用各的，游标互不干扰。
- 先走服务端 @过滤；cursor 推进但过滤为空时，用 ?all=1 再做一次本地 mention 兜底，
  防「xxx 613 确认」这类不带 @ 的叫名被服务端过滤漏掉。
- 永远先剔掉自己（含所有别名）发的消息，否则自己回执里的名字会被反复匹配、每轮白烧。
- 冷启动：游标文件不存在时只把游标推到当前最大值当基线，不重放历史。
"""
import datetime
import http.cookiejar
import json
import os
import re
import sys
import urllib.error
import urllib.request


def resolve_config_path(argv):
    """配置路径优先级：--config 参数 > WATCH_CONFIG 环境变量 > 同目录 rooms.json。"""
    if "--config" in argv:
        i = argv.index("--config")
        if i + 1 < len(argv):
            return os.path.abspath(argv[i + 1])
    env = os.environ.get("WATCH_CONFIG", "").strip()
    if env:
        return os.path.abspath(env)
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "rooms.json")


CFG_PATH = resolve_config_path(sys.argv[1:])
STATE_DIR = os.path.join(os.path.dirname(CFG_PATH), "state")
if not os.path.isdir(STATE_DIR):
    os.makedirs(STATE_DIR)


def load_cfg():
    with open(CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def get_identity(cfg):
    """从配置取本 agent 身份。identity 块优先，兼容旧的 self_name/self_aliases。"""
    ident = cfg.get("identity", {}) or {}
    name = str(ident.get("name") or cfg.get("self_name") or "").strip()
    aliases = list(ident.get("aliases") or cfg.get("self_aliases") or [])
    mention = list(ident.get("mention") or [])

    aliases_l = {a.strip().lower() for a in aliases if str(a).strip()}
    if name:
        aliases_l.add(name.lower())
    # 没显式给 mention 时，从名字/别名自动生成叫法（@名字 和 名字）
    if not mention:
        for a in aliases_l:
            mention.append("@" + a)
            mention.append(a)
    mention = [str(m).strip().lower().lstrip("@") for m in mention if str(m).strip()]
    mention = [m for m in mention if m]
    return name, aliases_l, build_mention(mention)


def make_opener():
    jar = http.cookiejar.CookieJar()
    # 绕过系统代理，直连内网/公网地址
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(jar))


def post_json(op, base, path, obj):
    req = urllib.request.Request(
        base + path,
        data=json.dumps(obj).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    return json.loads(op.open(req, timeout=15).read().decode("utf-8"))


def build_mention(keywords):
    """把叫法列表编译成正则：匹配 @名字 / 名字，允许不写 @；英文单词边界防误命中。"""
    parts = []
    for k in keywords:
        k = str(k).strip().lower().lstrip("@")
        if k:
            parts.append(re.escape(k))
    if not parts:
        return None
    # 长词优先，避免 'tra' 抢先匹配 'trae'
    parts.sort(key=len, reverse=True)
    return re.compile(r"(?<![A-Za-z0-9_])@?\s*(?:%s)(?![A-Za-z0-9_])" % "|".join(parts), re.I)


def cursor_path(key):
    return os.path.join(STATE_DIR, key + ".cursor")


def read_cursor(key):
    p = cursor_path(key)
    if not os.path.exists(p):
        return None, True
    txt = open(p, encoding="utf-8").read().strip()
    if not txt:
        return 0, False
    try:                       # 纯整数（首选）
        return int(txt), False
    except ValueError:         # 兼容旧 JSON 整数
        try:
            return int(json.loads(txt)), False
        except Exception:
            return 0, False


def write_cursor(key, val):
    with open(cursor_path(key), "w", encoding="utf-8") as f:
        f.write(str(int(val)))


def probe_room(room, my_name, aliases, mention):
    """探测单房，返回 (hits, cursor, cold, note)。异常在本函数内消化。"""
    key = room["key"]
    base = room["base"]
    cur, cold = read_cursor(key)
    if cold:
        cur = 0

    op = make_opener()
    need_login = room.get("need_login", True)

    def login():
        return post_json(op, base, "/api/login",
                         {"password": room["password"], "name": my_name})

    def fetch(after, all_=False):
        path = "/api/messages?after=%d&limit=100" % after
        if all_:
            path += "&all=1"
        d = json.loads(op.open(base + path, timeout=15).read().decode("utf-8"))
        if isinstance(d, dict):
            return d.get("messages", []), d.get("cursor")
        return d, None

    def fetch_head():
        """首屏请求(不带 after)的服务端 cursor 恒等于全库 head，用作冷启动基线。"""
        d = json.loads(op.open(base + "/api/messages?limit=1", timeout=15).read().decode("utf-8"))
        if isinstance(d, dict) and d.get("cursor") is not None:
            return int(d["cursor"])
        return None

    if need_login:
        try:
            login()
        except Exception:
            return [], cur, cold, "login failed"

    # 冷启动：基线必须推到全库 head，不能用 after=0 那页的 cursor(只是扫描位置)，
    # 否则下一轮会从 head 之前继续拉、把历史一页页重放成 HITS。
    if cold:
        try:
            head = fetch_head()
        except urllib.error.HTTPError as e:
            if e.code == 401 and need_login:
                try:
                    login()
                    head = fetch_head()
                except Exception as ex:
                    return [], cur, cold, "cold relogin failed %r" % ex
            else:
                return [], cur, cold, "cold head HTTP %s" % e.code
        except Exception as e:
            return [], cur, cold, "cold head error %r" % e
        if head is None:
            return [], cur, cold, "cold head unavailable"
        write_cursor(key, head)
        return [], head, True, "cold baseline head=%d" % head

    try:
        msgs, cur_srv = fetch(cur)
    except urllib.error.HTTPError as e:
        if e.code == 401 and need_login:
            try:
                login()
                msgs, cur_srv = fetch(cur)
            except Exception as ex:
                return [], cur, cold, "relogin failed %r" % ex
        else:
            return [], cur, cold, "HTTP %s" % e.code
    except Exception as e:
        return [], cur, cold, "fetch error %r" % e

    # cursor 推进：优先服务端 cursor，否则取消息最大 id
    if cur_srv is not None:
        nxt = int(cur_srv)
    elif msgs:
        nxt = max(int(m["id"]) for m in msgs)
    else:
        nxt = cur

    write_cursor(key, nxt)

    # 服务端已过滤；先剔掉自己及所有别名
    msgs = [m for m in msgs if str(m.get("name", "")).strip().lower() not in aliases]

    if msgs:
        return msgs, nxt, False, ""

    # cursor 推进但过滤为空 -> 本地 mention 兜底（防不带 @ 的叫名被漏）
    if nxt != cur and mention:
        try:
            delta, _ = fetch(cur, all_=True)
        except Exception:
            return [], nxt, False, "fallback fetch failed"
        delta = [m for m in delta if str(m.get("name", "")).strip().lower() not in aliases]
        hits = [m for m in delta if mention.search(str(m.get("text") or ""))]
        if hits:
            return hits, nxt, False, "mention fallback"
    return [], nxt, False, ""


def main():
    cfg = load_cfg()
    my_name, aliases, mention = get_identity(cfg)
    rooms = cfg.get("rooms", [])
    all_hits = []
    sleep_lines = []

    for room in rooms:
        hits, cur, cold, note = probe_room(room, my_name, aliases, mention)
        if cold:
            sleep_lines.append("%s %s" % (room["key"], note))
            continue
        if hits:
            for m in hits:
                all_hits.append({
                    "room": room["key"],
                    "label": room.get("label", room["key"]),
                    "id": m.get("id"),
                    "name": m.get("name"),
                    "text": str(m.get("text") or ""),
                    "ts": m.get("ts"),
                })
        else:
            sleep_lines.append("%s SLEEP cursor=%d%s" % (
                room["key"], cur, (" (" + note + ")") if note else ""))

    tag = "[agent=%s]" % (my_name or "?")
    if not all_hits:
        print("%s SLEEP | %s | 0 token" % (tag, " ; ".join(sleep_lines)))
        return

    print("%s HITS=%d" % (tag, len(all_hits)))
    for h in all_hits:
        print("=== room=%s id=%s who=%s ===" % (h["room"], h["id"], h["name"]))
        print(h["text"])
        print()


if __name__ == "__main__":
    main()
