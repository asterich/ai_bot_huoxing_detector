"""复制整个目录到 AstrBot/data/plugins/astrbot_plugin_huoxing。"""
import asyncio
import hashlib
from pathlib import Path

from astrbot.api import AstrBotConfig, logger
from astrbot.api.event import AstrMessageEvent, filter
from astrbot.api.message_components import Image
from astrbot.api.star import Context, Star, StarTools

from huoxing import Detector, MAX_BYTES
from huoxing.worker import ImageWorker
from huoxing.forward import format_forward_hit
from huoxing.cards import format_card_hit
from huoxing.classification import is_regular_image
from huoxing.policy import Settings, Cooldowns, TriggerLimiter, RecentImages


def read_image(path, max_bytes=MAX_BYTES):
    size = Path(path).stat().st_size
    if size > max_bytes:
        raise ValueError(f"图片文件过大：{size} bytes，上限 {max_bytes} bytes ({max_bytes / 1048576:g} MiB)")
    with Path(path).open("rb") as stream:
        data = stream.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ValueError(f"图片文件过大：读取超过 {max_bytes} bytes ({max_bytes / 1048576:g} MiB)")
    return data


class HuoxingPlugin(Star):
    def __init__(self, context: Context, config: AstrBotConfig = None):
        super().__init__(context)
        self.settings = Settings.from_config(config, logger.warning)
        self.worker = ImageWorker(Detector(StarTools.get_data_dir() / "images.sqlite",
            max_bytes=self.settings.max_bytes, max_pixels=self.settings.max_pixels))
        self.seen_messages = Cooldowns()
        self.limiter = TriggerLimiter(self.settings)
        self.recent_images = RecentImages(self.settings.same_image_cooldown_seconds)
        self.replies = Cooldowns()
        self._lock = asyncio.Lock()

    @filter.event_message_type(filter.EventMessageType.GROUP_MESSAGE)
    async def on_message(self, event: AstrMessageEvent):
        group = str(event.get_group_id() or "")
        sender = str(event.get_sender_id())
        if not group or sender == str(event.get_self_id()):
            return
        msg_id = str(event.message_obj.message_id or "")
        if not msg_id:
            return
        scope = f"qq:{event.get_self_id()}:{group}"
        if self.limiter.blocked((scope, sender)):
            return
        outer = []
        messages = event.get_messages()[:32]
        for segment in messages:
            kind = str(getattr(getattr(segment, "type", None), "value", "")).lower()
            if kind == "forward":
                outer.append({"type": "forward", "data": {"id": getattr(segment, "id", "")}})
            elif kind in ("json", "xml"):
                outer.append({"type": kind, "data": {"data": getattr(segment, "data", None)}})
            else:
                outer.append(None)  # Never expand Node/Nodes/Reply content.
        raw_images = None
        if getattr(event, "get_platform_name", lambda: "")() == "aiocqhttp":
            raw = getattr(event.message_obj, "raw_message", None)
            if callable(getattr(raw, "get", None)):
                segments = raw.get("message")
                if isinstance(segments, list):
                    outer = raw_images = segments[:32]
        images = []
        for part, segment in enumerate(raw_images if raw_images is not None else messages):
            if raw_images is not None:
                regular = is_regular_image(segment)
            else:
                regular = isinstance(segment, Image) and is_regular_image({
                    "type": "image", "data": {"file": getattr(segment, "file", ""),
                    "sub_type": getattr(segment, "sub_type", None), "subType": getattr(segment, "subType", None)}})
            if regular:
                images.append((part, segment))
        if not images and not any(isinstance(s, dict) and s.get("type") in ("forward", "json", "xml") for s in outer):
            return
        # No waiting queue: cover download, file read AND native work. The worker
        # remains busy after a cancelled await until its background thread exits.
        if self._lock.locked() or self.worker.busy:
            return
        settings = self.settings
        if not self.seen_messages.admit([((scope, msg_id), 86400)]):
            return
        hit = None
        async with self._lock:
            for kind, operation, formatter in (
                ("forward", self.worker.check_forward, format_forward_hit),
                ("card", self.worker.check_cards, format_card_hit),
            ):
                try:
                    results = await operation(outer, scope=scope, message_id=msg_id)
                    for result in results or []:
                        if formatter(result):
                            hit = (kind, result)
                            break
                except (ValueError, OSError, RuntimeError) as exc:
                    logger.warning("火星检测跳过 %s：message_id=%s %s: %s", kind, msg_id, type(exc).__name__, str(exc))
                if hit:
                    break
            for part, segment in images:
                result = await self._check_image(part, segment, raw_images is not None, scope, msg_id, sender)
                if result is not None:
                    if hit is None:
                        hit = ("image", result)
                    if self.limiter.record((scope, sender)):
                        break
            text = None
            if hit and self.replies.admit([(("reply", scope), settings.reply_cooldown_seconds)]):
                text = settings.reply(*hit)
        # Release the processing lock before yielding to the host send pipeline.
        if text:
            yield event.plain_result(text)

    async def _check_image(self, part, segment, raw, scope, msg_id, sender):
        if raw:
            media = segment["data"]
            source = media.get("file") or media.get("url") or ""
        else:
            source = getattr(segment, "file", "") or getattr(segment, "url", "")
        source_key = ("source", scope, hashlib.sha256(str(source).encode()).digest()) if source else None
        if source_key:
            cached, result = self.recent_images.get(source_key)
            if cached:
                return result
        digest_key = None
        try:
            if raw:
                segment = Image(file=media.get("file") or media.get("url") or "", url=media.get("url") or "")
            path = await asyncio.wait_for(segment.convert_to_file_path(), timeout=8)
            data = await asyncio.to_thread(read_image, path, self.settings.max_bytes)
            digest_key = ("content", scope, hashlib.sha256(data).digest())
            cached, result = self.recent_images.get(digest_key)
            if cached:
                self.recent_images.remember(source_key, result)
                return result
            result = await self.worker.check(data, scope=scope, message_id=msg_id, sender_id=sender, part=part)
            if result and result.duplicate and not result.replayed:
                self.recent_images.remember(source_key, result)
                self.recent_images.remember(digest_key, result)
                return result
        except (ValueError, OSError, RuntimeError, asyncio.TimeoutError) as exc:
            self.recent_images.remember(source_key, None)
            self.recent_images.remember(digest_key, None)
            reason = str(exc) or ("图片下载超时（8 秒）" if isinstance(exc, asyncio.TimeoutError) else "未提供异常详情")
            logger.warning("火星检测跳过图片：message_id=%s part=%s %s: %s", msg_id, part, type(exc).__name__, reason)
        return None

    async def terminate(self):
        await self.worker.close()
