from __future__ import annotations

import tempfile
from pathlib import Path
import unittest

from recording.cache_manager import RecordingCacheManager


class RecordingCacheTests(unittest.TestCase):
    def test_multiple_devices_rotate_finalize_and_export(self) -> None:
        jpeg = b"\xff\xd8frame-payload-for-cache-test\xff\xd9"
        with tempfile.TemporaryDirectory() as cache_dir, tempfile.TemporaryDirectory() as export_dir:
            manager = RecordingCacheManager(cache_dir, segment_bytes=320)
            manager.start()
            for _ in range(4):
                manager.write_frame("session-a", "S31CAM-AAAAAA", jpeg, 640, 480, 15)
            manager.write_frame("session-b", "S31CAM-BBBBBB", jpeg, 640, 480, 15)
            manager.finalize_session("session-b", "disconnect")
            manager.stop()

            pending = manager.pending_files()
            self.assertGreaterEqual(len(pending), 3)
            self.assertTrue(all(path.suffix == ".avi" for path in pending))
            exported = manager.export(export_dir)
            self.assertEqual(len(exported), len(pending))
            self.assertEqual(manager.pending_files(), [])
            self.assertTrue(all(Path(path).exists() for path in exported))


if __name__ == "__main__":
    unittest.main()
