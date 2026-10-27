"""只看最外层 forward / com.tencent.multimsg 卡片，不访问节点或下载整包。"""
import json
from dataclasses import dataclass


@dataclass(frozen=True)
class Envelope:
    namespace: str
    resource_id: str
    visible_card: str = ""


def _string(value, limit=4096):
    return value.strip()[:limit] if isinstance(value, str) else ""


def _resource_id(value):
    # 不截断不透明资源 ID：截断可能把两个不同资源变成同一个标识。
    return value if isinstance(value, str) and 0 < len(value) <= 4096 else ""


def envelope(segment) -> Envelope | None:
    if not isinstance(segment, dict):
        return None
    kind = segment.get("type")
    if not isinstance(kind, str):
        return None
    kind = kind.lower()
    data = segment.get("data", {})
    if kind == "forward" and isinstance(data, dict):
        # 不访问 data.content / messages / nodes。
        return Envelope("onebot11:forward-id", _resource_id(data.get("id")))
    if kind != "json":
        return None
    payload = data.get("data") if isinstance(data, dict) else data
    if isinstance(payload, str):
        if len(payload) > 65536:
            return None
        try:
            payload = json.loads(payload)
        except (ValueError, RecursionError):
            return None
    if not isinstance(payload, dict) or payload.get("app") != "com.tencent.multimsg":
        return None
    meta = payload.get("meta")
    detail = meta.get("detail") if isinstance(meta, dict) else None
    if not isinstance(detail, dict):
        return Envelope("qq:multimsg-resid", "")
    # 卡片可见的标题/摘要/预览文本，不含内部节点，也不访问 extra/content。
    preview = detail.get("news")
    news = []
    if isinstance(preview, list):
        news = [_string(item.get("text"), 512) for item in preview[:4] if isinstance(item, dict)]
    visible = {"source": _string(detail.get("source"), 512),
               "summary": _string(detail.get("summary"), 512), "news": news}
    card = json.dumps(visible, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    if not visible["source"] and not visible["summary"] and not any(news):
        card = ""
    return Envelope("qq:multimsg-resid", _resource_id(detail.get("resid")), card)


def check_top_level(detector, segments, *, scope, message_id):
    """segments 必须是外层消息数组。忽略 node/嵌套内容，无网络访问。"""
    results = []
    if not isinstance(segments, list):
        return results
    for part, segment in enumerate(segments[:32]):
        item = envelope(segment)
        if item is None:
            continue
        result = detector.forward_native.ingest(scope, message_id, part,
            item.namespace, item.resource_id, item.visible_card)
        results.append(result)
    return results


def format_forward_hit(result):
    if result.status == "same_resource" and not result.replayed:
        return f"这个聊天记录包的顶层资源 ID 之前出现过，首次消息：{result.first_message_id}。"
    return None
