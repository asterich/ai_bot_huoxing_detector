import json
import sqlite3

import pytest

from huoxing import Detector
from huoxing.cards import card_identity, check_cards, format_card_hit, identity


def card(url, title="same title", mini=False):
    payload = ({"app": "com.tencent.miniapp_01", "meta": {"detail_1": {"qqdocurl": url, "title": title}}}
               if mini else {"app": "com.tencent.structmsg", "meta": {"news": {"jumpUrl": url, "title": title}}})
    return {"type": "json", "data": {"data": json.dumps(payload)}}


def test_bilibili_card_types_tracking_replay_and_scope(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    def check(segment, mid, scope="g"):
        return check_cards(d, [segment], scope=scope, message_id=mid)[0]
    url = "https://www.bilibili.com/video/BV1xx411c7mD"
    first = check(card(url + "?spm_id_from=1"), "1")
    assert first.status == "new_resource"
    second = check(card(url + "?share_source=qq", title="changed", mini=True), "2")
    assert second.status == "same_resource" and second.first_message_id == "1"
    assert format_card_hit(second)
    assert format_card_hit(check(card(url), "2")) is None
    assert check(card(url), "3", "other").status == "new_resource"
    assert check(card(url.replace("c7mD", "c7mE")), "4").status == "new_resource"
    d.delete_scope("g")
    assert check(card(url), "5").status == "new_resource"


def test_wechat_identity_keeps_article_parameters():
    base = "https://mp.weixin.qq.com/s?__biz=abc&mid=123&idx=1&sn=signature"
    assert identity(base) == identity(base + "&scene=21&from=groupmessage#wechat_redirect")
    assert identity(base) != identity(base.replace("idx=1", "idx=2"))
    assert identity(base) != identity(base.replace("__biz=abc", "__biz=def"))
    assert identity(base.replace("&sn=signature", "")) is None
    assert identity(base + "&idx=2") is None
    assert identity("https://mp.weixin.qq.com/s/short_id?scene=21") == identity("http://mp.weixin.qq.com/s/short_id")


def test_generic_links_are_conservative_and_short_links_are_not_resolved():
    assert identity("https://b23.tv/abc") != identity("https://b23.tv/def")
    assert identity("https://example.com/?id=1") != identity("https://example.com/?id=2")
    assert identity("https://example.com/a#first") == identity("https://EXAMPLE.com/a#second")
    assert identity("https://bilibili.com.evil/video/BV1xx411c7mD")[0] == "share:url:v1"


@pytest.mark.parametrize("url", [None, 1, "javascript:alert(1)", "file:///tmp/a", "https://user:pass@example.com/a", "https://example.com:bad/a", "https://example.com:443/a", "https://example.com/\na", "x" * 4097])
def test_invalid_urls(url):
    assert identity(url) is None


def test_xml_json_and_nested_boundaries(tmp_path):
    url = "https://example.com/article?id=1&lang=zh"
    xml = {"type": "xml", "data": {"data": '<msg serviceID="1" url="https://example.com/article?id=1&amp;lang=zh"/>'}}
    assert card_identity(xml) == card_identity(card(url))
    assert card_identity({"type": "json", "data": {"data": "{"}}) is None
    assert card_identity({"type": "xml", "data": {"data": '<!DOCTYPE msg><msg serviceID="1" url="https://example.com"/>'}}) is None
    assert card_identity({"type": "xml", "data": {"data": '<msg serviceID="35" url="https://example.com"/>'}}) is None
    d = Detector(tmp_path / "db.sqlite")
    assert check_cards(d, [{"type": "node", "data": {"content": [card(url)]}}], scope="g", message_id="1") == []
    check_cards(d, [xml], scope="g", message_id="2")
    hit = check_cards(d, [card(url)], scope="g", message_id="3")[0]
    assert hit.status == "same_resource"
    with sqlite3.connect(tmp_path / "db.sqlite") as c:
        assert c.execute("SELECT COUNT(*) FROM images").fetchone()[0] == 0


def test_netease_routes_music_cards_and_different_content():
    expected = identity("https://music.163.com/#/song?id=123&userid=1")
    assert expected == ("share:netease-song:v1", "123")
    assert identity("https://y.music.163.com/m/song?id=123&uct2=tracking") == expected
    assert identity("https://music.163.com/song?id=123") == expected
    assert identity("https://music.163.com/#/song?id=124") != expected
    assert identity("https://music.163.com/#/playlist?id=123") != expected
    assert identity("https://music.163.com/#/song?id=123&id=124") is None
    assert identity("https://music.163.com/#/song?userid=123") is None
    music = {"type": "json", "data": {"data": json.dumps({
        "app": "com.tencent.structmsg", "meta": {"music": {
            "jumpUrl": "https://music.163.com/song?id=123", "musicUrl": "https://cdn.example/audio.mp3"}}})}}
    assert card_identity(music) == expected
    assert card_identity({"type": "xml", "data": {"data": '<msg serviceID="2" url="https://music.163.com/song?id=123"/>'}}) == expected


def test_netease_native_replay_and_scope(tmp_path):
    detector = Detector(tmp_path / "music.sqlite")
    one = card("https://music.163.com/#/song?id=123")
    two = card("https://y.music.163.com/m/song?id=123&userid=2")
    assert check_cards(detector, [one], scope="g", message_id="1")[0].status == "new_resource"
    hit = check_cards(detector, [two], scope="g", message_id="2")[0]
    assert hit.status == "same_resource" and hit.first_message_id == "1"
    assert check_cards(detector, [two], scope="g", message_id="2")[0].replayed
    assert check_cards(detector, [two], scope="other", message_id="3")[0].status == "new_resource"
