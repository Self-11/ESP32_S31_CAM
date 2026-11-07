# ESP32-S31 Python Qt Monitor

基于 PySide6 的多设备上位机，支持 TCP 5000 私有协议、JPEG 实时显示、抓拍保存、
实时时间水印和 MJPEG AVI 本地分段录像。

## 多设备录像

- 在“设备管理”中逐个填写设备 LCD 显示的 IP；每台设备使用独立标签页。
- 点击“开始全部设备录像”后，所有已连接及之后连接的设备都会分别录像。
- 录像先写入系统持久化缓存，不会在开始时要求选择保存位置。
- 设备断开或达到单段大小上限时自动封存当前段；重连后继续新建录像段。
- 停止录像后才选择导出目录。取消导出不会丢失录像，可稍后点击“导出缓存”。
- 每帧的当前日期和时间同时显示在预览画面并写入录像。
- 异常退出遗留的录像会在下次启动时尝试恢复索引。

## 设备状态与控制

每个设备标签页通过独立 HTTP 通道每 2 秒刷新板端状态，包括链路、IP、RGB/JPEG
帧计数、JPEG 丢帧以及 TCP/HTTP 客户端数量。“暂停推流/恢复推流”只控制视频发送，
不会断开状态查询通道。

设备标签页还会从板端能力接口读取可用分辨率。当前 OV3660 提供
`640×480 (10 fps)` 和 `240×240 (24 fps)` 两档，选择后点击“应用分辨率”。切换时画面会
短暂停顿但 TCP 无需重连；若正在录像，上位机会自动结束旧尺寸录像分段并创建新分段。

## 安装与运行

```powershell
cd F:\ESP32S31-CAM\software\host\qt_monitor
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe main.py
```

电脑和 ESP32 需连接同一个路由器。连接地址以设备 LCD 显示的 IP 为准，端口为 `5000`。

## 测试协议解析器

协议测试不依赖 PySide6：

```powershell
py -m unittest discover -s tests -v
```

协议说明见项目根目录 `docs/TCP_PROTOCOL.md`。
