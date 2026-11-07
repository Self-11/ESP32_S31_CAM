from __future__ import annotations

from PySide6.QtCore import QObject, QTimer, Signal, Slot
from PySide6.QtNetwork import (
    QAbstractSocket,
    QNetworkInterface,
    QNetworkProxy,
    QTcpSocket,
)

from .protocol import (MessageType, ProtocolError, StreamParser,
                              decode_face_results, decode_enroll_ack,
                              decode_face_list_reply,
                              decode_face_threshold_ack,
                              encode_face_enroll, encode_face_enroll_upload,
                              encode_face_delete, encode_face_clear,
                              encode_face_list, encode_face_threshold)


class CameraClient(QObject):
    connected = Signal()
    disconnected = Signal()
    status_message = Signal(str)
    debug_message = Signal(str)
    device_identified = Signal(str, str)
    jpeg_frame = Signal(bytes, object, object, int, int)
    face_results = Signal(object, int, int)
    face_enrolled = Signal(int)
    face_list_received = Signal(list)
    face_threshold_changed = Signal(float)
    traffic_updated = Signal(int)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._socket = QTcpSocket(self)
        # Camera traffic is a private raw-TCP protocol on the local network.
        # Never inherit Windows/PAC/VPN proxy settings: several system proxy
        # types are valid for HTTP but cannot be used by QTcpSocket.
        self._socket.setProxy(
            QNetworkProxy(QNetworkProxy.ProxyType.NoProxy)
        )
        self._parser = StreamParser()
        self._received_bytes = 0
        self._frame_count = 0
        self._connect_timer = QTimer(self)
        self._connect_timer.setSingleShot(True)
        self._connect_timer.setInterval(8000)
        self._connect_timer.timeout.connect(self._connection_timeout)
        self._socket.hostFound.connect(
            lambda: self.debug_message.emit("目标主机名/IP 已解析")
        )
        self._socket.stateChanged.connect(self._state_changed)
        self._socket.connected.connect(self._socket_connected)
        self._socket.disconnected.connect(self._socket_disconnected)
        self._socket.readyRead.connect(self._read_available)
        self._socket.errorOccurred.connect(self._socket_error)

    def connect_to_camera(self, host: str, port: int) -> None:
        if self._socket.state() != QAbstractSocket.SocketState.UnconnectedState:
            self._socket.abort()
        self._parser.reset()
        self._received_bytes = 0
        self._frame_count = 0
        self.status_message.emit(f"正在连接 {host}:{port}...")
        addresses = sorted(
            {
                address.toString()
                for address in QNetworkInterface.allAddresses()
                if not address.isLoopback() and not address.isNull()
            }
        )
        self.debug_message.emit(f"开始连接：{host}:{port}")
        self.debug_message.emit(
            "本机地址：" + (", ".join(addresses) if addresses else "未找到可用地址")
        )
        self.debug_message.emit(
            f"代理：{self._socket.proxy().type().name}（局域网直连）"
        )
        self._connect_timer.start()
        self._socket.connectToHost(host, port)

    def disconnect_from_camera(self) -> None:
        self.debug_message.emit("用户请求断开连接")
        self._socket.disconnectFromHost()

    def enroll_face(self, face_index: int = 0) -> None:
        if self.is_connected():
            data = encode_face_enroll(face_index)
            self._socket.write(data)
            self.debug_message.emit(f"发送录入命令：人脸序号 {face_index}")

    def enroll_face_upload(self, name: str, rgb565: bytes,
                            crop_w: int, crop_h: int) -> None:
        if self.is_connected():
            data = encode_face_enroll_upload(name, rgb565, crop_w, crop_h)
            self._socket.write(data)
            self.debug_message.emit(
                f"上传录入：{name} {crop_w}×{crop_h} ({len(rgb565)} bytes)"
            )

    def delete_face(self, face_id: int) -> None:
        if self.is_connected():
            data = encode_face_delete(face_id)
            self._socket.write(data)
            self.debug_message.emit(f"发送删除命令：ID {face_id}")

    def clear_all_faces(self) -> None:
        if self.is_connected():
            data = encode_face_clear()
            self._socket.write(data)
            self.debug_message.emit("发送清空命令")

    def request_face_list(self) -> None:
        if self.is_connected():
            data = encode_face_list()
            self._socket.write(data)
            self.debug_message.emit("发送查询已录入名单命令")

    def set_face_threshold(self, threshold: float) -> None:
        if self.is_connected():
            self._socket.write(encode_face_threshold(threshold))
            self.debug_message.emit(f"发送识别阈值：{threshold:.2f}")

    def is_connected(self) -> bool:
        return self._socket.state() == QAbstractSocket.SocketState.ConnectedState

    @Slot(QAbstractSocket.SocketState)
    def _state_changed(self, state: QAbstractSocket.SocketState) -> None:
        self.debug_message.emit(f"TCP 状态：{state.name}")

    @Slot()
    def _socket_connected(self) -> None:
        self._connect_timer.stop()
        self.debug_message.emit(
            "TCP 已连接："
            f"{self._socket.localAddress().toString()}:{self._socket.localPort()} -> "
            f"{self._socket.peerAddress().toString()}:{self._socket.peerPort()}"
        )
        self.connected.emit()

    @Slot()
    def _socket_disconnected(self) -> None:
        self._connect_timer.stop()
        self.debug_message.emit(
            f"TCP 已断开；累计接收 {self._received_bytes} 字节，"
            f"JPEG 帧 {self._frame_count}"
        )
        self.disconnected.emit()

    @Slot()
    def _connection_timeout(self) -> None:
        if self.is_connected():
            return
        self.debug_message.emit(
            "连接等待 8 秒超时：请检查电脑与设备是否连接同一路由器、"
            "设备 LCD 显示的 IP 是否一致，以及路由器是否启用了客户端隔离"
        )
        self.status_message.emit("连接超时，请查看下方调试日志")
        self._socket.abort()

    @Slot()
    def _read_available(self) -> None:
        data = bytes(self._socket.readAll())
        self._received_bytes += len(data)
        if self._received_bytes == len(data):
            self.debug_message.emit(f"收到第一批 TCP 数据：{len(data)} 字节")
        self.traffic_updated.emit(self._received_bytes)
        for message in self._parser.feed(data):
            header = message.header
            if header.message_type == MessageType.HELLO:
                hello = message.payload.decode("utf-8", errors="replace")
                self.debug_message.emit(f"收到设备 HELLO：{hello}")
                fields = {}
                for item in hello.split(";"):
                    if "=" in item:
                        key, value = item.split("=", 1)
                        fields[key.strip()] = value.strip()
                self.device_identified.emit(fields.get("id", "UNKNOWN_DEVICE"), hello)
                self.status_message.emit(
                    f"设备：{hello}"
                )
            elif header.message_type == MessageType.JPEG_FRAME:
                self._frame_count += 1
                if self._frame_count == 1:
                    self.debug_message.emit(
                        f"收到首帧 JPEG：序号 {header.sequence}，"
                        f"{header.width}×{header.height}，{len(message.payload)} 字节"
                    )
                self.jpeg_frame.emit(
                    message.payload,
                    header.sequence,
                    header.timestamp_us,
                    header.width,
                    header.height,
                )
            elif header.message_type == MessageType.FACE_RESULTS:
                try:
                    results = decode_face_results(message.payload)
                except ProtocolError as exc:
                    self.debug_message.emit(f"端侧人脸框数据无效：{exc}")
                    continue
                self.face_results.emit(results, header.width, header.height)
            elif header.message_type == MessageType.ERROR:
                text = message.payload.decode("utf-8", errors="replace")
                self.status_message.emit(f"设备错误：{text}")
            elif header.message_type == MessageType.FACE_ENROLL_ACK:
                try:
                    new_id = decode_enroll_ack(message.payload)
                    self.face_enrolled.emit(new_id)
                    self.debug_message.emit(f"设备确认录入，分配 ID：{new_id}")
                except ProtocolError:
                    pass
            elif header.message_type == MessageType.FACE_LIST_REPLY:
                try:
                    entries = decode_face_list_reply(message.payload)
                    self.face_list_received.emit(entries)
                except ProtocolError:
                    pass
            elif header.message_type == MessageType.FACE_THRESHOLD_ACK:
                try:
                    threshold = decode_face_threshold_ack(message.payload)
                    self.face_threshold_changed.emit(threshold)
                    self.debug_message.emit(f"设备已应用识别阈值：{threshold:.2f}")
                except ProtocolError as exc:
                    self.debug_message.emit(f"识别阈值确认无效：{exc}")

    @Slot(QAbstractSocket.SocketError)
    def _socket_error(self, error: QAbstractSocket.SocketError) -> None:
        self._connect_timer.stop()
        self.debug_message.emit(
            f"TCP 错误：{error.name}；{self._socket.errorString()}；"
            f"状态={self._socket.state().name}"
        )
        self.status_message.emit(f"网络错误：{self._socket.errorString()}")
