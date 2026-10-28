"""Transport-independent value objects. Forward payloads expose no nested message nodes."""
from dataclasses import dataclass


@dataclass(frozen=True)
class MessageContext:
    scope: str
    message_id: str
    sender_id: str = ""
    part: int = 0


@dataclass(frozen=True)
class ImageMessage:
    data: bytes


@dataclass(frozen=True)
class ForwardMessage:
    resource_namespace: str
    resource_id: str
    visible_card: str = ""


@dataclass(frozen=True)
class Message:
    context: MessageContext
    payload: ImageMessage | ForwardMessage
