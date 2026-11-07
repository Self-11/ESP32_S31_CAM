from __future__ import annotations

from PySide6.QtCore import QDateTime, QSettings, QTimer, Slot
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from recording import RecordingCacheManager
from .device_panel import DevicePanel


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self._settings = QSettings("ESP32S31", "CameraMonitor")
        self._recorder = RecordingCacheManager(parent=self)
        self._panels: list[DevicePanel] = []
        self._build_ui()
        self._recorder.debug_message.connect(self._append_debug)
        self._recorder.state_changed.connect(self._update_storage_status)
        self._restore_devices()
        self._update_storage_status()

    def _build_ui(self) -> None:
        self.setWindowTitle("ESP32-S31 多设备摄像监控")
        self.resize(1200, 850)
        central = QWidget(self)
        root = QVBoxLayout(central)

        device_box = QGroupBox("设备管理", central)
        device_row = QHBoxLayout(device_box)
        self._host_edit = QLineEdit(device_box)
        self._host_edit.setPlaceholderText("输入设备 LCD 显示的 IP")
        self._port_spin = QSpinBox(device_box)
        self._port_spin.setRange(1, 65535)
        self._port_spin.setValue(5000)
        add_button = QPushButton("添加并连接", device_box)
        device_row.addWidget(QLabel("设备 IP", device_box))
        device_row.addWidget(self._host_edit, 1)
        device_row.addWidget(QLabel("端口", device_box))
        device_row.addWidget(self._port_spin)
        device_row.addWidget(add_button)

        storage_box = QGroupBox("本地录像存储", central)
        storage_row = QHBoxLayout(storage_box)
        self._segment_spin = QSpinBox(storage_box)
        self._segment_spin.setRange(64, 3800)
        self._segment_spin.setValue(1024)
        self._segment_spin.setSuffix(" MB/段")
        self._record_button = QPushButton("开始全部设备录像", storage_box)
        self._export_button = QPushButton("导出缓存", storage_box)
        self._storage_label = QLabel("缓存：0 个文件", storage_box)
        storage_row.addWidget(QLabel("分段上限", storage_box))
        storage_row.addWidget(self._segment_spin)
        storage_row.addWidget(self._record_button)
        storage_row.addWidget(self._export_button)
        storage_row.addStretch()
        storage_row.addWidget(self._storage_label)

        self._tabs = QTabWidget(central)
        self._tabs.setDocumentMode(True)

        debug_box = QGroupBox("运行与连接日志", central)
        debug_layout = QVBoxLayout(debug_box)
        debug_buttons = QHBoxLayout()
        self._debug_log = QPlainTextEdit(debug_box)
        self._debug_log.setReadOnly(True)
        self._debug_log.setMaximumBlockCount(1000)
        self._debug_log.setMaximumHeight(170)
        copy_log = QPushButton("复制日志", debug_box)
        clear_log = QPushButton("清空", debug_box)
        debug_buttons.addStretch()
        debug_buttons.addWidget(copy_log)
        debug_buttons.addWidget(clear_log)
        debug_layout.addWidget(self._debug_log)
        debug_layout.addLayout(debug_buttons)

        root.addWidget(device_box)
        root.addWidget(storage_box)
        root.addWidget(self._tabs, 1)
        root.addWidget(debug_box)
        self.setCentralWidget(central)

        add_button.clicked.connect(self._add_from_input)
        self._host_edit.returnPressed.connect(self._add_from_input)
        self._record_button.clicked.connect(self._toggle_recording)
        self._export_button.clicked.connect(self._export_cache)
        copy_log.clicked.connect(
            lambda: QApplication.clipboard().setText(self._debug_log.toPlainText())
        )
        clear_log.clicked.connect(self._debug_log.clear)
        self._append_debug("多设备监控与持久化缓存已启动")

    @Slot(str)
    def _append_debug(self, message: str) -> None:
        timestamp = QDateTime.currentDateTime().toString("HH:mm:ss.zzz")
        self._debug_log.appendPlainText(f"[{timestamp}] {message}")

    @Slot()
    def _add_from_input(self) -> None:
        host = self._host_edit.text().strip()
        if not host:
            QMessageBox.information(self, "缺少地址", "请输入设备 LCD 显示的 IP 地址。")
            return
        self.add_device(host, self._port_spin.value(), auto_connect=True)
        self._host_edit.clear()

    def add_device(self, host: str, port: int, auto_connect: bool) -> DevicePanel:
        address_key = f"{host}:{port}"
        for panel in self._panels:
            if panel.address_key == address_key:
                self._tabs.setCurrentWidget(panel)
                if auto_connect:
                    panel.connect_device()
                return panel

        panel = DevicePanel(host, port, self._recorder, self)
        panel.debug_message.connect(self._append_debug)
        panel.title_changed.connect(self._update_panel_title)
        panel.remove_requested.connect(self._remove_panel)
        self._panels.append(panel)
        self._tabs.addTab(panel, panel.display_title)
        self._tabs.setCurrentWidget(panel)
        self._save_devices()
        self._append_debug(f"已添加设备：{address_key}")
        if auto_connect:
            QTimer.singleShot(0, panel.connect_device)
        return panel

    @Slot(object, str)
    def _update_panel_title(self, panel: DevicePanel, title: str) -> None:
        index = self._tabs.indexOf(panel)
        if index >= 0:
            self._tabs.setTabText(index, title)

    @Slot(object)
    def _remove_panel(self, panel: DevicePanel) -> None:
        index = self._tabs.indexOf(panel)
        if index < 0:
            return
        panel.close_session()
        self._tabs.removeTab(index)
        self._panels.remove(panel)
        panel.deleteLater()
        self._save_devices()
        self._append_debug(f"已移除设备：{panel.address_key}")

    @Slot()
    def _toggle_recording(self) -> None:
        if self._recorder.enabled:
            self._recorder.stop()
            self._record_button.setText("开始全部设备录像")
            self._record_button.setStyleSheet("")
            self._segment_spin.setEnabled(True)
            self._choose_export_directory(after_stop=True)
            return
        self._recorder.segment_bytes = self._segment_spin.value() * 1024 * 1024
        self._recorder.start()
        self._record_button.setText("停止录像并导出")
        self._record_button.setStyleSheet(
            "background:#dc2626;color:white;font-weight:bold"
        )
        self._segment_spin.setEnabled(False)
        self._append_debug(
            f"所有已连接及后续连接设备开始录像；单段上限 "
            f"{self._segment_spin.value()} MB"
        )

    def _choose_export_directory(self, after_stop: bool) -> None:
        count, _ = self._recorder.cache_summary()
        if count == 0:
            if after_stop:
                QMessageBox.information(self, "没有录像", "本次没有产生可导出的录像分段。")
            return
        destination = QFileDialog.getExistingDirectory(self, "选择录像保存目录")
        if not destination:
            self._append_debug("未选择保存目录，录像继续保留在持久化缓存中")
            self._update_storage_status()
            return
        try:
            exported = self._recorder.export(destination)
        except OSError as exc:
            QMessageBox.warning(self, "录像导出失败", str(exc))
            return
        QMessageBox.information(
            self, "导出完成", f"已保存 {len(exported)} 个录像分段到：\n{destination}"
        )

    @Slot()
    def _export_cache(self) -> None:
        self._choose_export_directory(after_stop=False)

    @Slot()
    def _update_storage_status(self) -> None:
        count, size = self._recorder.cache_summary()
        state = "录像中" if self._recorder.enabled else "已停止"
        self._storage_label.setText(
            f"{state}｜缓存 {count} 段，{size / (1024 * 1024):.1f} MB"
        )
        self._export_button.setEnabled(count > 0 and not self._recorder.enabled)

    def _save_devices(self) -> None:
        self._settings.setValue("devices", [panel.address_key for panel in self._panels])

    def _restore_devices(self) -> None:
        saved = self._settings.value("devices", [])
        if isinstance(saved, str):
            saved = [saved]
        for address in saved or []:
            host, separator, port_text = str(address).rpartition(":")
            if separator and host and port_text.isdigit():
                self.add_device(host, int(port_text), auto_connect=True)

    def closeEvent(self, event: object) -> None:
        if self._recorder.enabled:
            self._recorder.stop()
        for panel in list(self._panels):
            panel.close_session()
        self._save_devices()
        super().closeEvent(event)
