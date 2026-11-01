import asyncio
import io
import types
from pathlib import Path

import pytest
from PIL import Image

import huoxing
from huoxing.classification import is_regular_image
from conftest import encode
from test_plugins import load, module


@pytest.mark.parametrize("segment", [
    {"type": kind, "data": {}} for kind in ("face", "mface", "emoji", "node", "reply")
] + [{"type": "image", "data": data} for data in (
    {"sub_type": 1}, {"sub_type": "1"}, {"subType": 2}, {"sub_type": "bad"},
    {"file": "marketface"}, {"sub_type": 0, "subType": 1})])
def test_stickers_are_not_images(segment):
    assert not is_regular_image(segment)


@pytest.mark.parametrize("data", [{}, {"sub_type": 0}, {"sub_type": "0"}, {"sub_type": ""}])
def test_normal_images_are_allowed(data):
    assert is_regular_image({"type": "image", "data": data})


def test_skip_reasons(monkeypatch):
    with pytest.raises(ValueError, match="0 bytes"):
        huoxing._decode_rgb(b"")
    with pytest.raises(ValueError, match="8388609 bytes.*8388608 bytes"):
        huoxing._decode_rgb(b"x" * (huoxing.MAX_BYTES + 1))
    with pytest.raises(ValueError, match="8x20.*16 pixels"):
        huoxing._decode_rgb(encode(Image.new("RGB", (8, 20))))
    monkeypatch.setattr(huoxing, "MAX_PIXELS", 100)
    with pytest.raises(ValueError, match="20x20=400 pixels.*100 pixels"):
        huoxing._decode_rgb(encode(Image.new("RGB", (20, 20))))


def test_hdr_is_not_reported_as_animation_and_keeps_size_reason(monkeypatch):
    primary = Image.new("RGB", (40, 30))
    auxiliary = Image.new("L", (20, 15))
    out = io.BytesIO()
    primary.save(out, format="MPO", save_all=True, append_images=[auxiliary],
                 xmp=b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</AuxiliaryImageType></x:xmpmeta>')
    monkeypatch.setattr(huoxing, "MAX_PIXELS", 1000)
    with pytest.raises(ValueError) as error:
        huoxing._decode_rgb(out.getvalue())
    text = str(error.value)
    assert "40x30=1200 pixels" in text and "HDR 增益图" in text
    assert "动图" not in text


def test_pillow_bomb_becomes_an_explicit_skip(monkeypatch):
    data = encode(Image.new("RGB", (40, 30)))
    monkeypatch.setattr(Image, "MAX_IMAGE_PIXELS", 100)
    with pytest.raises(ValueError, match="Pillow 解压保护"):
        huoxing._decode_rgb(data)


def test_animation_and_plain_mpo_have_distinct_reasons():
    frames = [Image.new("RGB", (40, 30), color) for color in ("white", "black")]
    for fmt, expected in (("GIF", "动图"), ("MPO", "多图像文件")):
        out = io.BytesIO()
        frames[0].save(out, format=fmt, save_all=True, append_images=frames[1:])
        with pytest.raises(ValueError) as error:
            huoxing._decode_rgb(out.getvalue())
        assert expected in str(error.value) and "frames=2" in str(error.value)
        assert "HDR" not in str(error.value)


def astr_plugin(monkeypatch, tmp_path, image_class, warnings):
    class Star:
        def __init__(self, context):
            pass
    filters = types.SimpleNamespace(EventMessageType=types.SimpleNamespace(GROUP_MESSAGE=1),
        event_message_type=lambda *args: lambda function: function)
    module(monkeypatch, "astrbot")
    module(monkeypatch, "astrbot.api", logger=types.SimpleNamespace(warning=lambda *args: warnings.append(args)))
    module(monkeypatch, "astrbot.api.event", AstrMessageEvent=object, filter=filters)
    module(monkeypatch, "astrbot.api.message_components", Image=image_class)
    module(monkeypatch, "astrbot.api.star", Star=Star, Context=object,
        StarTools=types.SimpleNamespace(get_data_dir=lambda: tmp_path))
    value = load("policy_astr_plugin", Path(__file__).parents[2] / "plugins/astrbot/main.py")
    class RecordingDetector:
        def __init__(self, path):
            self.calls = []
        def ingest(self, data, **metadata):
            self.calls.append(metadata)
            huoxing._decode_rgb(data)
    monkeypatch.setattr(value, "Detector", RecordingDetector)
    return value.HuoxingPlugin(None)


@pytest.mark.parametrize("kind,data", [("mface", {}), ("face", {}), ("emoji", {}),
    ("image", {"sub_type": 1}), ("image", {"sub_type": "1"}), ("image", {"file": "marketface"})])
def test_astrbot_rejects_stickers_before_constructing_or_downloading(monkeypatch, tmp_path, kind, data):
    constructed = []
    class HostImage:
        def __init__(self, **kwargs):
            constructed.append(kwargs)
        async def convert_to_file_path(self):
            raise AssertionError("Sticker was downloaded")
    warnings = []
    plugin = astr_plugin(monkeypatch, tmp_path, HostImage, warnings)
    normal = tmp_path / "normal.png"
    normal.write_bytes(encode(Image.new("RGB", (40, 40))))
    async def convert(self):
        return str(normal)
    HostImage.convert_to_file_path = convert
    raw = [{"type": kind, "data": data}, {"type": "image", "data": {"url": "https://example.com/normal"}}]
    class Event:
        message_obj = types.SimpleNamespace(message_id="m1", raw_message={"message": raw})
        get_platform_name = lambda self: "aiocqhttp"
        get_group_id = lambda self: "g"
        get_self_id = lambda self: "bot"
        get_sender_id = lambda self: "u"
        # A normalized chain cannot distinguish the sticker from an ordinary Image.
        get_messages = lambda self: [object(), object()]
        plain_result = lambda self, text: text
    async def exercise():
        assert [item async for item in plugin.on_message(Event())] == []
        await plugin.terminate()
    asyncio.run(exercise())
    assert len(constructed) == 1 and constructed[0]["url"] == "https://example.com/normal"
    assert not warnings
    assert [call["part"] for call in plugin.worker.detector.calls] == [1]
    assert not (tmp_path / "images.sqlite").exists()


def test_astrbot_log_contains_reason_and_context(monkeypatch, tmp_path):
    class HostImage:
        async def convert_to_file_path(self):
            raise ValueError("图片文件过大：9000000 bytes，上限 8388608 bytes (8 MiB)")
    warnings = []
    plugin = astr_plugin(monkeypatch, tmp_path, HostImage, warnings)
    class Event:
        message_obj = types.SimpleNamespace(message_id="photo-123")
        get_group_id = lambda self: "g"
        get_self_id = lambda self: "bot"
        get_sender_id = lambda self: "u"
        get_messages = lambda self: [HostImage()]
    async def exercise():
        assert [item async for item in plugin.on_message(Event())] == []
        await plugin.terminate()
    asyncio.run(exercise())
    text = warnings[-1][0] % warnings[-1][1:]
    assert "photo-123" in text and "part=0" in text and "9000000 bytes" in text


def test_maibot_stickers_never_reach_worker(monkeypatch, tmp_path):
    module(monkeypatch, "maibot_sdk", MaiBotPlugin=object,
           HookHandler=lambda *args, **kwargs: lambda function: function)
    module(monkeypatch, "maibot_sdk.types", HookMode=types.SimpleNamespace(OBSERVE="observe"))
    plugin = load("policy_mai_plugin", Path(__file__).parents[2] / "plugins/maibot/plugin.py").create_plugin()
    async def unexpected(*args, **kwargs):
        raise AssertionError("Sticker reached image worker")
    async def no_results(*args, **kwargs):
        return []
    plugin.worker = types.SimpleNamespace(check=unexpected, check_forward=no_results)
    plugin.ctx = types.SimpleNamespace(logger=types.SimpleNamespace(warning=unexpected))
    async def exercise():
        for kind, data in [("emoji", {}), ("mface", {}), ("face", {}),
                           ("image", {"sub_type": 1}), ("image", {"file": "marketface"})]:
            await plugin.detect_images({"message_info": {"group_info": {"group_id": "g"}, "message_id": "1"},
                "raw_message": [{"type": kind, "data": {"binary_data_base64": "AAAA", **data}}]})
    asyncio.run(exercise())
