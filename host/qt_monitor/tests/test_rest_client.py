from __future__ import annotations

import unittest

from transport.rest_client import parse_json_response


class RestResponseTests(unittest.TestCase):
    def test_parses_status_object(self) -> None:
        status = parse_json_response(
            b'{"ready":true,"device_id":"S31CAM-123456","streaming":true}'
        )
        self.assertTrue(status["ready"])
        self.assertEqual(status["device_id"], "S31CAM-123456")

    def test_rejects_invalid_json_and_non_object(self) -> None:
        with self.assertRaises(ValueError):
            parse_json_response(b"not-json")
        with self.assertRaises(ValueError):
            parse_json_response(b"[]")


if __name__ == "__main__":
    unittest.main()
