"""Top-level share-card identity; no fetching, redirects, or title matching."""
import json
import hashlib
import re
import xml.etree.ElementTree as ET
from urllib.parse import parse_qs, urlencode, urlsplit, urlunsplit


def identity(url):
    if not isinstance(url, str) or not url or len(url) > 4096:
        return None
    if any(ord(c) < 33 for c in url) or "\\" in url:
        return None
    try:
        parsed = urlsplit(url)
        host = parsed.hostname
        if parsed.scheme not in ("http", "https") or not host or parsed.username or parsed.password or parsed.port:
            return None
        query = parse_qs(parsed.query, keep_blank_values=True)
    except ValueError:
        return None
    if host in ("www.bilibili.com", "m.bilibili.com", "bilibili.com"):
        match = re.fullmatch(r"/video/(BV[0-9A-Za-z]{10}|av[0-9]+)/?", parsed.path)
        if match:
            # Different pages in a multipart video share the same video identity.
            return "share:bilibili-video:v1", match[1]
    if host == "mp.weixin.qq.com":
        match = re.fullmatch(r"/s/([A-Za-z0-9_-]+)/?", parsed.path)
        if match:
            return "share:wechat-short:v1", match[1]
        keys = ("__biz", "mid", "idx", "sn")
        if parsed.path in ("/s", "/s/") and all(len(query.get(k, [])) == 1 and query[k][0] for k in keys):
            return "share:wechat-article:v1", urlencode([(k, query[k][0]) for k in keys])
        return None
    if host in ("music.163.com", "m.music.163.com", "y.music.163.com"):
        # NetEase's desktop site uses hash routes; preserve their semantic query.
        route = urlsplit(parsed.fragment) if parsed.fragment.startswith("/") else parsed
        params = parse_qs(route.query, keep_blank_values=True)
        match = re.fullmatch(r"/(?:m/)?(song|album|playlist)/?", route.path)
        ids = params.get("id", [])
        if match and len(ids) == 1 and re.fullmatch(r"[0-9]{1,20}", ids[0]):
            return "share:netease-" + match[1] + ":v1", str(int(ids[0]))
        return None
    # Unknown providers and short links require the same URL. Keep all query data
    # because it may identify content; only fragments and host case differ.
    normalized = urlunsplit((parsed.scheme, host, parsed.path or "/", parsed.query, ""))
    return "share:url:v1", normalized


def card_identity(segment):
    if not isinstance(segment, dict):
        return None
    kind = segment.get("type")
    data = segment.get("data")
    payload = data.get("data") if isinstance(data, dict) else data
    urls = []
    if kind == "json":
        if isinstance(payload, str):
            if len(payload) > 65536:
                return None
            try:
                payload = json.loads(payload)
            except (ValueError, RecursionError):
                return None
        if not isinstance(payload, dict):
            return None
        app = payload.get("app")
        meta = payload.get("meta")
        if not isinstance(meta, dict):
            return None
        if app in ("com.tencent.structmsg", "com.tencent.tuwen.lua", "com.tencent.music.lua"):
            news = meta.get("news")
            music = meta.get("music")
            if isinstance(news, dict):
                urls = [news.get("jumpUrl")]
            elif isinstance(music, dict):
                urls = [music.get("jumpUrl")]
        elif app == "com.tencent.miniapp_01":
            detail = meta.get("detail_1")
            if isinstance(detail, dict):
                urls = [detail.get("qqdocurl")]
    elif kind == "xml":
        if not isinstance(payload, str) or len(payload) > 65536:
            return None
        if "<!DOCTYPE" in payload.upper() or "<!ENTITY" in payload.upper():
            return None
        try:
            root = ET.fromstring(payload)
        except (ET.ParseError, ValueError, RecursionError):
            return None
        if root.tag == "msg" and root.get("serviceID") in ("1", "2"):
            urls = [root.get("url")]
    for url in urls:
        result = identity(url)
        if result:
            return result
    return None


def check_cards(detector, segments, *, scope, message_id):
    results = []
    if not isinstance(segments, list):
        return results
    for part, segment in enumerate(segments[:32]):
        item = card_identity(segment)
        if item:
            namespace, resource = item
            digest = hashlib.sha256(resource.encode("utf-8")).hexdigest()
            results.append(detector.forward_native.ingest(scope, message_id, part, namespace, digest, ""))
    return results


def format_card_hit(result):
    if result.status == "same_resource" and not result.replayed:
        return f"这张分享卡片的目标链接之前出现过，首次消息：{result.first_message_id}。"
    return None
