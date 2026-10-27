"""MaiBot SDK 2.x 示例；只处理 raw_message 最顶层图片/表情。"""
import base64
from pathlib import Path

from maibot_sdk import HookHandler, MaiBotPlugin
from maibot_sdk.types import HookMode

from huoxing import Detector, MAX_BYTES
from huoxing.worker import ImageWorker, format_hit


class HuoxingPlugin(MaiBotPlugin):
    async def on_load(self):
        self.worker = ImageWorker(Detector(Path(self.ctx.paths.data_dir) / "images.sqlite"))

    async def on_unload(self):
        await self.worker.close()

    async def on_config_update(self, scope, config_data, version):
        pass

    @HookHandler("chat.receive.before_process", mode=HookMode.OBSERVE, timeout_ms=15000)
    async def detect_images(self, message=None, **kwargs):
        if not isinstance(message, dict):
            return
        info = message.get("message_info", message)
        group = (info.get("group_info") or {}).get("group_id")
        msg_id = info.get("message_id")
        user = info.get("user_info") or {}
        # 宿主应提供真实群号；不把未知消息归入同一个默认群。
        if not group or not msg_id:
            return
        platform = info.get("platform") or user.get("platform") or "qq"
        raw = message.get("raw_message", [])
        if not isinstance(raw, list):
            return
        for part, segment in enumerate(raw[:32]):
            if not isinstance(segment, dict) or segment.get("type") not in ("image", "emoji"):
                continue
            data = segment.get("data")
            media = data if isinstance(data, dict) else segment
            encoded = media.get("binary_data_base64")
            if not isinstance(encoded, str) or len(encoded) > 4 * ((MAX_BYTES+2)//3):
                continue
            try:
                binary = base64.b64decode(encoded, validate=True)
                result = await self.worker.check(binary, scope=f"{platform}:{group}",
                    message_id=str(msg_id), sender_id=str(user.get("user_id", "")), part=part)
                if result and result.duplicate and not result.replayed:
                    stream_id = kwargs.get("stream_id") or info.get("stream_id")
                    if stream_id:
                        await self.ctx.send.text(format_hit(result), str(stream_id))
                    else:
                        self.ctx.logger.info("查重命中，宿主未提供 stream_id：%s", result.first_message_id)
                    break
            except (ValueError, OSError, RuntimeError) as exc:
                self.ctx.logger.warning("火星检测跳过图片：%s", type(exc).__name__)


def create_plugin():
    return HuoxingPlugin()
