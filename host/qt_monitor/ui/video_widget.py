from __future__ import annotations

from PySide6.QtCore import QRect, Qt
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QSizePolicy, QWidget


class VideoWidget(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._frame = QImage()
        self._face_boxes = ()
        self.setMinimumSize(640, 480)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_frame(self, frame: QImage) -> None:
        self._frame = frame
        self.update()

    def set_face_boxes(self, boxes: tuple) -> None:
        self._face_boxes = boxes
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:
        del event
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(16, 24, 32))
        if self._frame.isNull():
            painter.setPen(QColor(148, 163, 184))
            painter.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "等待摄像头画面...",
            )
            return

        scaled = self._frame.size().scaled(
            self.size(), Qt.AspectRatioMode.KeepAspectRatio
        )
        target = QRect(
            (self.width() - scaled.width()) // 2,
            (self.height() - scaled.height()) // 2,
            scaled.width(),
            scaled.height(),
        )
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        painter.drawImage(target, self._frame)
        if not self._face_boxes:
            return
        scale_x = target.width() / self._frame.width()
        scale_y = target.height() / self._frame.height()
        pen = QPen(QColor(34, 197, 94), 3)
        painter.setPen(pen)
        font = QFont("Arial")
        font.setPixelSize(14)
        font.setBold(True)
        painter.setFont(font)
        for box in self._face_boxes:
            rectangle = QRect(
                target.x() + round(box.x * scale_x),
                target.y() + round(box.y * scale_y),
                max(1, round(box.width * scale_x)),
                max(1, round(box.height * scale_y)),
            )
            painter.drawRect(rectangle)
            if hasattr(box, "label") and box.label:
                painter.drawText(
                    rectangle.adjusted(4, 2, 0, 0),
                    Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop,
                    box.label,
                )