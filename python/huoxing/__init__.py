"""解码、输入限额和异步调度在 Python；匹配及图片数据库全部由 C++ 执行。"""
import io
import warnings
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

from ._native import Detector as NativeDetector

MAX_BYTES = 8 * 1024 * 1024
MAX_PIXELS = 16_000_000


class Detector:
    def __init__(self, db_path="data/huoxing.sqlite", max_images=2000):
        self.native = NativeDetector(str(Path(db_path).resolve()), max_images)

    def ingest(self, data: bytes, *, scope: str, message_id: str,
               sender_id: str = "", part: int = 0):
        if not isinstance(data, bytes) or not data or len(data) > MAX_BYTES:
            raise ValueError("图片为空或超过 8 MiB")
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as im:
                if im.width * im.height > MAX_PIXELS or min(im.size) < 16:
                    raise ValueError("图片尺寸不在支持范围内")
                if getattr(im, "n_frames", 1) != 1:
                    raise ValueError("暂不支持动图，不自动只取首帧")
                im = ImageOps.exif_transpose(im).convert("RGBA")
                bg = Image.new("RGBA", im.size, "white")
                bg.alpha_composite(im)
                rgb = np.array(bg.convert("RGB"), dtype=np.uint8)
        return self.native.ingest_rgb(rgb, scope, message_id, sender_id, part)

    def delete_scope(self, scope: str):
        self.native.delete_scope(scope)


def result_dict(result):
    return {
        "duplicate": result.duplicate, "replayed": result.replayed,
        "image_id": result.image_id,
        "first_message_id": result.first_message_id,
        "first_sender_id": result.first_sender_id,
        "first_seen": result.first_seen,
        "evidence": {name: getattr(result.evidence, name) for name in (
            "method", "inliers", "inlier_ratio", "coverage", "pixel_agreement")},
    }
