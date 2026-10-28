import hashlib
import sqlite3

from huoxing import Detector
from conftest import encode, fixture


def test_rgb_digest_preserves_database_format(tmp_path):
    image = fixture()
    path = tmp_path / "compatibility.sqlite"
    detector = Detector(path)
    detector.ingest(encode(image), scope="group", message_id="original")

    # Existing databases use this exact size prefix followed by unmodified RGB bytes.
    expected = hashlib.sha256(
        f"{image.width}x{image.height}:".encode("ascii") + image.tobytes()
    ).hexdigest()
    with sqlite3.connect(path) as connection:
        actual, = connection.execute("SELECT digest FROM images").fetchone()
    assert actual == expected


def test_utf8_metadata_survives_borrowed_native_views(tmp_path):
    detector = Detector(tmp_path / "unicode.sqlite")
    data = encode(fixture())
    detector.ingest(data, scope="qq:机器人:群一", message_id="原图-😊", sender_id="群友甲")

    result = detector.ingest(data, scope="qq:机器人:群一", message_id="搬运-🚀")
    assert result.duplicate
    assert result.first_message_id == "原图-😊"
    assert result.first_sender_id == "群友甲"
