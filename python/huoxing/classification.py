"""Classify transport image segments before downloading or decoding media."""


def is_regular_image(segment):
    if not isinstance(segment, dict) or segment.get("type") != "image":
        return False
    data = segment.get("data")
    if not isinstance(data, dict):
        return False
    # OneBot implementations also report custom/market stickers as image segments.
    if data.get("file") == "marketface":
        return False
    return all(data.get(key) in (None, "", 0, "0") for key in ("sub_type", "subType"))
