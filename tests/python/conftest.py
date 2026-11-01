import io
import numpy as np
from PIL import Image, ImageDraw, ImageOps


def encode(im, fmt="PNG", **kwargs):
    stream = io.BytesIO()
    im.save(stream, fmt, **kwargs)
    return stream.getvalue()


def fixture(seed=42):
    rng = np.random.default_rng(seed)
    im = Image.new("RGB", (640, 480), (235, 230, 218))
    draw = ImageDraw.Draw(im)
    for _ in range(170):
        x, y = rng.integers(5, 580), rng.integers(5, 420)
        w, h = rng.integers(8, 60, size=2)
        color = tuple(int(v) for v in rng.integers(0, 230, size=3))
        draw.ellipse((x, y, x+w, y+h), fill=color, outline="black", width=2)
    draw.text((50, 100), f"MEME {seed}: HELLO GROUP", fill="black", stroke_width=1)
    return im


def variants(im):
    marked = im.copy()
    d = ImageDraw.Draw(marked)
    d.rectangle((350, 350, 620, 435), fill="white")
    d.text((370, 380), "WATERMARK 2026", fill="black", stroke_width=1)
    return {
        "crop": encode(im.crop((100, 70, 550, 420))),
        "border": encode(ImageOps.expand(im, (60, 40, 100, 80), "white")),
        "watermark": encode(marked),
        "jpeg": encode(im.resize((400, 300)), "JPEG", quality=60),
        "combined": encode(ImageOps.expand(marked.crop((70, 30, 600, 450)),
                            35, "black").resize((540, 440)), "JPEG", quality=70),
    }
