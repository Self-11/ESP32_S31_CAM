from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct


MAGIC = b"S31V"
VERSION = 1
HEADER_SIZE = 32
MAX_PAYLOAD_SIZE = 2 * 1024 * 1024
_HEADER_STRUCT = struct.Struct(">4sBBHIQHHII")


class MessageType(IntEnum):
    HELLO = 1
    CAPABILITIES = 2
    JPEG_FRAME = 3
    SET_MODE = 4
    START = 5
    STOP = 6
    SNAPSHOT = 7
    RECORD_START = 8
    RECORD_STOP = 9
    STATUS = 10
    ERROR = 11
    FACE_RESULTS = 12
    FACE_ENROLL = 20
    FACE_DELETE = 21
    FACE_CLEAR = 22
    FACE_LIST = 23
    FACE_ENROLL_ACK = 24
    FACE_LIST_REPLY = 25
    FACE_THRESHOLD_SET = 26
    FACE_THRESHOLD_ACK = 27


@dataclass(slots=True)
class FaceBox:
    x: int
    y: int
    width: int
    height: int
    score: float
    recognition_id: int = 0
    recognition_confidence: float = 0.0
    label: str = ""

    @property
    def is_recognized(self) -> bool:
        return self.recognition_id > 0


@dataclass(frozen=True, slots=True)
class FaceResults:
    source_sequence: int
    boxes: tuple[FaceBox, ...]


@dataclass(frozen=True, slots=True)
class FrameHeader:
    message_type: MessageType
    flags: int
    sequence: int
    timestamp_us: int
    width: int
    height: int
    payload_length: int
    header_crc32: int


@dataclass(frozen=True, slots=True)
class Message:
    header: FrameHeader
    payload: bytes


class ProtocolError(ValueError):
    pass


def decode_face_results(payload: bytes) -> FaceResults:
    if len(payload) < 8:
        raise ProtocolError("face results payload is too short")
    source_sequence, count = struct.unpack_from(">IB", payload)
    expected_min = 8 + count * 12
    expected_v2 = 8 + count * 18
    if count > 4 or (len(payload) != expected_min and len(payload) != expected_v2):
        raise ProtocolError(
            f"invalid face results payload: count={count}, size={len(payload)}"
        )
    boxes = []
    is_v2 = len(payload) == expected_v2
    for index in range(count):
        stride = 18 if is_v2 else 12
        x, y, width, height, score_x10000, _reserved = struct.unpack_from(
            ">6H", payload, 8 + index * stride
        )
        recog_id = 0
        recog_conf = 0.0
        if is_v2:
            recog_id, conf_bits = struct.unpack_from(
                ">HI", payload, 8 + index * 18 + 12
            )
            recog_conf = struct.unpack(">f", struct.pack(">I", conf_bits))[0]
        boxes.append(FaceBox(x, y, width, height, score_x10000 / 10000.0,
                             recog_id, recog_conf))
    return FaceResults(source_sequence, tuple(boxes))


def crc32_be(initial: int, data: bytes | bytearray | memoryview) -> int:
    """Match ESP-IDF esp_crc32_be(), including its initial/final inversion."""
    crc = (~initial) & 0xFFFFFFFF
    for value in data:
        crc ^= value << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def decode_header(data: bytes) -> FrameHeader:
    if len(data) != HEADER_SIZE:
        raise ProtocolError(f"invalid header size: {len(data)}")

    (magic, version, raw_type, flags, sequence, timestamp_us, width, height,
     payload_length, received_crc) = _HEADER_STRUCT.unpack(data)
    if magic != MAGIC:
        raise ProtocolError("invalid magic")
    if version != VERSION:
        raise ProtocolError(f"unsupported protocol version: {version}")
    calculated_crc = crc32_be(0xFFFFFFFF, data[:28])
    if received_crc != calculated_crc:
        raise ProtocolError("header CRC mismatch")
    try:
        message_type = MessageType(raw_type)
    except ValueError as exc:
        raise ProtocolError(f"unknown message type: {raw_type}") from exc
    if payload_length > MAX_PAYLOAD_SIZE:
        raise ProtocolError(f"payload too large: {payload_length}")

    return FrameHeader(
        message_type=message_type,
        flags=flags,
        sequence=sequence,
        timestamp_us=timestamp_us,
        width=width,
        height=height,
        payload_length=payload_length,
        header_crc32=received_crc,
    )


def encode_header(msg_type: MessageType, payload_length: int,
                  sequence: int = 0, timestamp_us: int = 0,
                  width: int = 0, height: int = 0) -> bytes:
    header_data = _HEADER_STRUCT.pack(
        MAGIC, VERSION, int(msg_type), 0,
        sequence, timestamp_us, width, height, payload_length, 0,
    )
    crc = crc32_be(0xFFFFFFFF, header_data[:28])
    return header_data[:28] + struct.pack(">I", crc)


def encode_face_enroll(face_index: int) -> bytes:
    header = encode_header(MessageType.FACE_ENROLL, 1)
    return header + bytes([face_index])


def encode_face_enroll_upload(name: str, rgb565: bytes,
                               crop_w: int, crop_h: int) -> bytes:
    name_bytes = name.encode("utf-8")
    if len(name_bytes) > 255:
        raise ValueError("name too long (max 255 bytes)")
    payload = bytearray(6 + len(name_bytes) + len(rgb565))
    payload[0] = 1  # version = upload mode
    payload[1] = len(name_bytes)
    payload[2:2 + len(name_bytes)] = name_bytes
    struct.pack_into(">HH", payload, 2 + len(name_bytes), crop_w, crop_h)
    payload[6 + len(name_bytes):] = rgb565
    header = encode_header(MessageType.FACE_ENROLL, len(payload))
    return header + bytes(payload)


def decode_enroll_ack(payload: bytes) -> int:
    if len(payload) < 2:
        raise ProtocolError("enroll ack payload too short")
    return struct.unpack_from(">H", payload)[0]


def encode_face_delete(face_id: int) -> bytes:
    header = encode_header(MessageType.FACE_DELETE, 2)
    return header + struct.pack(">H", face_id)


def encode_face_clear() -> bytes:
    return encode_header(MessageType.FACE_CLEAR, 0)

def encode_face_list() -> bytes:
    return encode_header(MessageType.FACE_LIST, 0)

def decode_face_list_reply(payload: bytes) -> list[tuple[int, str]]:
    if len(payload) < 2:
        return []
    count = (payload[0] << 8) | payload[1]
    entries: list[tuple[int, str]] = []
    offset = 2
    for _ in range(count):
        if offset + 3 > len(payload):
            break
        entry_id = (payload[offset] << 8) | payload[offset + 1]
        offset += 2
        name_len = payload[offset]
        offset += 1
        if offset + name_len > len(payload):
            break
        name = payload[offset:offset + name_len].decode("utf-8")
        offset += name_len
        entries.append((entry_id, name))
    return entries


def encode_face_threshold(threshold: float) -> bytes:
    if not 0.0 <= threshold <= 1.0:
        raise ValueError("face threshold must be between 0 and 1")
    threshold_x1000 = round(threshold * 1000)
    payload = struct.pack(">H", threshold_x1000)
    return encode_header(MessageType.FACE_THRESHOLD_SET, len(payload)) + payload


def decode_face_threshold_ack(payload: bytes) -> float:
    if len(payload) != 2:
        raise ProtocolError("face threshold ack must contain 2 bytes")
    return struct.unpack(">H", payload)[0] / 1000.0


class StreamParser:
    """Incremental parser that handles TCP fragmentation, coalescing and resync."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def reset(self) -> None:
        self._buffer.clear()

    def feed(self, data: bytes) -> list[Message]:
        self._buffer.extend(data)
        messages: list[Message] = []

        while len(self._buffer) >= HEADER_SIZE:
            if not self._buffer.startswith(MAGIC):
                position = self._buffer.find(MAGIC, 1)
                if position < 0:
                    del self._buffer[:-3]
                    break
                del self._buffer[:position]
                if len(self._buffer) < HEADER_SIZE:
                    break

            try:
                header = decode_header(bytes(self._buffer[:HEADER_SIZE]))
            except ProtocolError:
                del self._buffer[0]
                continue

            message_size = HEADER_SIZE + header.payload_length
            if len(self._buffer) < message_size:
                break
            payload = bytes(self._buffer[HEADER_SIZE:message_size])
            del self._buffer[:message_size]
            messages.append(Message(header, payload))

        return messages
