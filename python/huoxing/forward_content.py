"""Bounded whole-forward fingerprints. Fetch message metadata, never media bytes."""
import asyncio
import hashlib
import json
import re
from dataclasses import dataclass

from .cards import card_identity
from .forward import envelope
from .policy import RecentImages

MAX_MESSAGES = 200
MAX_SEGMENTS = 2000
MAX_REQUESTS = 4
MAX_DEPTH = 3
MAX_JSON_BYTES = 1024 * 1024
TOTAL_TIMEOUT = 6


def _digest(value):
    digest, size = hashlib.sha256(), 0
    encoder = json.JSONEncoder(ensure_ascii=False, separators=(",", ":"))
    for chunk in encoder.iterencode(value):
        encoded = chunk.encode("utf-8")
        size += len(encoded)
        if size > MAX_JSON_BYTES:
            raise ValueError("整包规范化内容超过 1 MiB")
        digest.update(encoded)
    return digest.hexdigest()


@dataclass
class Budget:
    messages: int = 0
    segments: int = 0
    requests: int = 0
    size: int = 0

    def charge(self, value):
        try:
            for chunk in json.JSONEncoder(ensure_ascii=False).iterencode(value):
                self.size += len(chunk.encode("utf-8"))
                if self.size > MAX_JSON_BYTES:
                    raise ValueError("整包元数据超过 1 MiB，不使用截断内容计算指纹")
        except (RecursionError, TypeError) as exc:
            raise ValueError("整包元数据格式无效或嵌套过深") from exc


class ForwardContentReader:
    def __init__(self):
        # Retain only small digests; no message bodies or downloaded media.
        self.cache = RecentImages(600, capacity=256)

    async def read(self, segments, fetch, *, scope, warn=lambda *args: None):
        """Return {outer part: SHA-256}; failures fall back to top-level IDs."""
        budget = Budget()
        memo, active, digests = {}, set(), {}

        async def operation():
            for part, segment in enumerate(segments[:32]):
                item = envelope(segment)
                if item is None or not item.resource_id:
                    continue
                key = (scope, item.namespace, item.resource_id)
                cached, digest = self.cache.get(key)
                if cached:
                    digests[part] = digest
                    continue
                try:
                    digest = await self._resource(item.resource_id, fetch, budget, memo, active, 0)
                    self.cache.remember(key, digest)
                    digests[part] = digest
                except Exception as exc:
                    warn("整包内容指纹跳过：part=%s %s: %s", part, type(exc).__name__, str(exc)[:300])

        try:
            await asyncio.wait_for(operation(), timeout=TOTAL_TIMEOUT)
        except asyncio.TimeoutError:
            warn("整包内容指纹跳过：超过 %s 秒总超时，保留已完成的整包指纹", TOTAL_TIMEOUT)
        return digests

    async def _resource(self, resource_id, fetch, budget, memo, active, depth):
        if depth > MAX_DEPTH:
            raise ValueError("整包嵌套超过 3 层")
        if resource_id in active:
            raise ValueError("整包存在循环引用")
        if resource_id in memo:
            return memo[resource_id]
        if budget.requests >= MAX_REQUESTS:
            raise ValueError("整包读取超过 4 次 API 请求")
        budget.requests += 1
        active.add(resource_id)
        try:
            response = await asyncio.wait_for(fetch(resource_id), timeout=3)
            budget.charge(response)
            if isinstance(response, dict) and "data" in response:
                if response.get("status", "ok") != "ok" or response.get("retcode", 0) != 0:
                    raise ValueError("get_forward_msg 返回失败")
                response = response["data"]
            if isinstance(response, dict):
                response = response.get("messages", response.get("message"))
            result = _digest(await self._nodes(response, fetch, budget, memo, active, depth))
            memo[resource_id] = result
            return result
        finally:
            active.discard(resource_id)

    async def _nodes(self, nodes, fetch, budget, memo, active, depth):
        if depth > MAX_DEPTH or not isinstance(nodes, list) or not nodes:
            raise ValueError("整包节点为空、格式无效或嵌套超过 3 层")
        budget.messages += len(nodes)
        if budget.messages > MAX_MESSAGES:
            raise ValueError("整包超过 200 条消息，不使用部分消息计算指纹")
        result = []
        for node in nodes:
            if not isinstance(node, dict):
                raise ValueError("整包消息节点格式无效")
            if node.get("type") == "node":
                node = node.get("data")
                if not isinstance(node, dict):
                    raise ValueError("整包 node.data 格式无效")
            sender = node.get("sender") or {}
            uid = node.get("user_id", node.get("uin", sender.get("user_id") if isinstance(sender, dict) else None))
            if isinstance(uid, (str, int)) and not isinstance(uid, bool) and str(uid):
                author = ["id", str(uid)]
            elif isinstance(node.get("name"), str) and node["name"]:
                author = ["name", node["name"]]
            else:
                raise ValueError("整包消息缺少发送者身份")
            content = node.get("message", node.get("content"))
            if not isinstance(content, list) or not content:
                raise ValueError("整包消息缺少数组格式正文")
            budget.segments += len(content)
            if budget.segments > MAX_SEGMENTS:
                raise ValueError("整包超过 2000 个消息段")
            parts = []
            for segment in content:
                normalized = await self._segment(segment, fetch, budget, memo, active, depth)
                if normalized[0] == "text" and parts and parts[-1][0] == "text":
                    parts[-1][1] += normalized[1]
                else:
                    parts.append(normalized)
            result.append([author, parts])
        return result

    async def _segment(self, segment, fetch, budget, memo, active, depth):
        if not isinstance(segment, dict) or not isinstance(segment.get("data"), dict):
            raise ValueError("整包消息段格式无效")
        kind, data = segment.get("type"), segment["data"]
        if kind == "text" and isinstance(data.get("text"), str):
            return ["text", data["text"]]
        item = envelope(segment)
        if item is not None:
            if not item.resource_id:
                raise ValueError("嵌套转发缺少资源 ID")
            return ["forward", await self._resource(item.resource_id, fetch, budget, memo, active, depth+1)]
        if kind == "node":
            return ["forward", _digest(await self._nodes([segment], fetch, budget, memo, active, depth+1))]
        if kind in ("image", "record", "video", "file"):
            for field in ("md5", "file"):
                value = data.get(field)
                match = re.fullmatch(r"([a-fA-F0-9]{32})(?:\.[A-Za-z0-9]{1,8})?", value) if isinstance(value, str) else None
                if match:
                    return [kind, "md5", match[1].lower()]
            if kind == "file" and isinstance(data.get("file_id"), str) and data["file_id"]:
                return [kind, "file_id", data["file_id"]]
            raise ValueError(f"整包 {kind} 缺少稳定文件标识；不下载媒体、不忽略该段")
        if kind in ("at", "face", "reply"):
            key = "qq" if kind == "at" else "id"
            value = data.get(key)
            if isinstance(value, (str, int)) and not isinstance(value, bool) and str(value):
                return [kind, str(value)]
        if kind in ("mface", "emoji"):
            ident = data.get("emoji_id")
            package = data.get("emoji_package_id", data.get("package_id"))
            if isinstance(ident, (str, int)) and isinstance(package, (str, int)):
                return [kind, str(package), str(ident)]
        if kind in ("json", "xml"):
            identity = card_identity(segment)
            if identity:
                return ["card", *identity]
            payload = data.get("data")
            if kind == "json" and isinstance(payload, str):
                try:
                    payload = json.loads(payload)
                except (ValueError, RecursionError) as exc:
                    raise ValueError("整包 JSON 卡片格式无效") from exc
            if isinstance(payload, (dict, str)):
                # Unknown cards keep every field; prefer a missed match to
                # dropping possibly meaningful payload information.
                return [kind, json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))]
        raise ValueError(f"整包不支持的消息段：{kind}；不使用部分内容计算指纹")
