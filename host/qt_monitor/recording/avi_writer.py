from __future__ import annotations

from pathlib import Path
import struct
from typing import BinaryIO


_AVI_HAS_INDEX = 0x10
_AVI_KEYFRAME = 0x10
_MAX_RIFF_SIZE = 0xFFFFFF00


def recover_incomplete_avi(path: str | Path) -> int:
    """Rebuild the index of a .part.avi left behind by an interrupted run."""
    source = Path(path)
    with source.open("r+b") as file:
        data = file.read()
        if len(data) < 128 or data[:4] != b"RIFF" or data[8:12] != b"AVI ":
            raise ValueError("not an AVI file")
        avih_tag = data.find(b"avih")
        strh_tag = data.find(b"strh")
        movi_tag = data.find(b"movi")
        if min(avih_tag, strh_tag, movi_tag) < 0:
            raise ValueError("incomplete AVI header")

        cursor = movi_tag + 4
        index: list[tuple[int, int]] = []
        max_frame = 0
        while cursor + 8 <= len(data) and data[cursor:cursor + 4] == b"00dc":
            frame_size = struct.unpack_from("<I", data, cursor + 4)[0]
            chunk_end = cursor + 8 + frame_size + (frame_size & 1)
            if chunk_end > len(data):
                break
            index.append((cursor - movi_tag, frame_size))
            max_frame = max(max_frame, frame_size)
            cursor = chunk_end

        file.seek(cursor)
        file.truncate()
        movi_size_position = movi_tag - 4
        file.seek(movi_size_position)
        file.write(struct.pack("<I", cursor - movi_size_position - 4))
        file.seek(cursor)
        index_data = b"".join(
            struct.pack("<4sIII", b"00dc", _AVI_KEYFRAME, offset, size)
            for offset, size in index
        )
        file.write(b"idx1")
        file.write(struct.pack("<I", len(index_data)))
        file.write(index_data)
        final_size = file.tell()

        avih = avih_tag + 8
        strh = strh_tag + 8
        for position, value in (
            (4, final_size - 8),
            (avih + 16, len(index)),
            (avih + 28, max_frame),
            (strh + 32, len(index)),
            (strh + 36, max_frame),
        ):
            file.seek(position)
            file.write(struct.pack("<I", value))
        file.flush()
    return len(index)


class MjpegAviWriter:
    """Write received JPEG frames directly into a classic indexed MJPEG AVI."""

    def __init__(
        self, path: str | Path, width: int, height: int, fps: int = 15
    ) -> None:
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        if not 1 <= fps <= 120:
            raise ValueError("fps must be between 1 and 120")

        self.path = Path(path)
        self.width = width
        self.height = height
        self.fps = fps
        self.frame_count = 0
        self.payload_bytes = 0
        self.max_frame_size = 0
        self._index: list[tuple[int, int]] = []
        self._closed = False
        self._file: BinaryIO = self.path.open("w+b")
        try:
            self._write_header()
        except Exception:
            self._file.close()
            raise

    @property
    def file_size(self) -> int:
        if self._closed:
            return self.path.stat().st_size
        return self._file.tell()

    @property
    def closed(self) -> bool:
        return self._closed

    def _write_chunk(self, tag: bytes, data: bytes) -> int:
        position = self._file.tell()
        self._file.write(tag)
        self._file.write(struct.pack("<I", len(data)))
        self._file.write(data)
        if len(data) & 1:
            self._file.write(b"\0")
        return position

    def _patch_u32(self, position: int, value: int) -> None:
        current = self._file.tell()
        self._file.seek(position)
        self._file.write(struct.pack("<I", value))
        self._file.seek(current)

    def _finish_list(self, size_position: int) -> None:
        self._patch_u32(size_position, self._file.tell() - size_position - 4)

    def _write_header(self) -> None:
        stream_rate = self.fps
        microseconds_per_frame = round(1_000_000 / self.fps)

        self._file.write(b"RIFF")
        self._riff_size_position = self._file.tell()
        self._file.write(b"\0\0\0\0AVI ")

        self._file.write(b"LIST")
        header_list_size = self._file.tell()
        self._file.write(b"\0\0\0\0hdrl")

        main_header = struct.pack(
            "<14I",
            microseconds_per_frame,
            0,
            0,
            _AVI_HAS_INDEX,
            0,
            0,
            1,
            0,
            self.width,
            self.height,
            0,
            0,
            0,
            0,
        )
        self._main_header_position = self._write_chunk(b"avih", main_header) + 8

        self._file.write(b"LIST")
        stream_list_size = self._file.tell()
        self._file.write(b"\0\0\0\0strl")
        stream_header = struct.pack(
            "<4s4sIHHIIIIIIIIhhhh",
            b"vids",
            b"MJPG",
            0,
            0,
            0,
            0,
            1,
            stream_rate,
            0,
            0,
            0,
            0xFFFFFFFF,
            0,
            0,
            0,
            self.width,
            self.height,
        )
        self._stream_header_position = self._write_chunk(b"strh", stream_header) + 8
        bitmap_header = struct.pack(
            "<IiiHH4sIiiII",
            40,
            self.width,
            self.height,
            1,
            24,
            b"MJPG",
            self.width * self.height * 3,
            0,
            0,
            0,
            0,
        )
        self._write_chunk(b"strf", bitmap_header)
        self._finish_list(stream_list_size)
        self._finish_list(header_list_size)

        self._file.write(b"LIST")
        self._movi_size_position = self._file.tell()
        self._file.write(b"\0\0\0\0movi")
        self._movi_offset_base = self._movi_size_position + 4

    def add_frame(self, jpeg: bytes | bytearray | memoryview) -> None:
        if self._closed:
            raise RuntimeError("AVI writer is closed")
        frame = bytes(jpeg)
        if len(frame) < 4 or not frame.startswith(b"\xff\xd8") or not frame.endswith(b"\xff\xd9"):
            raise ValueError("frame is not a complete JPEG image")
        projected_size = self._file.tell() + len(frame) + 8 + (len(frame) & 1)
        if projected_size >= _MAX_RIFF_SIZE:
            raise OSError("classic AVI reached its 4 GB size limit")

        chunk_position = self._write_chunk(b"00dc", frame)
        self._index.append((chunk_position - self._movi_offset_base, len(frame)))
        self.frame_count += 1
        self.payload_bytes += len(frame)
        self.max_frame_size = max(self.max_frame_size, len(frame))

    def close(self) -> None:
        if self._closed:
            return
        try:
            index_position = self._file.tell()
            self._patch_u32(
                self._movi_size_position,
                index_position - self._movi_size_position - 4,
            )
            index_data = bytearray()
            for offset, size in self._index:
                index_data.extend(
                    struct.pack("<4sIII", b"00dc", _AVI_KEYFRAME, offset, size)
                )
            self._write_chunk(b"idx1", bytes(index_data))

            self._patch_u32(self._main_header_position + 16, self.frame_count)
            self._patch_u32(self._main_header_position + 4, self.max_frame_size * self.fps)
            self._patch_u32(self._main_header_position + 28, self.max_frame_size)
            self._patch_u32(self._stream_header_position + 32, self.frame_count)
            self._patch_u32(self._stream_header_position + 36, self.max_frame_size)
            self._patch_u32(self._riff_size_position, self._file.tell() - 8)
            self._file.flush()
        finally:
            self._file.close()
            self._closed = True

    def __enter__(self) -> MjpegAviWriter:
        return self

    def __exit__(self, _exc_type: object, _exc: object, _traceback: object) -> None:
        self.close()
