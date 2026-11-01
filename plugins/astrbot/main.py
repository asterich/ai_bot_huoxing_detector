"""复制整个目录到 AstrBot/data/plugins/astrbot_plugin_huoxing。"""
import asyncio
from pathlib import Path

from astrbot.api import logger
from astrbot.api.event import AstrMessageEvent, filter
from astrbot.api.message_components import Image
from astrbot.api.star import Context, Star, StarTools

from huoxing import Detector, MAX_BYTES
from huoxing.worker import ImageWorker, format_hit
from huoxing.forward import format_forward_hit
from huoxing.classification import is_regular_image


def read_image(path):
    size = Path(path).stat().st_size
    if size > MAX_BYTES:
        raise ValueError(f"图片文件过大：{size} bytes，上限 {MAX_BYTES} bytes (8 MiB)")
    with Path(path).open("rb") as stream:
        return stream.read(MAX_BYTES + 1)


class HuoxingPlugin(Star):
    def __init__(self, context: Context):
        super().__init__(context)
        self.worker = ImageWorker(Detector(StarTools.get_data_dir() / "images.sqlite"))

    @filter.event_message_type(filter.EventMessageType.GROUP_MESSAGE)
    async def on_message(self, event: AstrMessageEvent):
        group = str(event.get_group_id() or "")
        if not group or str(event.get_sender_id()) == str(event.get_self_id()):
            return
        msg_id = str(event.message_obj.message_id or "")
        if not msg_id:
            return
        hits = []
        outer = []
        for segment in event.get_messages()[:32]:
            kind = str(getattr(getattr(segment, "type", None), "value", "")).lower()
            if kind == "forward":
                outer.append({"type": "forward", "data": {"id": getattr(segment, "id", "")}})
            elif kind in ("json", "xml"):
                outer.append({"type": kind, "data": {"data": getattr(segment, "data", None)}})
            else:
                outer.append(None)  # 不调用 Node/Nodes.to_dict()，它可能展开内部媒体。
        # AstrBot may discard unsupported XML components; OneBot retains them here.
        if getattr(event, "get_platform_name", lambda: "")() == "aiocqhttp":
            raw = getattr(event.message_obj, "raw_message", None)
            if callable(getattr(raw, "get", None)):
                segments = raw.get("message")
                if isinstance(segments, list):
                    outer = segments[:32]
        try:
            forward_results = await self.worker.check_forward(outer,
                scope=f"qq:{event.get_self_id()}:{group}", message_id=msg_id)
            for result in forward_results or []:
                text = format_forward_hit(result)
                if text:
                    hits.append(text)
                    break
        except (ValueError, OSError, RuntimeError) as exc:
            logger.warning("整包顶层查重跳过：%s", type(exc).__name__)
        # Classify raw OneBot segments before constructing Image: AstrBot drops sub_type.
        raw_images = None
        if getattr(event, "get_platform_name", lambda: "")() == "aiocqhttp":
            raw = getattr(event.message_obj, "raw_message", None)
            if callable(getattr(raw, "get", None)):
                segments = raw.get("message")
                if isinstance(segments, list):
                    raw_images = segments[:32]
        candidates = enumerate(raw_images if raw_images is not None else event.get_messages()[:32])
        for part, segment in candidates:
            if raw_images is not None:
                if not is_regular_image(segment):
                    continue
            elif not isinstance(segment, Image) or not is_regular_image({
                "type": "image", "data": {"file": getattr(segment, "file", ""),
                    "sub_type": getattr(segment, "sub_type", None),
                    "subType": getattr(segment, "subType", None)}}):
                continue
            if len(hits) >= 1:
                break
            try:
                if raw_images is not None:
                    media = segment["data"]
                    segment = Image(file=media.get("file") or media.get("url") or "",
                                    url=media.get("url") or "")
                path = await asyncio.wait_for(segment.convert_to_file_path(), timeout=8)
                data = await asyncio.to_thread(read_image, path)
                result = await self.worker.check(data,
                    scope=f"qq:{event.get_self_id()}:{group}", message_id=msg_id,
                    sender_id=str(event.get_sender_id()), part=part)
                if result and result.duplicate and not result.replayed:
                    hits.append(format_hit(result))
            except (ValueError, OSError, RuntimeError, asyncio.TimeoutError) as exc:
                reason = str(exc) or ("图片下载超时（8 秒）" if isinstance(exc, asyncio.TimeoutError) else "未提供异常详情")
                logger.warning("火星检测跳过图片：message_id=%s part=%s %s: %s",
                               msg_id, part, type(exc).__name__, reason)
        if hits:
            yield event.plain_result(hits[0])

    async def terminate(self):
        await self.worker.close()
