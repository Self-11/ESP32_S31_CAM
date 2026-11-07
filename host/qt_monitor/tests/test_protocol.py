import struct
import unittest

from transport.protocol import (
    HEADER_SIZE,
    MAGIC,
    MessageType,
    StreamParser,
    crc32_be,
    decode_face_results,
    decode_face_threshold_ack,
    encode_face_threshold,
)


def make_message(payload: bytes, sequence: int = 7) -> bytes:
    prefix = struct.pack(
        ">4sBBHIQHHI",
        MAGIC,
        1,
        MessageType.JPEG_FRAME,
        0,
        sequence,
        123456,
        640,
        480,
        len(payload),
    )
    crc = crc32_be(0xFFFFFFFF, prefix)
    return prefix + struct.pack(">I", crc) + payload


class ProtocolTests(unittest.TestCase):
    def test_header_is_fixed_size(self) -> None:
        self.assertEqual(len(make_message(b"")[:HEADER_SIZE]), HEADER_SIZE)

    def test_fragmented_message(self) -> None:
        wire = make_message(b"jpeg")
        parser = StreamParser()
        self.assertEqual(parser.feed(wire[:9]), [])
        messages = parser.feed(wire[9:])
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0].payload, b"jpeg")
        self.assertEqual(messages[0].header.sequence, 7)

    def test_coalesced_messages_and_resync(self) -> None:
        parser = StreamParser()
        messages = parser.feed(b"noise" + make_message(b"a", 1) + make_message(b"b", 2))
        self.assertEqual([message.payload for message in messages], [b"a", b"b"])

    def test_decode_face_results(self) -> None:
        payload = struct.pack(">IB3x6H", 99, 1, 10, 20, 30, 40, 8750, 0)
        result = decode_face_results(payload)
        self.assertEqual(result.source_sequence, 99)
        self.assertEqual(len(result.boxes), 1)
        self.assertEqual(result.boxes[0].width, 30)
        self.assertAlmostEqual(result.boxes[0].score, 0.875)

    def test_face_threshold_round_trip(self) -> None:
        wire = encode_face_threshold(0.35)
        messages = StreamParser().feed(wire)
        self.assertEqual(messages[0].header.message_type, MessageType.FACE_THRESHOLD_SET)
        self.assertEqual(messages[0].payload, struct.pack(">H", 350))
        self.assertAlmostEqual(decode_face_threshold_ack(messages[0].payload), 0.35)


if __name__ == "__main__":
    unittest.main()
