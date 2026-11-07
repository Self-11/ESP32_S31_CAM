from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import os
from pathlib import Path
import re
import shutil

from PySide6.QtCore import QObject, Signal

from .avi_writer import MjpegAviWriter, recover_incomplete_avi


DEFAULT_SEGMENT_BYTES = 1024 * 1024 * 1024


@dataclass
class _ActiveSegment:
    writer: MjpegAviWriter
    part_path: Path
    device_id: str


class RecordingCacheManager(QObject):
    state_changed = Signal()
    debug_message = Signal(str)

    def __init__(
        self,
        cache_root: str | Path | None = None,
        segment_bytes: int = DEFAULT_SEGMENT_BYTES,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.cache_root = Path(cache_root) if cache_root else self._default_cache_root()
        self.segment_bytes = segment_bytes
        self.enabled = False
        self._active: dict[str, _ActiveSegment] = {}
        self._sequence = 0
        self.cache_root.mkdir(parents=True, exist_ok=True)
        self._recover_interrupted_segments()

    @staticmethod
    def _default_cache_root() -> Path:
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "ESP32S31Camera" / "recording_cache"
        return Path.home() / ".esp32_s31_camera" / "recording_cache"

    @staticmethod
    def _safe_device_id(device_id: str) -> str:
        cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", device_id).strip("._")
        return cleaned or "UNKNOWN_DEVICE"

    def _recover_interrupted_segments(self) -> None:
        for part_path in self.cache_root.rglob("*.part.avi"):
            try:
                frame_count = recover_incomplete_avi(part_path)
                if frame_count == 0:
                    part_path.unlink(missing_ok=True)
                    continue
                final_path = part_path.with_name(part_path.name.replace(".part.avi", ".avi"))
                part_path.replace(final_path)
                self.debug_message.emit(f"已恢复异常中断录像：{final_path}")
            except (OSError, ValueError) as exc:
                self.debug_message.emit(f"无法恢复缓存录像 {part_path}: {exc}")

    def start(self) -> None:
        self.enabled = True
        self.debug_message.emit("本地多设备录像已启动")
        self.state_changed.emit()

    def _new_segment(
        self, session_key: str, device_id: str, width: int, height: int, fps: int
    ) -> _ActiveSegment:
        safe_id = self._safe_device_id(device_id)
        now = datetime.now()
        directory = self.cache_root / safe_id / now.strftime("%Y%m%d")
        directory.mkdir(parents=True, exist_ok=True)
        self._sequence += 1
        name = f"{safe_id}_{now:%Y%m%d_%H%M%S}_{self._sequence:03d}.part.avi"
        path = directory / name
        writer = MjpegAviWriter(path, width, height, max(1, min(120, fps)))
        segment = _ActiveSegment(writer, path, safe_id)
        self._active[session_key] = segment
        self.debug_message.emit(f"[{safe_id}] 新录像分段：{path.name}")
        self.state_changed.emit()
        return segment

    def write_frame(
        self,
        session_key: str,
        device_id: str,
        jpeg: bytes,
        width: int,
        height: int,
        fps: int,
    ) -> None:
        if not self.enabled:
            return
        segment = self._active.get(session_key)
        if segment and (segment.writer.width, segment.writer.height) != (width, height):
            self.finalize_session(session_key, "分辨率变化")
            segment = None
        estimated = len(jpeg) + 10
        if segment and segment.writer.frame_count > 0 and segment.writer.file_size + estimated >= self.segment_bytes:
            self.finalize_session(session_key, "达到分段大小上限")
            segment = None
        if segment is None:
            segment = self._new_segment(session_key, device_id, width, height, fps)
        segment.writer.add_frame(jpeg)

    def finalize_session(self, session_key: str, reason: str) -> Path | None:
        segment = self._active.pop(session_key, None)
        if segment is None:
            return None
        segment.writer.close()
        if segment.writer.frame_count == 0:
            segment.part_path.unlink(missing_ok=True)
            self.state_changed.emit()
            return None
        final_path = segment.part_path.with_name(
            segment.part_path.name.replace(".part.avi", ".avi")
        )
        segment.part_path.replace(final_path)
        self.debug_message.emit(
            f"[{segment.device_id}] 分段已保存：{final_path.name}；"
            f"{segment.writer.frame_count} 帧；原因={reason}"
        )
        self.state_changed.emit()
        return final_path

    def stop(self) -> None:
        self.enabled = False
        for session_key in list(self._active):
            self.finalize_session(session_key, "停止本地录像")
        self.debug_message.emit("本地多设备录像已停止")
        self.state_changed.emit()

    def pending_files(self) -> list[Path]:
        return sorted(
            path
            for path in self.cache_root.rglob("*.avi")
            if not path.name.endswith(".part.avi")
        )

    def cache_summary(self) -> tuple[int, int]:
        files = self.pending_files()
        return len(files), sum(path.stat().st_size for path in files)

    def export(self, destination: str | Path) -> list[Path]:
        destination_root = Path(destination)
        exported: list[Path] = []
        for source in self.pending_files():
            relative = source.relative_to(self.cache_root)
            target = destination_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target = target.with_name(
                    f"{target.stem}_{datetime.now():%H%M%S%f}{target.suffix}"
                )
            shutil.move(str(source), str(target))
            exported.append(target)
        for directory in sorted(self.cache_root.rglob("*"), reverse=True):
            if directory.is_dir():
                try:
                    directory.rmdir()
                except OSError:
                    pass
        self.debug_message.emit(f"已导出 {len(exported)} 个录像分段到：{destination_root}")
        self.state_changed.emit()
        return exported
