import asyncio
import copy
import json
import types

import pytest

from huoxing.forward_content import ForwardContentReader


def outer(resource):
    return {"type": "forward", "data": {"id": resource}}


def node(text="hello", sender=123):
    return {"sender": {"user_id": sender, "nickname": "before"}, "time": 1,
            "message_id": 100, "message": [{"type": "text", "data": {"text": text}}]}


def fingerprint(response):
    async def run():
        async def fetch(_):
            return response
        return await ForwardContentReader().read([outer("r")], fetch, scope="g")
    return asyncio.run(run())


def test_changed_resource_time_nickname_and_segmentation_preserve_whole_identity():
    one = {"messages": [node("hello"), node("world", 456)]}
    two = copy.deepcopy(one)
    two["messages"][0].update(time=200, message_id=999)
    two["messages"][0]["sender"]["nickname"] = "after"
    two["messages"][1]["message"] = [{"type": "text", "data": {"text": "wor"}}, {"type": "text", "data": {"text": "ld"}}]
    async def run():
        async def fetch(resource):
            return one if resource == "a" else two
        result = await ForwardContentReader().read([outer("a"), outer("b")], fetch, scope="g")
        assert result[0] == result[1]
    asyncio.run(run())
    baseline = fingerprint(one)
    for changed in (
        {"messages": list(reversed(one["messages"]))},
        {"messages": [node("different"), node("world", 456)]},
        {"messages": [node("hello", 999), node("world", 456)]},
    ):
        assert fingerprint(changed) != baseline


def test_official_nodes_and_snowluma_messages_have_same_fingerprint():
    snow = {"messages": [node()]}
    official = {"message": [{"type": "node", "data": {"uin": "123", "name": "display", "content": node()["message"]}}]}
    assert fingerprint(snow) == fingerprint(official)


def test_media_identifiers_without_downloading_and_known_card_identity():
    base = node()
    base["message"] = [{"type": "image", "data": {"file": "a"*32+".jpg", "url": "https://media.example/a?key=old"}}]
    changed = copy.deepcopy(base)
    changed["message"][0]["data"]["url"] = "https://media.example/a?key=new"
    assert fingerprint({"messages": [base]}) == fingerprint({"messages": [changed]})
    changed["message"][0]["data"]["file"] = "b"*32+".jpg"
    assert fingerprint({"messages": [base]}) != fingerprint({"messages": [changed]})
    for url in ("https://music.163.com/#/song?id=123&tracking=a", "https://y.music.163.com/m/song?id=123&tracking=b"):
        segment = {"type": "json", "data": {"data": json.dumps({"app": "com.tencent.music.lua", "meta": {"music": {"jumpUrl": url}}})}}
        changed["message"] = [segment]
        if "tracking=a" in url:
            first = fingerprint({"messages": [changed]})
        else:
            assert fingerprint({"messages": [changed]}) == first


@pytest.mark.parametrize("payload", [
    {"messages": []}, {"messages": [node()]*201},
    {"messages": [dict(node(), message=node()["message"]*2001)]},
    {"messages": [node("x"*(1024*1024))]},
    {"messages": [dict(node(), message=[{"type": "image", "data": {"file": "image.png", "url": "https://example.com"}}])]},
    {"messages": [dict(node(), message=[{"type": "unknown", "data": {}}])]},
    {"messages": [{"message": node()["message"]}]},
    {"status": "failed", "retcode": 1, "data": {"messages": [node()]}},
])
def test_never_hashes_partial_or_unsupported_content(payload):
    assert fingerprint(payload) == {}


def test_resource_cache_is_scoped_and_does_not_retain_bodies():
    async def run():
        reader, calls = ForwardContentReader(), []
        async def fetch(resource):
            calls.append(resource)
            return {"messages": [node()]}
        first = await reader.read([outer("same")], fetch, scope="g")
        assert await reader.read([outer("same")], fetch, scope="g") == first
        assert calls == ["same"]
        await reader.read([outer("same")], fetch, scope="other")
        assert calls == ["same", "same"]
        assert all(isinstance(result, str) and len(result) == 64 for _, result in reader.cache.entries.values())
    asyncio.run(run())


def test_request_limit_cycle_depth_and_nested_content():
    async def run():
        calls, warnings = [], []
        async def fetch(resource):
            calls.append(resource)
            return {"messages": [node()]}
        result = await ForwardContentReader().read([outer(str(i)) for i in range(5)], fetch, scope="g", warn=lambda *args: warnings.append(args))
        assert len(result) == len(calls) == 4 and warnings
        async def cyclic(resource):
            return {"messages": [dict(node(), message=[outer(resource)])]}
        assert await ForwardContentReader().read([outer("cycle")], cyclic, scope="g") == {}
        async def deep(resource):
            return {"messages": [dict(node(), message=[outer(str(int(resource)+1))])]}
        assert await ForwardContentReader().read([outer("0")], deep, scope="g") == {}
        async def nested(resource):
            return {"messages": [node()]} if resource == "leaf" else {"messages": [dict(node(), message=[outer("leaf")])]}
        assert await ForwardContentReader().read([outer("parent")], nested, scope="g")
    asyncio.run(run())


def test_total_timeout_falls_back_and_cancellation_propagates(monkeypatch):
    import huoxing.forward_content as content
    monkeypatch.setattr(content, "TOTAL_TIMEOUT", 0.01)
    async def run():
        async def slow(_):
            await asyncio.sleep(10)
        assert await ForwardContentReader().read([outer("r")], slow, scope="g") == {}
        entered = asyncio.Event()
        async def pending(_):
            entered.set()
            await asyncio.Event().wait()
        task = asyncio.create_task(ForwardContentReader().read([outer("r")], pending, scope="g"))
        await entered.wait()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
    asyncio.run(run())


def test_native_whole_pack_replay_and_scope(tmp_path):
    from huoxing import Detector
    from huoxing.forward import check_top_level
    detector = Detector(tmp_path / "pack.sqlite")
    digest = fingerprint({"messages": [node()]})[0]
    def check(resource, mid, scope="g"):
        return check_top_level(detector, [outer(resource)], scope=scope, message_id=mid, content_fingerprints={0: digest})[0]
    assert check("changed-a", "1").status == "new_resource"
    hit = check("changed-b", "2")
    assert hit.status == "same_resource" and hit.first_message_id == "1"
    assert check("changed-b", "2").replayed
    assert check("changed-c", "3", "other").status == "new_resource"
    detector.delete_scope("g")
    assert check("changed-d", "4").status == "new_resource"
