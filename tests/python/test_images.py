import io
import sqlite3
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pytest
from PIL import Image
from huoxing import Detector
from conftest import encode, fixture, variants


@pytest.mark.parametrize("kind", ["crop", "border", "watermark", "jpeg", "combined"])
@pytest.mark.parametrize("reverse", [False, True])
def test_modifications(tmp_path, kind, reverse):
    original = encode(fixture())
    modified = variants(fixture())[kind]
    a, b = (modified, original) if reverse else (original, modified)
    d = Detector(tmp_path / "db.sqlite")
    assert not d.ingest(a, scope="g", message_id="1").duplicate
    result = d.ingest(b, scope="g", message_id="2")
    assert result.duplicate and result.evidence.inliers >= 12


@pytest.mark.parametrize("seed", range(6))
def test_negative(tmp_path, seed):
    d = Detector(tmp_path / "db.sqlite")
    d.ingest(encode(fixture()), scope="g", message_id="1")
    assert not d.ingest(encode(fixture(seed)), scope="g", message_id="2").duplicate


def test_small_shared_patch(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    base = fixture()
    other = fixture(55)
    other.paste(base.crop((0, 0, 100, 100)), (0, 0))
    d.ingest(encode(base), scope="g", message_id="1")
    assert not d.ingest(encode(other), scope="g", message_id="2").duplicate


def test_persistence_scope_and_replay(tmp_path):
    path = tmp_path / "db.sqlite"
    im = encode(fixture())
    d = Detector(path)
    assert not d.ingest(im, scope="g", message_id="1", sender_id="u").duplicate
    assert d.ingest(im, scope="g", message_id="1").replayed
    r = Detector(path).ingest(im, scope="g", message_id="2")
    assert r.duplicate and r.first_message_id == "1" and r.first_sender_id == "u"
    assert r.first_seen.endswith("Z")
    assert d.ingest(im, scope="g", message_id="2").replayed
    assert not d.ingest(im, scope="other", message_id="1").duplicate
    d.delete_scope("g")
    assert not d.ingest(im, scope="g", message_id="1").duplicate
    with sqlite3.connect(path) as db:
        assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        assert not db.execute("PRAGMA foreign_key_check").fetchall()


def test_flat_images_and_multipart(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    im = Image.new("RGB", (80, 80), "white")
    d.ingest(encode(im), scope="g", message_id="1", part=0)
    assert d.ingest(encode(im, "BMP"), scope="g", message_id="1", part=1).duplicate
    assert not d.ingest(encode(Image.new("RGB", (80,80), "black")),
                        scope="g", message_id="2").duplicate


def test_concurrency(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    im = encode(fixture())
    with ThreadPoolExecutor(4) as pool:
        results = list(pool.map(lambda n: d.ingest(im, scope="g", message_id=str(n)), range(4)))
    assert sum(not r.duplicate for r in results) == 1


def test_capacity_rollback(tmp_path):
    path = tmp_path / "db.sqlite"
    d = Detector(path, max_images=1)
    d.ingest(encode(fixture()), scope="g", message_id="1")
    with pytest.raises(RuntimeError, match="capacity"):
        d.ingest(encode(fixture(55)), scope="g", message_id="2")
    with sqlite3.connect(path) as db:
        assert db.execute("SELECT COUNT(*) FROM occurrences").fetchone()[0] == 1


@pytest.mark.parametrize("data", [b"", b"invalid", b"x" * (8*1024*1024+1)],
                         ids=["empty", "invalid", "oversize"])
def test_bad_images(tmp_path, data):
    with pytest.raises((ValueError, OSError)):
        Detector(tmp_path / "db.sqlite").ingest(data, scope="g", message_id="1")


def test_animation(tmp_path):
    im = Image.new("RGB", (30,30), "white")
    out = io.BytesIO()
    im.save(out, "GIF", save_all=True, append_images=[Image.new("RGB", (30,30), "black")])
    with pytest.raises(ValueError, match="动图"):
        Detector(tmp_path / "db.sqlite").ingest(out.getvalue(), scope="g", message_id="1")


def test_native_rejects_bad_array(tmp_path):
    d = Detector(tmp_path / "db.sqlite")
    with pytest.raises((TypeError, ValueError)):
        d.native.ingest_rgb(np.zeros((20,20,3), dtype=np.float32), "g", "1")
    with pytest.raises(ValueError):
        d.native.ingest_rgb(np.zeros((10,10,3), dtype=np.uint8), "g", "1")
