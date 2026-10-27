"""Run after `pixi run install`; creates no permanent image database."""
import io
import json
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from huoxing import Detector, result_dict


def encode(image):
    output = io.BytesIO()
    image.save(output, "PNG")
    return output.getvalue()


image = Image.new("RGB", (640, 480), (235, 230, 218))
draw = ImageDraw.Draw(image)
rng = np.random.default_rng(42)
for _ in range(170):
    x, y = rng.integers(5, 580), rng.integers(5, 420)
    width, height = rng.integers(8, 60, size=2)
    color = tuple(int(value) for value in rng.integers(0, 230, size=3))
    draw.ellipse((x, y, x + width, y + height), fill=color, outline="black", width=2)

with tempfile.TemporaryDirectory(prefix="huoxing-demo-") as directory:
    detector = Detector(Path(directory) / "demo.sqlite")
    detector.ingest(encode(image), scope="qq:demo:group", message_id="original", sender_id="alice")
    result = detector.ingest(encode(image.crop((100, 70, 550, 420))),
                             scope="qq:demo:group", message_id="cropped", sender_id="bob")
    print(json.dumps(result_dict(result), ensure_ascii=False, indent=2))
