"""Transport-independent message detection, backed by the C++ repository interfaces."""
import io
import warnings
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

from . import _native
from .messages import ForwardMessage, ImageMessage, Message, MessageContext

DetectionStatus = _native.DetectionStatus
HashBasis = _native.HashBasis
MessageKind = _native.MessageKind
MAX_BYTES = 8 * 1024 * 1024
MAX_PIXELS = 16_000_000


def _decode_rgb(data: bytes, *, max_bytes=None, max_pixels=None):
    max_bytes = MAX_BYTES if max_bytes is None else max_bytes
    max_pixels = MAX_PIXELS if max_pixels is None else max_pixels
    if not isinstance(data, bytes):
        raise ValueError("图片输入必须为 bytes")
    if not data:
        raise ValueError("图片为空：0 bytes")
    if len(data) > max_bytes:
        raise ValueError(f"图片文件过大：{len(data)} bytes，上限 {max_bytes} bytes ({max_bytes / 1048576:g} MiB)")
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        try:
            opened = Image.open(io.BytesIO(data))
        except (Image.DecompressionBombWarning, Image.DecompressionBombError) as exc:
            raise ValueError(f"图片像素过多：触发 Pillow 解压保护，配置上限 {max_pixels} pixels；{exc}") from exc
        with opened as image:
            reasons = []
            pixels = image.width * image.height
            if pixels > max_pixels:
                reasons.append(f"图片像素过多：{image.width}x{image.height}={pixels} pixels，上限 {max_pixels} pixels")
            if min(image.size) < 16:
                reasons.append(f"图片尺寸过小：{image.width}x{image.height}，短边至少 16 pixels")
            frames = getattr(image, "n_frames", 1)
            hdr = any(marker in data for marker in (
                b"urn:com:apple:photo:2020:aux:hdrgainmap",
                b"http://ns.adobe.com/hdr-gain-map/1.0/",
                b"urn:iso:std:iso:ts:21496:-1",
            ))
            if hdr:
                reasons.append(f"强制跳过带 HDR 增益图的照片：format={image.format}, frames={frames}，包含辅助图像")
            if frames != 1:
                # These XMP namespaces identify auxiliary HDR data, not animation.
                if hdr:
                    pass
                elif image.format in ("GIF", "PNG", "WEBP", "AVIF") and getattr(image, "is_animated", False):
                    reasons.append(f"暂不支持动图：format={image.format}, frames={frames}，不自动只取首帧")
                else:
                    reasons.append(f"暂不支持多图像文件：format={image.format}, frames={frames}，不自动只取首幅")
            if reasons:
                raise ValueError("；".join(reasons))
            image = ImageOps.exif_transpose(image).convert("RGBA")
            background = Image.new("RGBA", image.size, "white")
            background.alpha_composite(image)
            return np.array(background.convert("RGB"), dtype=np.uint8)


class Detector:
    def __init__(self, db_path="data/huoxing.sqlite", max_images=2000, *, max_bytes=MAX_BYTES, max_pixels=MAX_PIXELS):
        self.max_bytes = max_bytes
        self.max_pixels = max_pixels
        path = str(Path(db_path).resolve())
        self.native = _native.Detector(path, max_images)
        # Kept for compatibility with existing event adapters; it delegates to the same engine.
        self.forward_native = _native.ForwardDetector(path)

    def detect(self, message: Message):
        if not isinstance(message, Message):
            raise TypeError("detect() expects a Message")
        context = message.context
        native_context = _native.MessageContext(
            context.scope, context.message_id, context.sender_id, context.part)
        if isinstance(message.payload, ImageMessage):
            payload = _native.ImageMessage(_decode_rgb(message.payload.data, max_bytes=self.max_bytes, max_pixels=self.max_pixels))
        elif isinstance(message.payload, ForwardMessage):
            payload = _native.ForwardMessage(message.payload.resource_namespace,
                message.payload.resource_id, message.payload.visible_card)
        else:
            raise TypeError("Unsupported message payload")
        return self.native.detect(_native.Message(native_context, payload))

    def ingest(self, data: bytes, *, scope: str, message_id: str,
               sender_id: str = "", part: int = 0):
        return self.native.ingest_rgb(_decode_rgb(data, max_bytes=self.max_bytes, max_pixels=self.max_pixels), scope, message_id, sender_id, part)

    def delete_scope(self, scope: str):
        # The repository clears both kinds and their identity index in one transaction.
        self.native.delete_scope(scope)


def result_dict(result):
    return {
        "duplicate": result.duplicate, "replayed": result.replayed,
        "image_id": result.image_id,
        "first_message_id": result.first_message_id,
        "first_sender_id": result.first_sender_id,
        "first_seen": result.first_seen,
        "message_hash": result.message_hash,
        "hash_basis": result.hash_basis,
        "evidence": {name: getattr(result.evidence, name) for name in (
            "method", "inliers", "inlier_ratio", "coverage", "pixel_agreement")},
    }
