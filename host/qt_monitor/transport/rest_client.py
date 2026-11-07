from __future__ import annotations

import json
from typing import Any

from PySide6.QtCore import QByteArray, QObject, QTimer, QUrl, Signal, Slot
from PySide6.QtNetwork import (
    QNetworkAccessManager,
    QNetworkProxy,
    QNetworkReply,
    QNetworkRequest,
)


def parse_json_response(data: bytes) -> dict[str, Any]:
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid JSON response: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("JSON response must be an object")
    return value


class CameraRestClient(QObject):
    status_updated = Signal(dict)
    capabilities_updated = Signal(dict)
    command_completed = Signal(str, bool, str)
    debug_message = Signal(str)
    ota_progress = Signal(int)

    def __init__(self, host: str, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.host = host
        self._manager = QNetworkAccessManager(self)
        self._manager.setProxy(QNetworkProxy(QNetworkProxy.ProxyType.NoProxy))
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(2000)
        self._poll_timer.timeout.connect(self.request_status)
        self._capabilities_received = False
        self._last_error = ""
        self._ota_in_progress = False

    def _request(self, path: str, kind: str, post: bool = False) -> None:
        request = QNetworkRequest(QUrl(f"http://{self.host}{path}"))
        request.setTransferTimeout(3000)
        request.setHeader(QNetworkRequest.KnownHeaders.ContentTypeHeader, "application/json")
        reply = (
            self._manager.post(request, QByteArray(b""))
            if post
            else self._manager.get(request)
        )
        reply.setProperty("request_kind", kind)
        reply.finished.connect(lambda reply=reply: self._finished(reply))

    def start_polling(self) -> None:
        if not self._poll_timer.isActive():
            self.request_status()
            if not self._capabilities_received:
                self._request("/api/v1/capabilities", "capabilities")
            self._poll_timer.start()

    def stop_polling(self) -> None:
        self._poll_timer.stop()

    @Slot()
    def request_status(self) -> None:
        if self._ota_in_progress:
            return
        self._request("/api/v1/status", "status")

    def set_streaming(self, enabled: bool) -> None:
        action = "start" if enabled else "stop"
        self._request(f"/api/v1/stream/{action}", f"stream_{action}", post=True)

    def set_resolution(self, width: int, height: int) -> None:
        self._request(
            f"/api/v1/camera/mode?width={width}&height={height}",
            f"mode_{width}x{height}",
            post=True,
        )

    def upload_firmware(self, firmware: bytes, token: str) -> None:
        self._ota_in_progress = True
        self._poll_timer.stop()
        request = QNetworkRequest(QUrl(f"http://{self.host}/api/v1/ota"))
        request.setTransferTimeout(300000)
        request.setHeader(
            QNetworkRequest.KnownHeaders.ContentTypeHeader,
            "application/octet-stream",
        )
        request.setRawHeader(b"X-OTA-Token", token.encode("utf-8"))
        reply = self._manager.post(request, QByteArray(firmware))
        reply.setProperty("request_kind", "ota")
        reply.uploadProgress.connect(self._ota_upload_progress)
        reply.finished.connect(lambda reply=reply: self._finished(reply))

    @Slot(int, int)
    def _ota_upload_progress(self, sent: int, total: int) -> None:
        if total > 0:
            self.ota_progress.emit(min(100, int(sent * 100 / total)))

    @Slot()
    def _finished(self, reply: QNetworkReply) -> None:
        kind = str(reply.property("request_kind"))
        try:
            if reply.error() != QNetworkReply.NetworkError.NoError:
                error = reply.errorString()
                response_body = bytes(reply.readAll())
                if response_body:
                    try:
                        payload = parse_json_response(response_body)
                        device_error = str(payload.get("error", "")).strip()
                        device_code = str(payload.get("code", "")).strip()
                        detail = ": ".join(part for part in (device_error, device_code) if part)
                        if detail:
                            error = f"{error}；设备返回：{detail}"
                    except ValueError:
                        text = response_body.decode("utf-8", errors="replace").strip()
                        if text:
                            error = f"{error}；设备返回：{text[:240]}"
                if error != self._last_error:
                    self.debug_message.emit(f"HTTP {kind} 失败：{error}")
                    self._last_error = error
                if kind.startswith(("stream_", "mode_", "ota")):
                    self.command_completed.emit(kind, False, error)
                return
            status_code = reply.attribute(
                QNetworkRequest.Attribute.HttpStatusCodeAttribute
            )
            if status_code is not None and int(status_code) >= 400:
                raise ValueError(f"HTTP {status_code}")
            payload = parse_json_response(bytes(reply.readAll()))
            self._last_error = ""
            if kind == "status":
                self.status_updated.emit(payload)
            elif kind == "capabilities":
                self._capabilities_received = True
                self.capabilities_updated.emit(payload)
            elif kind.startswith(("stream_", "mode_", "ota")):
                self.command_completed.emit(kind, True, json.dumps(payload, ensure_ascii=False))
                if kind != "ota":
                    self.request_status()
        except ValueError as exc:
            self.debug_message.emit(f"HTTP {kind} 响应无效：{exc}")
            if kind.startswith(("stream_", "mode_", "ota")):
                self.command_completed.emit(kind, False, str(exc))
        finally:
            if kind == "ota":
                self._ota_in_progress = False
                QTimer.singleShot(8000, self.start_polling)
            reply.deleteLater()
