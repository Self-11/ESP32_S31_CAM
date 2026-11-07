from __future__ import annotations

from datetime import datetime
from pathlib import Path
from time import monotonic
from uuid import uuid4

from PySide6.QtCore import QByteArray, QBuffer, QIODevice, Qt, QTimer, Signal, Slot
from PySide6.QtGui import QColor, QFont, QImage, QPainter
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QDoubleSpinBox,
    QVBoxLayout,
    QWidget,
)

from recording import RecordingCacheManager
from transport.camera_client import CameraClient
from transport.rest_client import CameraRestClient
from .video_widget import VideoWidget


class DevicePanel(QWidget):
    title_changed = Signal(object, str)
    remove_requested = Signal(object)
    debug_message = Signal(str)

    def __init__(
        self,
        host: str,
        port: int,
        recorder: RecordingCacheManager,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.host = host
        self.port = port
        self.session_key = uuid4().hex
        self.device_id = f"IP-{host.replace('.', '-')}"
        self._recorder = recorder
        self._client = CameraClient(self)
        self._rest = CameraRestClient(host, self)
        self._streaming = True
        self._current_resolution = (0, 0)
        self._latest_image = QImage()
        self._face_boxes = ()
        self._face_source_size = (0, 0)
        self._fps_started = monotonic()
        self._fps_frames = 0
        self._fps = 0.0
        self._first_recording_frame_logged = False
        self._build_ui()
        self._wire_signals()

    @property
    def address_key(self) -> str:
        return f"{self.host}:{self.port}"

    @property
    def display_title(self) -> str:
        return f"{self.device_id}  ({self.host})"

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        controls = QHBoxLayout()
        self._address_label = QLabel(f"{self.host}:{self.port}", self)
        self._state_label = QLabel("未连接", self)
        self._connect_button = QPushButton("连接", self)
        self._snapshot_button = QPushButton("抓拍", self)
        self._snapshot_button.setEnabled(False)
        self._stream_button = QPushButton("暂停推流", self)
        self._stream_button.setEnabled(False)
        self._resolution_combo = QComboBox(self)
        self._resolution_combo.setEnabled(False)
        self._resolution_button = QPushButton("应用分辨率", self)
        self._resolution_button.setEnabled(False)
        self._enroll_button = QPushButton("上传录入", self)
        self._enroll_button.setEnabled(False)
        self._clear_button = QPushButton("清除全部", self)
        self._clear_button.setEnabled(False)
        self._list_button = QPushButton("查看已录入", self)
        self._list_button.setEnabled(False)
        self._threshold_spin = QDoubleSpinBox(self)
        self._threshold_spin.setRange(0.10, 0.90)
        self._threshold_spin.setSingleStep(0.01)
        self._threshold_spin.setDecimals(2)
        self._threshold_spin.setValue(0.30)
        self._threshold_spin.setPrefix("阈值 ")
        self._threshold_spin.setEnabled(False)
        self._threshold_button = QPushButton("应用阈值", self)
        self._threshold_button.setEnabled(False)
        self._ota_button = QPushButton("OTA升级", self)
        self._ota_button.setEnabled(False)
        self._name_map: dict[int, str] = {}
        remove_button = QPushButton("移除设备", self)
        controls.addWidget(self._address_label)
        controls.addWidget(self._state_label)
        controls.addStretch()
        controls.addWidget(self._connect_button)
        controls.addWidget(self._snapshot_button)
        controls.addWidget(self._stream_button)
        controls.addWidget(self._resolution_combo)
        controls.addWidget(self._resolution_button)
        controls.addWidget(self._enroll_button)
        controls.addWidget(self._clear_button)
        controls.addWidget(self._list_button)
        controls.addWidget(self._threshold_spin)
        controls.addWidget(self._threshold_button)
        controls.addWidget(self._ota_button)
        controls.addWidget(remove_button)

        self._video = VideoWidget(self)
        self._detail_label = QLabel("等待连接", self)
        self._detail_label.setWordWrap(True)
        self._device_status_label = QLabel("板端状态：等待 HTTP 响应", self)
        self._device_status_label.setWordWrap(True)
        root.addLayout(controls)
        root.addWidget(self._video, 1)
        root.addWidget(self._detail_label)
        root.addWidget(self._device_status_label)

        self._connect_button.clicked.connect(self._toggle_connection)
        self._snapshot_button.clicked.connect(self._save_snapshot)
        self._stream_button.clicked.connect(self._toggle_streaming)
        self._resolution_button.clicked.connect(self._apply_resolution)
        self._enroll_button.clicked.connect(self._enroll_face)
        self._clear_button.clicked.connect(self._clear_faces)
        self._list_button.clicked.connect(self._show_face_list)
        self._threshold_button.clicked.connect(self._apply_face_threshold)
        self._ota_button.clicked.connect(self._upload_firmware)
        remove_button.clicked.connect(lambda: self.remove_requested.emit(self))

    def _wire_signals(self) -> None:
        prefix = lambda text: self.debug_message.emit(f"[{self.device_id}] {text}")
        self._client.debug_message.connect(prefix)
        self._client.status_message.connect(self._detail_label.setText)
        self._client.connected.connect(self._connected)
        self._client.disconnected.connect(self._disconnected)
        self._client.device_identified.connect(self._identified)
        self._client.jpeg_frame.connect(self._display_frame)
        self._client.face_results.connect(self._face_results_received)
        self._client.face_enrolled.connect(self._face_enrolled)
        self._client.face_list_received.connect(self._face_list_received)
        self._client.face_threshold_changed.connect(self._face_threshold_changed)
        self._rest.debug_message.connect(prefix)
        self._rest.status_updated.connect(self._status_updated)
        self._rest.capabilities_updated.connect(self._capabilities_updated)
        self._rest.command_completed.connect(self._command_completed)
        self._rest.ota_progress.connect(self._ota_progress)

    @Slot()
    def connect_device(self) -> None:
        if self._client.is_connected():
            return
        self._connect_button.setEnabled(False)
        self._state_label.setText("连接中...")
        self._rest.start_polling()
        self._client.connect_to_camera(self.host, self.port)

    @Slot()
    def _toggle_connection(self) -> None:
        if self._client.is_connected():
            self._client.disconnect_from_camera()
        else:
            self.connect_device()

    @Slot()
    def _connected(self) -> None:
        self._state_label.setText("已连接")
        self._state_label.setStyleSheet("color:#16a34a;font-weight:bold")
        self._connect_button.setText("断开")
        self._connect_button.setEnabled(True)
        self._enroll_button.setEnabled(True)
        self._clear_button.setEnabled(True)
        self._threshold_spin.setEnabled(True)
        self._threshold_button.setEnabled(True)
        self._ota_button.setEnabled(True)
        self._detail_label.setText("等待设备信息和 JPEG 帧...")
        self._first_recording_frame_logged = False
        self._client.request_face_list()

    @Slot()
    def _disconnected(self) -> None:
        self._recorder.finalize_session(self.session_key, "设备断开")
        self._face_boxes = ()
        self._face_source_size = (0, 0)
        self._video.set_face_boxes(())
        self._state_label.setText("未连接")
        self._state_label.setStyleSheet("")
        self._connect_button.setText("连接")
        self._connect_button.setEnabled(True)
        self._snapshot_button.setEnabled(False)
        self._stream_button.setEnabled(False)
        self._resolution_button.setEnabled(False)
        self._enroll_button.setEnabled(False)
        self._clear_button.setEnabled(False)
        self._threshold_spin.setEnabled(False)
        self._threshold_button.setEnabled(False)
        self._ota_button.setEnabled(False)
        self._detail_label.setText("连接已断开")

    @Slot(str, str)
    def _identified(self, device_id: str, hello: str) -> None:
        del hello
        old_id = self.device_id
        self.device_id = device_id or old_id
        self.title_changed.emit(self, self.display_title)
        self.debug_message.emit(f"[{old_id}] 识别为设备 {self.device_id}")

    @Slot(dict)
    def _status_updated(self, status: dict) -> None:
        http_device_id = str(status.get("device_id", ""))
        if http_device_id and http_device_id != self.device_id:
            self._identified(http_device_id, "")
        self._streaming = bool(status.get("streaming", True))
        self._stream_button.setText("暂停推流" if self._streaming else "恢复推流")
        self._stream_button.setEnabled(True)
        link = str(status.get("link", "--"))
        ip = str(status.get("ip", self.host))
        rgb = int(status.get("rgb_frames", 0))
        jpeg = int(status.get("jpeg_frames", 0))
        dropped = int(status.get("jpeg_dropped", 0))
        tcp_clients = int(status.get("tcp_clients", 0))
        mjpeg_clients = int(status.get("mjpeg_clients", 0))
        face_ready = bool(status.get("face_ready", False))
        face_count = int(status.get("face_count", 0))
        face_ms = int(status.get("face_inference_ms", 0))
        sd_mounted = bool(status.get("sd_mounted", False))
        recording = bool(status.get("recording", False))
        recorded_frames = int(status.get("recorded_frames", 0))
        stream_text = "运行" if self._streaming else "暂停"
        width = int(status.get("width", 0))
        height = int(status.get("height", 0))
        if width and height:
            self._current_resolution = (width, height)
        self._device_status_label.setText(
            f"板端：{link}  IP {ip}  {width or '--'}×{height or '--'}  推流 {stream_text}｜"
            f"RGB {rgb}  JPEG {jpeg}  丢帧 {dropped}｜"
            f"TCP {tcp_clients}  HTTP {mjpeg_clients}｜"
            f"端侧人脸 {'就绪' if face_ready else '加载中'}  {face_count} 个  {face_ms} ms｜"
            f"SD {'录像中' if recording else ('已挂载' if sd_mounted else '未挂载')}  {recorded_frames} 帧"
        )

    @Slot(object, int, int)
    def _face_results_received(self, results: object, width: int, height: int) -> None:
        boxes = results.boxes
        for box in boxes:
            rid = box.recognition_id
            conf = box.recognition_confidence
            if rid > 0 and rid in self._name_map:
                box.label = f"{self._name_map[rid]} {conf:.0%}"
            elif rid > 0:
                box.label = f"ID:{rid} {conf:.0%}"
            else:
                box.label = ""
        self._face_boxes = boxes
        self._face_source_size = (width, height)
        self._video.set_face_boxes(boxes)

        parts = []
        for box in boxes:
            if box.recognition_id != 0:
                label_name = self._name_map.get(box.recognition_id, f"ID:{box.recognition_id}")
                parts.append(f"{label_name}({box.recognition_confidence:.3f})")
            else:
                parts.append(f"unknown({box.score:.2f})")
        if parts:
            self.debug_message.emit(
                f"[{self.device_id}] faces: {' | '.join(parts)}"
            )

    @Slot(dict)
    def _capabilities_updated(self, capabilities: dict) -> None:
        resolutions = capabilities.get("resolutions", [])
        self._resolution_combo.clear()
        for item in resolutions if isinstance(resolutions, list) else []:
            if not isinstance(item, dict):
                continue
            width = int(item.get("width", 0))
            height = int(item.get("height", 0))
            fps = int(item.get("fps", 0))
            if width > 0 and height > 0:
                self._resolution_combo.addItem(
                    f"{width}×{height}" + (f" ({fps} fps)" if fps else ""),
                    (width, height),
                )
        available = self._resolution_combo.count() > 0
        current = self._resolution_combo.findData(self._current_resolution)
        if current >= 0:
            self._resolution_combo.setCurrentIndex(current)
        self._resolution_combo.setEnabled(available)
        self._resolution_button.setEnabled(available)
        self.debug_message.emit(
            f"[{self.device_id}] 能力：摄像头={capabilities.get('camera', '--')}，"
            f"{capabilities.get('width', '--')}×{capabilities.get('height', '--')}，"
            f"TCP端口={capabilities.get('tcp_port', '--')}"
        )

    @Slot()
    def _toggle_streaming(self) -> None:
        self._stream_button.setEnabled(False)
        self._rest.set_streaming(not self._streaming)

    @Slot()
    def _apply_resolution(self) -> None:
        selected = self._resolution_combo.currentData()
        if not selected:
            return
        width, height = selected
        self._resolution_button.setEnabled(False)
        self.debug_message.emit(
            f"[{self.device_id}] 请求切换分辨率到 {width}×{height}，画面会短暂停顿"
        )
        self._rest.set_resolution(width, height)

    @Slot(str, bool, str)
    def _command_completed(self, command: str, success: bool, detail: str) -> None:
        if command == "ota":
            self._ota_button.setEnabled(True)
            if success:
                self.debug_message.emit(
                    f"[{self.device_id}] OTA 写入成功，设备正在重启"
                )
                self._detail_label.setText("OTA 完成，等待设备重启并重新连接...")
            else:
                self.debug_message.emit(f"[{self.device_id}] OTA 失败：{detail}")
                QMessageBox.warning(self, "OTA 失败", detail)
            return
        if command.startswith("mode_"):
            self.debug_message.emit(
                f"[{self.device_id}] 分辨率切换{'成功' if success else '失败'}：{detail}"
            )
            self._resolution_button.setEnabled(self._resolution_combo.count() > 0)
            return
        action = "恢复推流" if command == "stream_start" else "暂停推流"
        self.debug_message.emit(
            f"[{self.device_id}] {action}{'成功' if success else '失败'}：{detail}"
        )
        self._stream_button.setEnabled(True)

    @Slot()
    def _upload_firmware(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(
            self, "选择应用固件", "", "ESP32 应用固件 (*.bin)"
        )
        if not file_path:
            return
        try:
            firmware = Path(file_path).read_bytes()
        except OSError as exc:
            QMessageBox.warning(self, "OTA 失败", f"无法读取固件：{exc}")
            return
        if not firmware or firmware[0] != 0xE9:
            QMessageBox.warning(
                self, "OTA 失败", "这不是有效的 ESP 应用固件（镜像头不是 0xE9）"
            )
            return
        if len(firmware) > 0x680000:
            QMessageBox.warning(self, "OTA 失败", "固件超过 OTA 分区容量 6.5 MiB")
            return
        token, ok = QInputDialog.getText(
            self, "OTA 身份验证", "OTA 密钥：",
            QLineEdit.EchoMode.Password, "s31cam-ota"
        )
        if not ok or not token:
            return
        self._ota_button.setEnabled(False)
        self._detail_label.setText(f"正在上传 OTA 固件：0%（{len(firmware)} 字节）")
        self.debug_message.emit(
            f"[{self.device_id}] 开始 OTA：{Path(file_path).name}，{len(firmware)} 字节"
        )
        self._rest.upload_firmware(firmware, token)

    @Slot(int)
    def _ota_progress(self, percent: int) -> None:
        if percent >= 100:
            self._detail_label.setText(
                "固件已上传，设备正在擦除分区、写入并校验（通常需要10～30秒）..."
            )
        else:
            self._detail_label.setText(f"正在上传 OTA 固件：{percent}%")

    @staticmethod
    def _add_timestamp(image: QImage, timestamp: str) -> QImage:
        result = image.convertToFormat(QImage.Format.Format_RGB32)
        painter = QPainter(result)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        font = QFont("Arial")
        font.setPixelSize(max(16, result.height() // 24))
        font.setBold(True)
        painter.setFont(font)
        metrics = painter.fontMetrics()
        margin = 10
        text_width = metrics.horizontalAdvance(timestamp)
        text_height = metrics.height()
        x = result.width() - text_width - margin * 2
        y = result.height() - text_height - margin * 2
        painter.fillRect(
            x,
            y,
            text_width + margin,
            text_height + margin,
            QColor(0, 0, 0, 150),
        )
        painter.setPen(Qt.GlobalColor.white)
        painter.drawText(x + margin // 2, y + metrics.ascent() + margin // 2, timestamp)
        painter.end()
        return result

    def _add_face_boxes(self, image: QImage, boxes: tuple) -> QImage:
        if not boxes:
            return image
        result = image.convertToFormat(QImage.Format.Format_RGB32)
        painter = QPainter(result)
        painter.setPen(QColor(34, 197, 94))
        font = QFont("Arial")
        font.setPixelSize(max(14, result.height() // 30))
        font.setBold(True)
        painter.setFont(font)
        for box in boxes:
            painter.drawRect(box.x, box.y, box.width, box.height)
            if box.label:
                painter.drawText(box.x + 4, max(font.pixelSize(), box.y - 4), box.label)
        painter.end()
        return result

    @staticmethod
    def _encode_jpeg(image: QImage) -> bytes:
        data = QByteArray()
        buffer = QBuffer(data)
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        if not image.save(buffer, "JPEG", 90):
            buffer.close()
            raise OSError("Qt JPEG encoder failed")
        buffer.close()
        return bytes(data)

    @Slot(bytes, object, object, int, int)
    def _display_frame(
        self, jpeg: bytes, sequence: int, timestamp_us: int, width: int, height: int
    ) -> None:
        del timestamp_us
        image = QImage.fromData(QByteArray(jpeg), "JPEG")
        if image.isNull():
            self._detail_label.setText(f"JPEG 解码失败，序号 {sequence}")
            return
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        stamped = self._add_timestamp(image, timestamp)
        archived = stamped
        if self._face_source_size == (width, height):
            archived = self._add_face_boxes(stamped, self._face_boxes)
        self._latest_image = archived
        self._video.set_frame(stamped)
        self._video.set_face_boxes(
            self._face_boxes if self._face_source_size == (width, height) else ()
        )
        self._snapshot_button.setEnabled(True)

        self._fps_frames += 1
        elapsed = monotonic() - self._fps_started
        if elapsed >= 1.0:
            self._fps = self._fps_frames / elapsed
            self._fps_frames = 0
            self._fps_started = monotonic()
        self._detail_label.setText(
            f"{self.device_id}  {width}×{height}  序号 {sequence}  {self._fps:.1f} fps"
        )

        if self._recorder.enabled:
            try:
                recorded_jpeg = self._encode_jpeg(archived)
                nominal_fps = round(self._fps) if self._fps >= 1 else 15
                self._recorder.write_frame(
                    self.session_key,
                    self.device_id,
                    recorded_jpeg,
                    width,
                    height,
                    nominal_fps,
                )
                if not self._first_recording_frame_logged:
                    self.debug_message.emit(f"[{self.device_id}] 已开始写入带时间水印的录像")
                    self._first_recording_frame_logged = True
            except (OSError, ValueError, RuntimeError) as exc:
                self._recorder.finalize_session(self.session_key, "录像写入错误")
                self.debug_message.emit(f"[{self.device_id}] 录像写入失败：{exc}")

    @Slot()
    def _save_snapshot(self) -> None:
        if self._latest_image.isNull():
            return
        default_name = f"{self.device_id}_{datetime.now():%Y%m%d_%H%M%S}.jpg"
        file_name, _ = QFileDialog.getSaveFileName(
            self, "保存抓拍", default_name, "JPEG (*.jpg *.jpeg)"
        )
        if file_name and not self._latest_image.save(file_name, "JPEG", 95):
            QMessageBox.warning(self, "保存失败", f"无法写入：{file_name}")

    @Slot()
    def _enroll_face(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(
            self, "选择人脸照片", "",
            "图片文件 (*.jpg *.jpeg *.png *.bmp);;所有文件 (*.*)"
        )
        if not file_path:
            return
        name, ok = QInputDialog.getText(self, "输入姓名", "请为这张人脸输入姓名：")
        if not ok or not name.strip():
            return
        name = name.strip()
        try:
            rgb565, crop_w, crop_h = self._image_to_rgb565_crop(file_path)
        except (OSError, ValueError, RuntimeError) as exc:
            QMessageBox.warning(self, "录入失败", f"无法处理图片：{exc}")
            return
        if rgb565 is None:
            QMessageBox.information(self, "未检测到人脸", "所选图片中未检测到人脸，请换一张正面清晰照片。")
            return
        self._client.enroll_face_upload(name, rgb565, crop_w, crop_h)
        self._pending_name = name
        self.debug_message.emit(
            f"[{self.device_id}] 上传录入：{name} {crop_w}×{crop_h}"
        )

    @Slot(int)
    def _face_enrolled(self, new_id: int) -> None:
        if new_id > 0 and hasattr(self, "_pending_name"):
            self._name_map[new_id] = self._pending_name
        self.debug_message.emit(
            f"[{self.device_id}] 人脸录入完成，ID：{new_id}"
        )

    @Slot()
    def _clear_faces(self) -> None:
        self._client.clear_all_faces()
        self._name_map.clear()
        self.debug_message.emit(f"[{self.device_id}] 请求清除所有人脸")

    @Slot()
    def _show_face_list(self) -> None:
        self._list_button.setEnabled(False)
        self._client.request_face_list()

    @Slot()
    def _apply_face_threshold(self) -> None:
        self._threshold_button.setEnabled(False)
        self._client.set_face_threshold(self._threshold_spin.value())

    @Slot(float)
    def _face_threshold_changed(self, threshold: float) -> None:
        self._threshold_spin.setValue(threshold)
        self._threshold_button.setEnabled(self._client.is_connected())
        self.debug_message.emit(
            f"[{self.device_id}] 识别阈值已设为 {threshold:.2f}"
        )

    @Slot(list)
    def _face_list_received(self, entries: list) -> None:
        self._list_button.setEnabled(True)
        for eid, ename in entries:
            if ename:
                self._name_map[eid] = ename
        if not entries:
            return
        lines = [f"ID {eid}: {ename}" for eid, ename in entries]
        QMessageBox.information(
            self, "已录入名单",
            f"设备 {self.device_id} 已录入 {len(entries)} 人：\n\n" + "\n".join(lines)
        )

    def close_session(self) -> None:
        self._rest.stop_polling()
        self._recorder.finalize_session(self.session_key, "移除设备或关闭程序")
        if self._client.is_connected():
            self._client.disconnect_from_camera()

    @staticmethod
    def _image_to_rgb565_crop(file_path: str) -> tuple[bytes | None, int, int]:
        import cv2
        import numpy as np
        raw = np.fromfile(file_path, dtype=np.uint8)
        img = cv2.imdecode(raw, cv2.IMREAD_COLOR)
        if img is None:
            raise OSError(f"无法读取图片：{file_path}")
        return DevicePanel._bgr_to_rgb565_crop(img)

    @staticmethod
    def _bgr_to_rgb565_crop(img) -> tuple[bytes | None, int, int]:
        import cv2
        import numpy as np
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        )
        faces = cascade.detectMultiScale(gray, 1.1, 5, minSize=(60, 60))
        if len(faces) == 0:
            return None, 0, 0
        # Keep generous, square context around the Haar detection.  The board
        # runs its landmark-capable MSR+MNP detector again; a tightly cropped
        # face is outside the image distribution that detector expects and can
        # otherwise produce ESP_ERR_NOT_FOUND.
        x, y, w, h = max(faces, key=lambda face: int(face[2]) * int(face[3]))
        cx = x + w / 2.0
        cy = y + h / 2.0
        side = int(max(w, h) * 2.4)
        side = min(side, img.shape[1], img.shape[0])
        x1 = int(round(cx - side / 2.0))
        y1 = int(round(cy - side / 2.0))
        x1 = max(0, min(x1, img.shape[1] - side))
        y1 = max(0, min(y1, img.shape[0] - side))
        x2 = x1 + side
        y2 = y1 + side
        crop = img[y1:y2, x1:x2]
        target_max = 320
        scale = target_max / max(crop.shape[1], crop.shape[0])
        crop = cv2.resize(crop, (int(crop.shape[1] * scale), int(crop.shape[0] * scale)))
        h_img, w_img = crop.shape[:2]
        rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
        r = (rgb[:, :, 0] >> 3).astype(np.uint16)
        g = (rgb[:, :, 1] >> 2).astype(np.uint16)
        b = (rgb[:, :, 2] >> 3).astype(np.uint16)
        rgb565 = (r << 11) | (g << 5) | b
        return rgb565.tobytes(), w_img, h_img