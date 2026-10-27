"""用最小宿主替身执行事件入口；不冒充真实 QQ 端到端测试。"""
import asyncio
import base64
import importlib.util
import sys
import types
from pathlib import Path

from conftest import encode, fixture


def module(monkeypatch, name, **members):
    value = types.ModuleType(name)
    for key, member in members.items():
        setattr(value, key, member)
    monkeypatch.setitem(sys.modules, name, value)
    return value


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def test_astrbot_entrypoint(monkeypatch, tmp_path):
    class Star:
        def __init__(self, context):
            pass
    class Image:
        async def convert_to_file_path(self):
            return str(tmp_path / "image.png")
    class Node:
        @property
        def content(self):
            raise AssertionError("Expanded forwarded content")
    filters = types.SimpleNamespace(
        EventMessageType=types.SimpleNamespace(GROUP_MESSAGE=1),
        event_message_type=lambda *args: lambda function: function)
    logger = types.SimpleNamespace(warning=lambda *args: None)
    module(monkeypatch, "astrbot")
    module(monkeypatch, "astrbot.api", logger=logger)
    module(monkeypatch, "astrbot.api.event", AstrMessageEvent=object, filter=filters)
    module(monkeypatch, "astrbot.api.message_components", Image=Image)
    module(monkeypatch, "astrbot.api.star", Star=Star, Context=object,
           StarTools=types.SimpleNamespace(get_data_dir=lambda: tmp_path))
    plugin = load("test_astr_plugin", Path(__file__).parents[2] / "plugins/astrbot/main.py").HuoxingPlugin(None)
    (tmp_path / "image.png").write_bytes(encode(fixture()))
    class Event:
        message_obj = types.SimpleNamespace(message_id="1")
        get_group_id = lambda self: "group"
        get_self_id = lambda self: "bot"
        get_sender_id = lambda self: "user"
        get_messages = lambda self: [Node(), Image()]
        plain_result = lambda self, text: text
    async def exercise():
        event = Event()
        assert [item async for item in plugin.on_message(event)] == []
        event.message_obj = types.SimpleNamespace(message_id="2")
        assert len([item async for item in plugin.on_message(event)]) == 1
        assert [item async for item in plugin.on_message(event)] == []
        await plugin.terminate()
    asyncio.run(exercise())


def test_maibot_entrypoint(monkeypatch, tmp_path):
    module(monkeypatch, "maibot_sdk", MaiBotPlugin=object,
           HookHandler=lambda *args, **kwargs: lambda function: function)
    module(monkeypatch, "maibot_sdk.types", HookMode=types.SimpleNamespace(OBSERVE="observe"))
    plugin = load("test_mai_plugin", Path(__file__).parents[2] / "plugins/maibot/plugin.py").create_plugin()
    sent = []
    async def send_text(text, stream_id):
        sent.append((text, stream_id))
    logger = types.SimpleNamespace(warning=lambda *args: None, info=lambda *args: None)
    plugin.ctx = types.SimpleNamespace(paths=types.SimpleNamespace(data_dir=tmp_path), logger=logger,
                                      send=types.SimpleNamespace(text=send_text))
    payload = base64.b64encode(encode(fixture())).decode()
    async def exercise():
        await plugin.on_load()
        message = {"message_info": {"group_info": {"group_id": "g"},
                    "message_id": "1", "user_info": {"user_id": "u"}},
                   "raw_message": [{"type": "image", "data": {"binary_data_base64": payload}}]}
        await plugin.detect_images(message, stream_id="s")
        message["message_info"]["message_id"] = "2"
        await plugin.detect_images(message, stream_id="s")
        assert len(sent) == 1 and sent[0][1] == "s"
        await plugin.detect_images(message, stream_id="s")
        assert len(sent) == 1
        # 位于 node 内的媒体不应进入图片数据库或产生回复。
        message["message_info"]["message_id"] = "3"
        message["raw_message"] = [{"type": "node", "data": {"content": [
            {"type": "image", "data": {"binary_data_base64": payload}}]}}]
        await plugin.detect_images(message, stream_id="s")
        assert len(sent) == 1
        await plugin.on_unload()
    asyncio.run(exercise())
