import asyncio
import io
import json
import types
from pathlib import Path

import pytest
from PIL import Image

import huoxing
from huoxing.policy import Settings, TriggerLimiter, RecentImages, Cooldowns, IMAGE_TEXT
from test_image_policy import astr_plugin
from conftest import encode


def test_schema_defaults_match_settings_and_no_skip_switches():
    schema = json.loads((Path(__file__).parents[2] / "plugins/astrbot/_conf_schema.json").read_text(encoding="utf-8"))
    defaults = Settings()
    assert set(schema) == set(Settings.__dataclass_fields__)
    assert all(item["default"] == getattr(defaults, key) for key, item in schema.items())
    assert not any(name in schema for name in ("allow_hdr", "allow_animation", "allow_stickers"))


@pytest.mark.parametrize("config", [
    {"trigger_image_count": 0}, {"trigger_image_count": 1.5}, {"trigger_image_count": True},
    {"max_image_mib": 0}, {"max_image_megapixels": 33}, {"sender_pause_seconds": float("nan")},
    {"sender_pause_seconds": float("inf")}, {"trigger_window_seconds": "bad"},
    {"image_reply_template": "{first_message_id.__class__}"}, {"card_reply_template": "{"},
    {"forward_reply_template": "{first_seen:99999999}"}, {"image_reply_template": ""},
])
def test_bad_config_falls_back_and_warns(config):
    warnings = []
    assert Settings.from_config(config, lambda *args: warnings.append(args)) == Settings()
    assert len(warnings) == 1


def test_custom_templates_and_limits():
    settings = Settings.from_config({"image_reply_template": "火星：{first_message_id} {{ok}} {method}", "max_image_mib": 2.5, "max_image_megapixels": 4})
    result = types.SimpleNamespace(first_message_id="old", evidence=types.SimpleNamespace(method="pixels"))
    assert settings.reply("image", result) == "火星：old {ok} pixels"
    assert settings.max_bytes == 2621440 and settings.max_pixels == 4000000


def test_sliding_window_pause_boundary_scope_and_no_extension():
    now = [0]
    limiter = TriggerLimiter(Settings(trigger_window_seconds=120, trigger_image_count=3, sender_pause_seconds=300), clock=lambda: now[0])
    key = ("g", "u")
    assert not limiter.record(key)
    now[0] = 100
    assert not limiter.record(key)
    now[0] = 120  # The oldest hit has left the sliding window.
    assert not limiter.record(key)
    now[0] = 121
    assert limiter.record(key) and limiter.blocked(key)
    assert not limiter.blocked(("g", "other")) and not limiter.blocked(("other", "u"))
    now[0] = 420
    assert limiter.record(key)  # Traffic during a pause cannot extend it.
    now[0] = 421
    assert not limiter.blocked(key)
    assert not limiter.record(key)


@pytest.mark.parametrize("config", [{"trigger_window_seconds": 0}, {"sender_pause_seconds": 0}])
def test_pause_can_be_disabled(config):
    limiter = TriggerLimiter(Settings.from_config(config))
    for _ in range(10):
        assert not limiter.record("u") and not limiter.blocked("u")


def test_all_state_has_fixed_capacity_and_cache_expires():
    now = [0]
    cache = RecentImages(10, capacity=2, clock=lambda: now[0])
    evidence = object()
    cache.remember("a", evidence)
    cache.remember("b", None)
    assert cache.get("a") == (True, evidence)
    assert cache.get("b") == (True, None)
    cache.remember("c", evidence)
    assert cache.get("a") == (False, None)
    now[0] = 10
    assert cache.get("c") == (False, None)
    limiter = TriggerLimiter(Settings(), capacity=2)
    cooldown = Cooldowns(capacity=2)
    for key in range(10):
        limiter.record(key)
        cooldown.mark(key)
    assert len(limiter.people) == len(cooldown.entries) == 2


def test_configured_limits_do_not_change_mandatory_media_rejection():
    data = encode(Image.new("RGB", (400, 400)))
    with pytest.raises(ValueError, match="100000 pixels"):
        huoxing._decode_rgb(data, max_pixels=100000)
    assert huoxing._decode_rgb(data, max_pixels=200000).shape == (400, 400, 3)
    with pytest.raises(ValueError, match="100 bytes"):
        huoxing._decode_rgb(data, max_bytes=100)
    frames = [Image.new("RGB", (40, 30), color) for color in ("white", "black")]
    for fmt in ("GIF", "PNG", "WEBP"):
        out = io.BytesIO()
        frames[0].save(out, format=fmt, save_all=True, append_images=frames[1:], duration=100)
        with pytest.raises(ValueError, match="动图"):
            huoxing._decode_rgb(out.getvalue(), max_bytes=32 * 1024**2, max_pixels=32000000)
    out = io.BytesIO()
    frames[0].save(out, format="JPEG", xmp=b'http://ns.adobe.com/hdr-gain-map/1.0/')
    with pytest.raises(ValueError, match="HDR"):
        huoxing._decode_rgb(out.getvalue(), max_bytes=32 * 1024**2, max_pixels=32000000)


def harness(monkeypatch, tmp_path, config=None):
    downloads = []
    path = tmp_path / "photo.png"
    path.write_bytes(encode(Image.new("RGB", (40, 40))))
    class HostImage:
        def __init__(self, **kwargs):
            self.source = kwargs.get("file", "")
        async def convert_to_file_path(self):
            downloads.append(self.source)
            return str(path)
    plugin = astr_plugin(monkeypatch, tmp_path, HostImage, [])
    plugin.settings = Settings.from_config(config)
    now = [0]
    plugin.limiter = TriggerLimiter(plugin.settings, clock=lambda: now[0])
    plugin.recent_images = RecentImages(plugin.settings.same_image_cooldown_seconds, clock=lambda: now[0])
    plugin.replies.clock = lambda: now[0]
    calls = []
    hit = types.SimpleNamespace(duplicate=True, replayed=False, first_message_id="first", first_seen=42, evidence=types.SimpleNamespace(method="pixels"))
    class Worker:
        busy = False
        async def check(self, data, **metadata):
            calls.append(metadata)
            return hit
        async def check_forward(self, *args, **kwargs):
            return []
        check_cards = check_forward
        async def close(self):
            pass
    plugin.worker = Worker()
    class Event:
        def __init__(self, mid, sender="u", group="g", sources=("same.png",)):
            self.message_obj = types.SimpleNamespace(message_id=mid, raw_message={"message": [
                {"type": "image", "data": {"file": source}} for source in sources]})
            self.sender, self.group = sender, group
        get_platform_name = lambda self: "aiocqhttp"
        get_group_id = lambda self: self.group
        get_sender_id = lambda self: self.sender
        get_self_id = lambda self: "bot"
        get_messages = lambda self: []
        plain_result = lambda self, text: text
    async def run(mid, **kwargs):
        return [item async for item in plugin.on_message(Event(mid, **kwargs))]
    return plugin, run, now, downloads, calls


def test_trigger_ban_counts_cached_hits_skips_all_work_and_recovers(monkeypatch, tmp_path):
    plugin, run, now, downloads, calls = harness(monkeypatch, tmp_path, {"trigger_image_count": 3, "sender_pause_seconds": 10, "image_reply_template": "火星 {first_message_id}"})
    async def exercise():
        assert await run("1") == ["火星 first"]
        assert await run("1") == []  # Host replay cannot inflate the counter.
        assert await run("2") == []  # Reply cooldown still counts this hit.
        assert not plugin.limiter.blocked(("qq:bot:g", "u"))
        assert await run("3") == []
        assert plugin.limiter.blocked(("qq:bot:g", "u"))
        assert len(downloads) == len(calls) == 1
        assert await run("4", sources=("new.png",)) == []
        assert len(downloads) == len(calls) == 1
        await run("5", sender="other", sources=("new.png",))
        assert len(downloads) == 2 and len(calls) == 1  # Different source, same bytes: no decode.
        now[0] = 10
        await run("6")
        assert not plugin.limiter.blocked(("qq:bot:g", "u"))
        now[0] = 70
        await run("7")
        assert len(calls) == 2
        await plugin.terminate()
    asyncio.run(exercise())


def test_multiple_images_count_individually_and_group_isolation(monkeypatch, tmp_path):
    plugin, run, _, downloads, _ = harness(monkeypatch, tmp_path, {"trigger_image_count": 2})
    async def exercise():
        assert len(await run("1", sources=("a", "b", "c"))) == 1
        assert plugin.limiter.blocked(("qq:bot:g", "u"))
        assert len(downloads) == 2  # Stop the current message as soon as paused.
        assert len(await run("2", group="other")) == 1
        assert not plugin.limiter.blocked(("qq:bot:other", "u"))
    asyncio.run(exercise())


def test_busy_download_does_not_create_a_queue(monkeypatch, tmp_path):
    plugin, run, _, downloads, _ = harness(monkeypatch, tmp_path)
    original = plugin._check_image
    async def exercise():
        entered, release = asyncio.Event(), asyncio.Event()
        async def slow(*args):
            entered.set()
            await release.wait()
            return await original(*args)
        plugin._check_image = slow
        first = asyncio.create_task(run("1"))
        await entered.wait()
        assert await run("2", sender="other") == []
        assert downloads == []
        release.set()
        await first
        assert len(downloads) == 1
        plugin.worker.busy = True
        assert await run("3", sender="third") == []
        assert len(downloads) == 1
    asyncio.run(exercise())
