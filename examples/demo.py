"""Run after `pixi run install`; creates no permanent image database."""
import io
import json
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from huoxing import (Detector, ForwardMessage, ImageMessage, Message,
                     MessageContext, result_dict)


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
    detector.detect(Message(
        MessageContext("qq:demo:group", "original", "alice"),
        ImageMessage(encode(image)),
    ))
    result = detector.detect(Message(
        MessageContext("qq:demo:group", "cropped", "bob"),
        ImageMessage(encode(image.crop((100, 70, 550, 420)))),
    ))
    assert result.duplicate and result.first_message_id == "original"
    print(json.dumps(result_dict(result), ensure_ascii=False, indent=2))

    # Simulated outer resource metadata; no QQ connection or package download.
    forward = ForwardMessage("onebot11:forward-id", "demo-resource")
    detector.detect(Message(MessageContext("qq:demo:group", "forward-1", "alice"), forward))
    result = detector.detect(Message(
        MessageContext("qq:demo:group", "forward-2", "bob"), forward,
    ))
    assert result.duplicate and result.first_message_id == "forward-1"
    print(json.dumps(result_dict(result), ensure_ascii=False, indent=2))
