"""AstrBot settings and bounded, monotonic admission/cooldown state."""
import math
import time
from collections import OrderedDict, deque
from dataclasses import dataclass
from string import Formatter


IMAGE_TEXT = "这张图之前出现过：消息 {first_message_id}，首次记录 {first_seen}。依据：{method}。"
FORWARD_TEXT = "这个聊天记录包之前出现过，首次消息：{first_message_id}。"
CARD_TEXT = "这张分享卡片的目标链接之前出现过，首次消息：{first_message_id}。"


@dataclass(frozen=True)
class Settings:
    image_reply_template: str = IMAGE_TEXT
    forward_reply_template: str = FORWARD_TEXT
    card_reply_template: str = CARD_TEXT
    trigger_window_seconds: float = 180
    trigger_image_count: float = 3
    sender_pause_seconds: float = 600
    same_image_cooldown_seconds: float = 60
    reply_cooldown_seconds: float = 30
    max_image_mib: float = 8
    max_image_megapixels: float = 16

    @property
    def max_bytes(self):
        return int(self.max_image_mib * 1024 * 1024)

    @property
    def max_pixels(self):
        return int(self.max_image_megapixels * 1_000_000)

    @classmethod
    def from_config(cls, config, warn=lambda *args: None):
        config = config if callable(getattr(config, "get", None)) else {}
        defaults = cls()
        values = {}
        for key in cls.__dataclass_fields__:
            default = getattr(defaults, key)
            value = config.get(key, default)
            try:
                if key.endswith("_template"):
                    if not isinstance(value, str) or not value.strip() or len(value) > 2000:
                        raise ValueError("文案须为非空文本，最长 2000 字")
                    for _, field, spec, conversion in Formatter().parse(value):
                        if field is not None and (field not in {"first_message_id", "first_seen", "method"} or spec or conversion):
                            raise ValueError("仅支持 {first_message_id}、{first_seen}、{method} 占位符")
                else:
                    if isinstance(value, bool):
                        raise ValueError("须填写数字")
                    value = float(value)
                    if key == "trigger_image_count":
                        if not value.is_integer():
                            raise ValueError("触发图片数须为整数")
                        low, high = 1, 1000
                    elif key in ("max_image_mib", "max_image_megapixels"):
                        low, high = 0.1, 32
                    else:
                        low, high = 0, 86400
                    if not math.isfinite(value) or not low <= value <= high:
                        raise ValueError(f"须在 {low} 到 {high} 之间")
                values[key] = value
            except (TypeError, ValueError) as exc:
                warn("火星配置 %s 无效：%s；使用默认值", key, str(exc))
                values[key] = default
        return cls(**values)

    def reply(self, kind, result):
        template = getattr(self, kind + "_reply_template")
        return template.format(first_message_id=getattr(result, "first_message_id", ""),
                               first_seen=getattr(result, "first_seen", ""),
                               method=getattr(getattr(result, "evidence", None), "method", ""))


class Cooldowns:
    """No timers or task queues; each cache has a fixed maximum size."""
    def __init__(self, capacity=4096, clock=time.monotonic):
        self.entries = OrderedDict()
        self.capacity = capacity
        self.clock = clock

    def blocked(self, key, seconds):
        started = self.entries.get(key)
        return seconds > 0 and started is not None and self.clock() - started < seconds

    def mark(self, key):
        self.entries[key] = self.clock()
        self.entries.move_to_end(key)
        while len(self.entries) > self.capacity:
            self.entries.popitem(last=False)

    def admit(self, limits):
        if any(self.blocked(key, seconds) for key, seconds in limits):
            return False
        for key, seconds in limits:
            if seconds > 0:
                self.mark(key)
        return True


class TriggerLimiter:
    """Sliding window of duplicate images per bot/group/sender, bounded in RAM.

    Zero window or pause disables the ban. Blocked traffic never extends a ban.
    """
    def __init__(self, settings, capacity=4096, clock=time.monotonic):
        self.settings = settings
        self.capacity = capacity
        self.clock = clock
        self.people = OrderedDict()

    def blocked(self, key):
        state = self.people.get(key)
        return state is not None and self.clock() < state[1]

    def record(self, key):
        s, now = self.settings, self.clock()
        if not s.trigger_window_seconds or not s.sender_pause_seconds:
            return False
        hits, until = self.people.get(key, (deque(), 0))
        if now < until:
            return True
        if until:  # A completed pause starts a fresh window.
            hits.clear()
        while hits and now - hits[0] >= s.trigger_window_seconds:
            hits.popleft()
        hits.append(now)
        paused = len(hits) >= s.trigger_image_count
        self.people[key] = (deque() if paused else hits, now + s.sender_pause_seconds if paused else 0)
        self.people.move_to_end(key)
        while len(self.people) > self.capacity:
            self.people.popitem(last=False)
        return paused


class RecentImages:
    """Store duplicate evidence (or a rejected image) to avoid repeated work."""
    def __init__(self, seconds, capacity=4096, clock=time.monotonic):
        self.seconds, self.capacity, self.clock = seconds, capacity, clock
        self.entries = OrderedDict()

    def get(self, key):
        state = self.entries.get(key)
        if state is None:
            return False, None
        when, result = state
        if self.seconds <= 0 or self.clock() - when >= self.seconds:
            del self.entries[key]
            return False, None
        return True, result

    def remember(self, key, result):
        if key is None or self.seconds <= 0:
            return
        self.entries[key] = (self.clock(), result)
        self.entries.move_to_end(key)
        while len(self.entries) > self.capacity:
            self.entries.popitem(last=False)
