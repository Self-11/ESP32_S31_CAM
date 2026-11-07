# ESP32-S31 视觉采集系统项目规划书

## 1. 项目目标

基于 ESP32-S31-Korvo-1 V1.1 与 ESP-IDF v6.1，构建一套模块化视觉采集系统：

- DVP 摄像头实时采集与 RGB LCD 本地预览
- JPEG 帧编码、TCP 私有协议推流和 HTTP MJPEG 看流
- microSD 抓拍、MJPEG AVI 录像和本地回放
- Qt 上位机预览、控制、抓拍、录像和 OpenCV 人脸框选
- Wi-Fi 网络链路以及预留的有线网络抽象
- A/B 双槽 OTA、镜像校验、启动确认和异常回滚
- OTA 期间统一停止媒体与存储业务，升级完成后安全重启

## 2. 已确认硬件基线

| 项目 | 已确认配置 |
| --- | --- |
| 主控 | ESP32-S31-WROOM-3，双核 RISC-V |
| 存储 | 16 MB SPI Flash，16 MB PSRAM |
| LCD 子板 | ESP32-S3-LCD-EV-Board-SUB3 v1.3 |
| LCD | 4.3 英寸，800x480，RGB565，ST7262E43 |
| 触摸 | GT1151 |
| 摄像头接口 | 8-bit DVP，SCCB/I2C 控制 |
| 实测参考传感器 | OV3660；运行时仍保留传感器识别和校验 |
| SD 卡 | SDIO 3.0，4-bit，GPIO20~25，GPIO39 电源控制 |
| 无线网络 | 2.4 GHz Wi-Fi 6 |
| USB | USB 2.0 High-Speed OTG Host；后续预留 Device 功能 |

### 2.1 硬件约束

RGB LCD 使用 GPIO8~19、GPIO33~36、GPIO40、GPIO43~45。ESP32-S31 原生
EMAC 的 RMII/RGMII 信号与 LCD 的 GPIO8~19 冲突，且 Korvo-1 没有板载 PHY。
首版产品基线为 **LCD + Wi-Fi**；有线网络通过统一网络接口预留，不在首版启用。

## 3. 总体数据流

```text
Camera sensor
    | DVP / RGB565 or sensor JPEG
    v
camera_service -> frame_broker ----------------------------+
                       |                                    |
                       +-> preview_service -> RGB LCD       |
                       +-> jpeg_service -> stream_service --+-> TCP / HTTP
                       +-> capture_service -> SD JPEG
                       +-> record_service -> SD MJPEG AVI

control_service <-> TCP/HTTP commands <-> Qt host
ota_service      <-> HTTP firmware upload
```

首选运行策略是让摄像头长期输出 RGB565，再由 ESP32-S31 JPEG 硬件编码器产生网络
JPEG，以减少传感器重配置。若硬件编码链在目标 IDF/BSP 版本中存在限制，则使用已经
实测的 OV3660 RGB565/JPEG 双档切换作为降级路径。

## 4. 软件分层

### 4.1 application

- `app_core`：系统状态机、事件总线、启动/停止顺序、故障汇总
- `app_main`：只负责组装模块，不承载业务实现

### 4.2 services

- `board_service`：BSP、LCD、触摸、摄像头、电源与板级资源
- `media_service`：采集、预览、JPEG、帧分发和模式切换
- `network_service`：Wi-Fi、链路选择、TCP/HTTP 服务生命周期
- `storage_service`：SDMMC、抓拍、AVI 文件和空间管理
- `ota_service`：OTA 会话、任务静默、镜像写入、校验和重启

### 4.3 protocol

- TCP 5000 私有帧协议
- HTTP REST 控制接口
- HTTP multipart MJPEG
- 统一错误码、能力查询和版本协商

### 4.4 host

- `host/qt_monitor`：Qt 上位机，独立构建
- 网络接收与 UI 解耦；OpenCV 人脸检测运行在工作线程

## 5. FreeRTOS 任务规划

| 任务 | 建议核 | 优先级 | 职责 |
| --- | ---: | ---: | --- |
| camera_task | 1 | 18 | V4L2/DVP 取帧、时间戳和入队 |
| preview_task | 1 | 16 | LCD 帧提交、显示所有权切换 |
| jpeg_task | 1 | 15 | 硬件 JPEG 编码和输出缓冲管理 |
| stream_task | 0 | 12 | TCP/HTTP 客户端分发和背压 |
| control_task | 0 | 11 | 控制命令、分辨率和启停请求 |
| storage_task | 0 | 10 | SD 卡文件写入、AVI 封装 |
| ota_task | 0 | 19 | OTA 独占会话和镜像写入 |
| health_task | 0 | 5 | 内存、帧率、丢帧和故障监控 |

任务优先级是初始值，必须通过实机帧率、Wi-Fi 吞吐和 SD 写入测试调整。

## 6. 并发与资源所有权

- 摄像头只有 `camera_service` 可以直接操作。
- LCD 只有 `preview_service` 可以提交 framebuffer。
- SD 卡只有 `storage_service` 执行文件写操作。
- OTA 通过系统状态机申请全局维护模式，禁止业务模块自行挂起其他任务。
- 帧数据使用引用计数或固定缓冲池传递，不在热路径反复 malloc/free。
- 队列满时丢弃旧视频帧，控制命令和 OTA 数据不得丢弃。
- 网络慢客户端不得反向阻塞相机采集。

## 7. 系统状态机

```text
BOOT -> INITIALIZING -> READY -> STREAMING/RECORDING
                         |             |
                         +-----------> MAINTENANCE_OTA
                                       |
                                       +-> REBOOT

任意状态 -> FAULT -> 释放可恢复资源 -> READY 或受控重启
```

进入 OTA 的顺序：停止接受新控制请求、停止录像、停止推流、停止预览、停止相机、
卸载或同步 SD 写入、开始 OTA。OTA 失败时按逆序恢复；OTA 成功时不恢复业务，直接重启。

## 8. TCP 私有帧协议草案

所有多字节字段使用网络字节序。

```text
magic[4] = "S31V"
version  u8
type     u8
flags    u16
sequence u32
timestamp_us u64
width    u16
height   u16
payload_length u32
header_crc32 u32
payload[...]
```

首批消息类型：`HELLO`、`CAPABILITIES`、`JPEG_FRAME`、`SET_MODE`、
`START_STREAM`、`STOP_STREAM`、`SNAPSHOT`、`RECORD_START`、`RECORD_STOP`、
`STATUS` 和 `ERROR`。

## 9. HTTP 接口草案

| 方法与路径 | 用途 |
| --- | --- |
| `GET /api/v1/status` | 版本、状态、帧率、内存和链路 |
| `GET /api/v1/capabilities` | 分辨率和功能能力 |
| `POST /api/v1/stream/start` | 启动推流 |
| `POST /api/v1/stream/stop` | 停止推流 |
| `POST /api/v1/camera/mode` | 设置分辨率/质量 |
| `POST /api/v1/snapshot` | 抓拍到 SD 或返回 JPEG |
| `POST /api/v1/record/start` | 开始 MJPEG AVI 录像 |
| `POST /api/v1/record/stop` | 停止录像 |
| `GET /stream.mjpeg` | multipart MJPEG 看流 |
| `POST /api/v1/ota` | 上传固件并升级 |

## 10. 内存预算原则

- LCD 800x480 RGB565 单帧约 750 KiB，双 framebuffer 约 1.5 MiB PSRAM。
- 720p RGB565 单帧约 1.76 MiB，不允许无预算地复制多份。
- V4L2 DVP 流还可能需要额外 backup buffer，启动前检查最大连续 PSRAM 块。
- JPEG 输出使用有上限的缓冲池；超过上限时降低质量或丢帧，禁止无限扩容。
- 任务栈按峰值监测调整，图像大缓冲不得放在任务栈。

## 11. OTA 与分区

- 使用 ESP-IDF 官方 OTA API 和 bootloader 回滚机制。
- 16 MB Flash 初始规划为两个 5 MiB 应用槽。
- 新固件首次启动完成相机、显示、网络基础自检后调用镜像确认。
- 在确认前发生崩溃或看门狗复位，由 bootloader 回滚。
- 不自行实现 Flash 擦写、镜像头解析或槽位切换算法。

## 12. 实施阶段

### 阶段 0：工程基础

- 模块目录、日志、状态机、事件和配置
- 官方 BSP/视频/JPEG 组件依赖
- A/B 分区表和基础 CI 构建

### 阶段 1：板级 bring-up

- LCD 色条、双缓冲和触摸验证
- 摄像头 ID、RGB565 帧和帧率验证
- SD 卡读写与持续写吞吐测试

### 阶段 2：本地媒体链

- RGB565 实时预览
- JPEG 编码/传感器 JPEG 降级路径
- 抓拍、AVI 录像和回放

### 阶段 3：网络与协议

- Wi-Fi STA/AP 管理
- TCP 5000 推流与控制协议
- HTTP REST 和 MJPEG

### 阶段 4：Qt 上位机

- 协议解析、显示、抓拍和录像
- OpenCV 人脸检测及叠框
- 设备配置和 OTA UI

### 阶段 5：OTA 与可靠性

- 双槽 OTA、回滚、自检确认
- 网络中断、SD 满、客户端慢速和内存碎片测试
- 24 小时稳定性与性能验收

## 13. 首版验收指标

- 800x480 LCD 连续预览 2 小时无定格、无永久偏移
- Wi-Fi TCP 与 HTTP MJPEG 均可独立启停，断开后资源可回收
- 慢客户端不导致相机停止采集
- 抓拍文件可被常用图像软件打开
- 录像文件断电前正常关闭时可播放；异常中断时可恢复已完成帧
- OTA 成功后进入新槽并确认；故障镜像能够自动回滚
- 所有核心模块支持重复 start/stop，不遗留任务和大块缓冲

