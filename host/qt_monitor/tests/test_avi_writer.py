from __future__ import annotations

import struct
import tempfile
from pathlib import Path
import unittest

from recording.avi_writer import MjpegAviWriter, recover_incomplete_avi


class AviWriterTests(unittest.TestCase):
    def test_writes_indexed_mjpeg_avi(self) -> None:
        jpeg = b"\xff\xd8test-jpeg\xff\xd9"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.avi"
            with MjpegAviWriter(path, 640, 480, 15) as writer:
                writer.add_frame(jpeg)
                writer.add_frame(jpeg)
            data = path.read_bytes()

        self.assertEqual(data[:4], b"RIFF")
        self.assertEqual(data[8:12], b"AVI ")
        self.assertIn(b"MJPG", data)
        self.assertIn(b"movi", data)
        self.assertIn(b"idx1", data)
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data) - 8)
        avih = data.index(b"avih") + 8
        self.assertEqual(struct.unpack_from("<I", data, avih + 16)[0], 2)
        idx1 = data.index(b"idx1")
        self.assertEqual(struct.unpack_from("<I", data, idx1 + 4)[0], 32)

    def test_rejects_non_jpeg_frame(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            writer = MjpegAviWriter(Path(directory) / "bad.avi", 640, 480)
            with self.assertRaises(ValueError):
                writer.add_frame(b"not jpeg")
            writer.close()

    def test_recovers_interrupted_segment(self) -> None:
        jpeg = b"\xff\xd8recover-me\xff\xd9"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "interrupted.part.avi"
            writer = MjpegAviWriter(path, 320, 240, 10)
            writer.add_frame(jpeg)
            writer.add_frame(jpeg)
            writer._file.flush()
            writer._file.close()
            frame_count = recover_incomplete_avi(path)
            data = path.read_bytes()
        self.assertEqual(frame_count, 2)
        self.assertIn(b"idx1", data)
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data) - 8)


if __name__ == "__main__":
    unittest.main()
