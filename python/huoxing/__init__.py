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


def _decode_rgb(data: bytes):
    if not isinstance(data, bytes) or not data or len(data) > MAX_BYTES:
        raise ValueError("图片为空或超过 8 MiB")
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(io.BytesIO(data)) as image:
            if image.width * image.height > MAX_PIXELS or min(image.size) < 16:
                raise ValueError("图片尺寸不在支持范围内")
            if getattr(image, "n_frames", 1) != 1:
                raise ValueError("暂不支持动图，不自动只取首帧")
            image = ImageOps.exif_transpose(image).convert("RGBA")
            background = Image.new("RGBA", image.size, "white")
            background.alpha_composite(image)
            return np.array(background.convert("RGB"), dtype=np.uint8)


class Detector:
    def __init__(self, db_path="data/huoxing.sqlite", max_images=2000):
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
            payload = _native.ImageMessage(_decode_rgb(message.payload.data))
        elif isinstance(message.payload, ForwardMessage):
            payload = _native.ForwardMessage(message.payload.resource_namespace,
                message.payload.resource_id, message.payload.visible_card)
        else:
            raise TypeError("Unsupported message payload")
        return self.native.detect(_native.Message(native_context, payload))

    def ingest(self, data: bytes, *, scope: str, message_id: str,
               sender_id: str = "", part: int = 0):
        return self.native.ingest_rgb(_decode_rgb(data), scope, message_id, sender_id, part)

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
