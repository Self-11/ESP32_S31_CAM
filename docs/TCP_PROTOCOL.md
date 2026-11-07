# ESP32-S31 TCP JPEG 协议

TCP 服务监听 `5000` 端口。所有多字节整数均为网络字节序（大端），帧头固定为 32 字节。

| 偏移 | 长度 | 字段 | 说明 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `S31V` / `0x53333156` |
| 4 | 1 | version | 当前为 1 |
| 5 | 1 | type | 1=HELLO，3=JPEG_FRAME，12=FACE_RESULTS |
| 6 | 2 | flags | 保留 |
| 8 | 4 | sequence | JPEG 帧序号 |
| 12 | 8 | timestamp_us | 板端微秒时间戳 |
| 20 | 2 | width | 图像宽度 |
| 22 | 2 | height | 图像高度 |
| 24 | 4 | payload_length | 紧随帧头的负载长度 |
| 28 | 4 | header_crc32 | 前 28 字节的 ESP ROM CRC32-BE |

连接建立后，板端首先发送 `HELLO`，随后持续发送 `JPEG_FRAME`。TCP 是字节流，客户端必须处理拆包和粘包，不能假设一次读取对应一帧。

人脸检测由 ESP32-S31 端侧完成。检测结果变化时，板端在 JPEG 帧之后发送 `FACE_RESULTS`：头部的 `width/height` 是检测框所属的源图尺寸，负载结构如下。

| 偏移 | 长度 | 字段 | 说明 |
| ---: | ---: | --- | --- |
| 0 | 4 | source_sequence | 送入端侧检测的 RGB 帧序号 |
| 4 | 1 | count | 人脸数量，最大 4 |
| 5 | 3 | reserved | 保留 |
| 8 | 12×count | boxes | 每项依次为 x、y、width、height、score_x10000、reserved，均为 16 位大端整数 |

上位机只解析并显示这些框，不运行 OpenCV 或其他本地人脸检测。

`HELLO` 头部和文本中的尺寸是客户端连接时的当前尺寸。运行中通过 HTTP 切换分辨率后，后续每个 `JPEG_FRAME` 头部会携带新尺寸，客户端应以帧头为准，不需要重连 TCP。

## HTTP 控制接口

| 方法与路径 | 用途 |
| --- | --- |
| `GET /api/v1/status` | 当前链路、推流状态、分辨率与帧计数 |
| `GET /api/v1/capabilities` | 返回 OV3660 支持的 `640×480@10fps`、`240×240@24fps` |
| `POST /api/v1/stream/start` | 恢复 TCP/MJPEG 视频发送 |
| `POST /api/v1/stream/stop` | 暂停 TCP/MJPEG 视频发送 |
| `POST /api/v1/camera/mode?width=640&height=480` | 安全切换分辨率；也可传 `240×240` |
| `GET /stream.mjpeg` | 浏览器 MJPEG 实时画面 |

切换模式时板端依次停止 DVP、等待旧 JPEG 帧释放、重设 OV3660、JPEG 编码器和 LCD 画布，再恢复采集。接口只接受官方驱动中已启用的两档 RGB565 模式。
