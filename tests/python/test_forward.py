import json
import sqlite3

from huoxing import Detector
from huoxing.forward import check_top_level, envelope


def card(resid, **extra):
    detail = {"resid": resid, "source": "聊天记录", "summary": "查看2条消息",
              "news": [{"text": "群友: [图片]"}], **extra}
    return {"type": "json", "data": {"data": json.dumps(
        {"app": "com.tencent.multimsg", "meta": {"detail": detail}})}}


def check(d, segments, msg="1", group="g"):
    return check_top_level(d, segments, scope=group, message_id=msg)


def test_same_resource_and_changed_id(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    assert check(d, [card("abc")])[0].status == "new_resource"
    r = check(d, [card("abc", uniseq="changed")], "2")[0]
    assert r.status == "same_resource" and r.first_message_id == "1"
    assert check(d, [card("def")], "3")[0].status == "possible_same_card"
    assert check(d, [card("abc")], "2")[0].replayed
    assert check(d, [card("abc")], group="other")[0].status == "new_resource"


def test_does_not_descend(tmp_path):
    class Poison(dict):
        def get(self, *args):
            raise AssertionError("Touched nested messages")
    d = Detector(tmp_path / "db.sqlite")
    segments = [{"type": "node", "data": Poison()},
                {"type": "forward", "data": {"id": "pack", "content": Poison()}}]
    r = check(d, segments)
    assert len(r) == 1 and r[0].status == "new_resource"
    with sqlite3.connect(tmp_path / "db.sqlite") as db:
        assert db.execute("SELECT COUNT(*) FROM images").fetchone()[0] == 0


def test_unknown_and_preview_collision(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    assert check(d, [{"type": "forward", "data": {}}])[0].status == "insufficient_evidence"
    assert check(d, [card("")], "2")[0].status == "insufficient_evidence"
    assert check(d, [card("")], "3")[0].status == "possible_same_card"
    assert envelope({"type": "json", "data": {"data": "{"}}) is None
    assert envelope({"type": "json", "data": {"data": "x" * 65537}}) is None


def test_clean_scope(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    check(d, [card("abc")])
    check(d, [card("abc")], "2")
    d.delete_scope("g")
    assert check(d, [card("abc")])[0].status == "new_resource"
