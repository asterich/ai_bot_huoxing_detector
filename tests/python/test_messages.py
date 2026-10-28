import re
import sqlite3

import pytest
from huoxing import (DetectionStatus, Detector, ForwardMessage, ImageMessage,
                     Message, MessageContext)
from conftest import encode, fixture


def message(payload, message_id, scope="g", sender="alice"):
    return Message(MessageContext(scope, message_id, sender), payload)


def test_one_detector_handles_both_payload_types(tmp_path):
    detector = Detector(tmp_path / "messages.sqlite")
    image = ImageMessage(encode(fixture()))
    original = detector.detect(message(image, "image-1"))
    repeated = detector.detect(message(image, "image-2"))
    assert repeated.status == DetectionStatus.Duplicate
    assert repeated.first.message_id == "image-1"
    assert original.message_hash == repeated.message_hash
    assert original.hash_basis == "decoded_pixels"

    forward = ForwardMessage("onebot11:forward-id", "resource-A")
    first = detector.detect(message(forward, "forward-1"))
    second = detector.detect(message(forward, "forward-2"))
    assert first.status == DetectionStatus.New
    assert second.status == DetectionStatus.Duplicate
    assert second.first.message_id == "forward-1"
    assert second.first.sender_id == "alice"
    assert first.message_hash == second.message_hash
    assert second.hash_basis == "forward_resource"
    assert first.message_hash != original.message_hash
    for result in (original, repeated, first, second):
        assert re.fullmatch(r"[0-9a-f]{64}", result.message_hash)
        assert result.fingerprint.algorithm == "sha256"
        assert result.fingerprint.version == 1


@pytest.mark.parametrize("kind", ["image", "forward"])
def test_content_identity_excludes_delivery_metadata(tmp_path, kind):
    detector = Detector(tmp_path / "messages.sqlite")
    payload = (ImageMessage(encode(fixture())) if kind == "image"
               else ForwardMessage("onebot11:forward-id", "A"))
    first = detector.detect(message(payload, "1", scope="group-1", sender="alice"))
    second = detector.detect(message(payload, "2", scope="group-2", sender="bob"))
    assert first.message_hash == second.message_hash
    assert not second.duplicate  # Search remains group-scoped despite equal identity hashes.


def test_length_framing_disambiguates_delimiters(tmp_path):
    detector = Detector(tmp_path / "messages.sqlite")
    first = detector.detect(message(ForwardMessage("ab:c", "d"), "1"))
    second = detector.detect(message(ForwardMessage("ab", "c:d"), "2"))
    assert first.message_hash != second.message_hash
    assert not second.duplicate


def test_preview_hash_is_only_weak_evidence(tmp_path):
    detector = Detector(tmp_path / "messages.sqlite")
    first = detector.detect(message(ForwardMessage("qq", "A", "same preview"), "1"))
    possible = detector.detect(message(ForwardMessage("qq", "B", "same preview"), "2"))
    exact = detector.detect(message(ForwardMessage("qq", "B", "same preview"), "3"))
    assert possible.status == DetectionStatus.PossibleSameCard
    assert not possible.duplicate
    assert possible.message_hash != first.message_hash
    assert possible.evidence.method == "visible_card"
    assert exact.duplicate and exact.first.message_id == "2"


def test_missing_content_still_has_an_occurrence_hash(tmp_path):
    detector = Detector(tmp_path / "messages.sqlite")
    payload = ForwardMessage("qq", "")
    first = detector.detect(message(payload, "1"))
    replay = detector.detect(message(payload, "1"))
    other = detector.detect(message(payload, "2"))
    assert first.status == DetectionStatus.InsufficientEvidence
    assert first.hash_basis == "occurrence"
    assert replay.replayed and replay.message_hash == first.message_hash
    assert other.message_hash != first.message_hash and not other.duplicate


def test_replay_hash_comes_from_stored_image(tmp_path):
    detector = Detector(tmp_path / "messages.sqlite")
    first = detector.detect(message(ImageMessage(encode(fixture())), "1"))
    replay = detector.detect(message(ImageMessage(encode(fixture(1))), "1"))
    assert replay.replayed
    assert replay.message_hash == first.message_hash


@pytest.mark.parametrize("kind", ["image", "forward"])
def test_lazy_index_preserves_legacy_first_occurrence(tmp_path, kind):
    path = tmp_path / "messages.sqlite"
    detector = Detector(path)
    payload = (ImageMessage(encode(fixture())) if kind == "image"
               else ForwardMessage("qq", "A", "preview"))
    first = detector.detect(message(payload, "original"))
    with sqlite3.connect(path) as connection:
        connection.execute("DELETE FROM message_hashes")
    second = detector.detect(message(payload, "second"))
    third = detector.detect(message(payload, "third"))
    assert second.first.message_id == third.first.message_id == "original"
    assert first.message_hash == second.message_hash == third.message_hash


def test_hash_index_rolls_back_with_capacity_failure(tmp_path):
    path = tmp_path / "messages.sqlite"
    detector = Detector(path, max_images=1)
    detector.detect(message(ImageMessage(encode(fixture())), "1"))
    with pytest.raises(RuntimeError, match="capacity"):
        detector.detect(message(ImageMessage(encode(fixture(1))), "2"))
    with sqlite3.connect(path) as connection:
        assert connection.execute("SELECT COUNT(*) FROM message_hashes").fetchone()[0] == 1
        assert connection.execute("SELECT COUNT(*) FROM occurrences").fetchone()[0] == 1


def test_scope_cleanup_is_atomic_for_both_message_types(tmp_path):
    path = tmp_path / "messages.sqlite"
    detector = Detector(path)
    detector.detect(message(ImageMessage(encode(fixture())), "image"))
    payload = ForwardMessage("qq", "A", "preview")
    detector.detect(message(payload, "forward-1"))
    detector.detect(message(payload, "forward-2"))
    detector.delete_scope("g")
    with sqlite3.connect(path) as connection:
        for table in ("images", "occurrences", "forward_occurrences", "message_hashes"):
            assert connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] == 0
        assert not connection.execute("PRAGMA foreign_key_check").fetchall()
