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


def read_image(path):
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
            elif kind == "json":
                outer.append({"type": "json", "data": {"data": getattr(segment, "data", None)}})
            else:
                outer.append(None)  # 不调用 Node/Nodes.to_dict()，它可能展开内部媒体。
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
        # 只遍历最顶层 Image；Node、Nodes、Forward、Reply 内部均不访问。
        for part, segment in enumerate(event.get_messages()[:32]):
            if not isinstance(segment, Image):
                continue
            if len(hits) >= 1:
                break
            try:
                path = await asyncio.wait_for(segment.convert_to_file_path(), timeout=8)
                data = await asyncio.to_thread(read_image, path)
                result = await self.worker.check(data,
                    scope=f"qq:{event.get_self_id()}:{group}", message_id=msg_id,
                    sender_id=str(event.get_sender_id()), part=part)
                if result and result.duplicate and not result.replayed:
                    hits.append(format_hit(result))
            except (ValueError, OSError, RuntimeError, asyncio.TimeoutError) as exc:
                logger.warning("火星检测跳过图片：%s", type(exc).__name__)
        if hits:
            yield event.plain_result(hits[0])

    async def terminate(self):
        await self.worker.close()
