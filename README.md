# ESP32-S31-CAM 智能摄像头系统

本项目是一套运行在 **ESP32-S31 Korvo-1 V1.1** 开发板上的完整摄像头应用，包含：

- OV3660 摄像头采集；
- 800×480 RGB LCD 实时预览；
- GT1151 触摸屏 Wi-Fi 配置；
- 端侧人脸检测与人脸识别；
- TCP JPEG 私有协议与 HTTP MJPEG 推流；
- Python/PySide6 多设备上位机；
- microSD 自动循环录像；
- 双 OTA 分区在线升级及回滚保护；
- NVS、SPIFFS、SD 卡三类持久化存储。

这份文档面向第一次接手本项目、尚不熟悉 ESP-IDF、FreeRTOS、LVGL 或视觉流水线的开发者。除了编译和运行方法，也会解释各模块为什么存在、它们如何协作，以及发生故障时应该从哪里开始排查。

---

## 1. 项目当前能力

### 1.1 板端功能

| 功能     | 当前实现                                      |
| -------- | --------------------------------------------- |
| 摄像头   | OV3660，DVP 接口，RGB565 采集                 |
| 分辨率   | 640×480 @ 10 FPS；240×240 @ 24 FPS            |
| LCD      | 800×480 RGB565，LVGL 实时预览                 |
| 人脸检测 | ESP-DL `HumanFaceDetect`，MSR+MNP S8 模型     |
| 人脸识别 | ESP-DL `HumanFaceRecognizer`，MFN S8 特征模型 |
| 人脸库   | `/spiffs/face_db.bin`                         |
| 人脸录入 | 由上位机上传裁剪后的 RGB565 人脸图像          |
| 实时视频 | TCP 5000 私有协议、HTTP MJPEG                 |
| SD 录像  | 自动启动，MJPEG AVI，每 5 分钟分段            |
| 循环覆盖 | SD 剩余空间低于 256 MiB 时删除最旧 AVI        |
| 网络     | Wi-Fi STA；无配置时提供设置热点               |
| 校时     | SNTP 优先，HTTP `Date` 响应头作为回退         |
| OTA      | `ota_0` / `ota_1` 双分区，支持回滚确认        |

### 1.2 上位机功能

`host/qt_monitor` 是 Python + PySide6 编写的桌面上位机：

- 同时连接多台设备；
- 显示实时 JPEG 画面；
- 显示板端返回的人脸框、人名和置信度；
- 上传人脸照片并录入人脸库；
- 查询、删除、清空人脸；
- 调节识别阈值；
- 切换摄像头分辨率；
- 抓拍；
- 电脑端分段录像及异常录像恢复；
- 上传 OTA 固件并显示进度；
- 每 2 秒读取设备状态。

> 板端 SD 录像与上位机录像是两套互相独立的功能。关闭上位机不会停止板端录像。

---

## 2. 硬件与软件环境

### 2.1 硬件假设

- 主控：ESP32-S31，双核，当前运行频率 320 MHz；
- Flash：16 MiB；
- PSRAM：16 MiB；
- 摄像头：OV3660；
- LCD：800×480 RGB565；
- 触摸：GT1151；
- SD 卡：SDMMC 接口；
- 当前实际使用 1-bit、10 MHz SDMMC 模式，以提高持续录像稳定性。

SDMMC 原本由 BSP 配置为 4-bit 高速模式。在摄像头、LCD、Wi-Fi 同时工作时曾出现：

```text
sdmmc_write_blocks failed (0x107)
```

`0x107` 是超时。当前代码在 `storage_service` 中主动降为 1-bit、10 MHz；其带宽仍足够保存 640×480、10 FPS 的 MJPEG。

### 2.2 工具链

项目当前按以下环境开发和验证：

- ESP-IDF 6.1；
- 目标芯片：`esp32s31`；
- CMake + Ninja；
- Python 虚拟环境用于 ESP-IDF 和上位机；
- Windows PowerShell。

ESP32-S31 属于较新的目标芯片，请使用项目已经验证过的 ESP-IDF 6.1 工具链，不建议随意换成旧版 IDF。

---

## 3. 目录结构

```text
ESP32_S31_CAM/
├─ CMakeLists.txt                 ESP-IDF 项目入口
├─ sdkconfig.defaults             默认构建配置
├─ partitions.csv                 16 MiB Flash 分区表
├─ main/
│  ├─ main.c                      app_main()，系统入口
│  └─ idf_component.yml           IDF 组件依赖
├─ components/
│  ├─ app_core/                   系统启动顺序和全局状态
│  ├─ board_service/              LCD、触摸、摄像头硬件抽象
│  ├─ media_service/              摄像头采集、LCD预览、识别调度
│  ├─ jpeg_service/               硬件 JPEG 编码与双缓冲
│  ├─ face_detection_service/     异步人脸检测
│  ├─ face_recognition_service/   特征提取、匹配和人脸数据库
│  ├─ storage_service/            SPIFFS 与 microSD 挂载
│  ├─ recording_service/          板端 MJPEG AVI 循环录像
│  ├─ network_service/            Wi-Fi、TCP、HTTP、校时、OTA入口
│  ├─ stream_protocol/            TCP 32字节帧头编解码
│  ├─ ota_service/                ESP-IDF OTA 分区写入封装
│  └─ settings_service/           LCD触摸Wi-Fi设置页面
├─ host/qt_monitor/               Python Qt 上位机
│  ├─ main.py
│  ├─ transport/                  TCP协议、REST客户端
│  ├─ recording/                  上位机 AVI 与缓存
│  ├─ ui/                         窗口和设备面板
│  └─ tests/                      协议和录像单元测试
└─ docs/
   ├─ TCP_PROTOCOL.md             TCP/HTTP协议补充说明
   └─ PROJECT_PLAN.md             早期项目规划
```

`managed_components/` 是 ESP-IDF Component Manager 下载的第三方组件，不应把业务逻辑直接写进去。若必须临时修改 BSP，升级依赖时要特别检查这些修改是否被覆盖。

---

## 4. 总体软件架构

系统采用“服务组件 + 数据流水线”的结构。`app_core` 只负责按顺序初始化和启动服务，具体业务由各组件完成。

```text
OV3660
  │ RGB565 / V4L2 DVP
  ▼
media_service capture_task (Core 1)
  ├─► LCD画布 / LVGL预览 ─► LCD框、人名、置信度
  ├─► 每5帧提交一次 ─────► face_detection_service
  │                           └─► face_recognition_service
  └─► jpeg_service硬件编码
          ├─► network_service ─► TCP 5000 / HTTP MJPEG
          └─► recording_service ─► SD卡 MJPEG AVI
```

这里有一个非常重要的设计：

- LCD 上的人脸框由 LVGL 在显示阶段绘制；
- JPEG 编码使用的是摄像头 RGB565 画布；
- 因此 SD 录像和网络 JPEG **不包含 LCD 的框、人名和置信度**；
- 上位机收到板端 `FACE_RESULTS` 后，自己在显示控件上叠加框。

换句话说，框是“显示叠加层”，不是被写进原始视频像素的内容。

---

## 5. 系统启动流程

入口位于 `main/main.c`：

```text
app_main()
  ├─ app_core_init()
  └─ app_core_start()
```

### 5.1 初始化阶段 `app_core_init()`

顺序如下：

1. 创建系统状态互斥锁；
2. `board_service_init()`：启动 LCD/LVGL、触摸和 OV3660；
3. `storage_service_init()`：挂载内部 SPIFFS；
4. `face_detection_service_init()`：分配检测缓冲并创建模型任务；
5. `media_service_init()`：分配预览画布、创建 LCD UI、初始化 JPEG 编码器；
6. `network_service_init()`：初始化 NVS、TCP/IP、Wi-Fi；
7. `settings_service_init()`：创建触摸 Wi-Fi 页面；
8. `ota_service_init()`：初始化 OTA 后端状态。

### 5.2 启动阶段 `app_core_start()`

1. 标记板级服务已启动；
2. 挂载 SD 卡；
3. 加载人脸识别器和 SPIFFS 人脸库；
4. 启动摄像头采集任务；
5. SD 已挂载时自动启动录像任务；
6. 启动 HTTP 和 TCP 服务；
7. 状态切换为 `APP_STATE_READY`；
8. 如果本次从新 OTA 分区启动，确认镜像有效，取消回滚。

任何关键步骤返回错误时，`ESP_RETURN_ON_ERROR` 会中止后续启动，并在串口打印具体组件名称。

---

## 6. 摄像头与媒体流水线

### 6.1 采集格式

摄像头通过 ESP Video/V4L2 风格接口采集 RGB565。当前默认模式：

```text
640 × 480, RGB565 BE, 10 FPS
```

另一档模式：

```text
240 × 240, RGB565 BE, 24 FPS
```

摄像头在板上物理倒装，因此 `media_service` 在开始视频流前设置传感器垂直/水平方向修正，使 LCD、JPEG、人脸检测和录入使用相同方向。

### 6.2 `capture_task`

采集任务固定在 Core 1，优先级 5。它循环执行：

1. 从 DVP 取出一帧；
2. 拷贝或转换到 PSRAM RGB565 画布；
3. 更新 LCD 图像；
4. 每 5 帧把图像提交给异步人脸检测；
5. 使用最新检测结果做人脸识别；
6. 调用硬件 JPEG 编码；
7. 归还摄像头缓冲区。

摄像头采集是系统实时性的核心。耗时操作不能随意放进该任务，否则会降低帧率、增加 DVP 缓冲堆积，甚至触发看门狗。

### 6.3 分辨率切换

上位机调用：

```http
POST /api/v1/camera/mode?width=640&height=480
```

切换流程会停止当前采集、等待 JPEG 引用释放、重新配置 OV3660 和 JPEG 编码器，然后恢复采集。TCP 客户端不需要断线重连，每个 JPEG 帧头都携带自己的宽高。

---

## 7. JPEG 服务

`jpeg_service` 使用 ESP32-S31 硬件 JPEG 编码器，将 RGB565 转成 JPEG，默认质量为 80。

内部使用两个输出槽：

- 一个槽可被网络发送或 SD 录像引用；
- 编码器尽量写另一个没有引用的槽；
- 消费者必须调用 `jpeg_service_release()` 释放引用；
- 两个槽都忙时，新 JPEG 会被丢弃，而不是阻塞摄像头采集。

这种设计优先保证“实时画面继续向前”，而不是保证每一帧都被网络或录像消费。

状态接口中的：

- `jpeg_frames`：成功编码数；
- `jpeg_dropped`：由于缓冲忙而丢弃的编码请求数。

---

## 8. 人脸检测与识别

### 8.1 人脸检测

检测模型输入为 224×224 RGB565。原始图像会先做中心正方形裁剪，再缩放到 224×224。

检测任务特点：

- Core 1；
- 优先级 3；
- 两个 PSRAM 输入缓冲；
- 最多返回 4 张脸；
- 返回框、检测分数和 5 个关键点；
- 异步执行，不阻塞摄像头任务；
- 推理跟不上时覆盖旧待处理帧，只保留最新帧。

因此 `face_dropped` 增长并不一定代表错误，它表示检测速度低于摄像头帧率，系统主动跳过了旧帧。

### 8.2 人脸识别

识别器使用检测模型提供的 5 点关键点进行人脸对齐和特征提取。数据库默认存放在：

```text
/spiffs/face_db.bin
```

识别采用以下判定条件：

1. 最佳相似度必须高于阈值；
2. 最佳结果与第二名相似度差至少为 `0.08`；
3. 媒体层需要连续命中 2 次才确认身份；
4. 已确认身份在短暂漏检时保持约 1.5 秒，减少文字闪烁。

默认阈值为 `0.30`。上位机可在 0.10～0.90 范围调节。阈值越高：

- 误识别概率越低；
- 同一个人在角度、光线变化时更容易显示 unknown。

当前实际相似度常见在 0.35～0.50，因此不应机械照搬桌面级人脸模型的高阈值。

### 8.3 人脸数据库与姓名的分离存储

理解下面两个概念的区别很重要：

|          | `face_db.bin`（人脸数据库）                                      | `s_face_names`（人名表）                            |
| -------- | ---------------------------------------------------------------- | --------------------------------------------------- |
| 管理者   | `HumanFaceRecognizer`（ESP-DL 库）                               | `media_service`（应用层）                           |
| 存储位置 | SPIFFS 文件系统                                                  | NVS 键值分区（`media_cfg` 命名空间）                |
| 内容     | 512 维 `float32` 特征向量                                        | ID → 人名字符串                                     |
| 单条大小 | ~2048 字节                                                       | 最大 24 字节                                        |
| 上限     | 取决于 SPIFFS 可用空间（2.8 MiB）                                | 当前 32 人，可按需调大                              |
| 读写接口 | `face_recognition_service_enroll()` / `delete()` / `recognize()` | `media_service_set_face_name()` / `get_face_list()` |

为什么分开？`HumanFaceRecognizer` 是一个通用数学库，它只关心"ID=1 的特征向量像不像当前这个人"，不知道也不需要知道这个人叫什么。名字是应用层的概念，由 `media_service` 单独管理。录入流程中，上位机先上传人脸图像完成特征入库（获得 ID），再设置该 ID 对应的姓名——两个操作分别写入 SPIFFS 和 NVS。

**ID 约定**：

- `id = 0`：未识别或 unknown；
- `id >= 1`：人脸库中的用户；
- 人名表最大 `FACE_NAME_MAX = 32` 个条目，每个名字最长 `FACE_NAME_LEN - 1 = 23` 个有效字符（UTF-8）；
- 设备重启后姓名自动从 NVS 恢复，不会丢失。

**人脸数据库容量分析**：

MFN S8 特征模型输出 512 维 `float32` 向量，每人约占用 **2048 字节**。当前 `FACE_NAME_MAX = 32`，满库约 **64 KiB**，仅占 SPIFFS 分区（2.8125 MiB）的 2.2%。即使在极端场景下将上限调到 512 人（~1 MiB），SPIFFS 仍有充足余量。当前瓶颈不在存储空间，而在识别器需要把全库特征载入 PSRAM 做逐一比对——人数越多，单次识别耗时越长。

**姓名容量分析**：

NVS 分区为 24 KiB，Wi-Fi 凭证等占用约 1~2 KiB 后，剩余约 22 KiB 可用于姓名 blob。每条姓名占 24 字节，理论可存储约 **900 人** 的姓名。当前 `FACE_NAME_MAX = 32` 仅占用 768 字节（3.2%），远未触及上限。如需扩大，只需修改 `media_service.c` 中的宏并重新编译，无需调整分区表。

### 8.4 上位机上传录入

当前项目没有依赖现场按键录入。典型流程为：

1. 用户在上位机选择照片并输入姓名；
2. 上位机检测/裁剪并转换成 RGB565；
3. 通过 TCP `FACE_ENROLL` 上传；
4. 板端在视觉核心执行同步检测和特征录入；
5. 特征写入 SPIFFS 人脸库；
6. 板端返回新 ID。

上传尺寸和负载必须符合协议，否则会返回录入失败。

---

## 9. LCD 与触摸界面

LCD 由 BSP 和 LVGL 驱动。主要显示内容：

- 摄像头预览；
- 当前分辨率和帧率；
- RGB/JPEG 帧计数；
- 人脸识别阈值；
- 人脸框；
- 人名与置信度；
- Wi-Fi 设置入口及设备 IP。

LVGL 不是线程安全的。修改 LVGL 对象前应使用：

```c
board_service_display_lock(timeout_ms);
/* LVGL 操作 */
board_service_display_unlock();
```

曾经的 GT1151/I2C 看门狗问题通过有限 I2C 事务超时缓解。若日志显示 `taskLVGL` 长时间停在 I2C 驱动，应优先检查触摸排线、电源和 I2C 超时配置，而不是误判为人脸模型卡死。

---

## 10. 板端 SD 自动录像

### 10.1 工作方式

只要：

- SD 卡挂载成功；
- 摄像头媒体服务启动；

录像服务就自动启动，不需要上位机命令。

录像直接复用 `jpeg_service` 已经编码好的 JPEG，不重复做图像编码，因此额外 CPU 开销较低。

### 10.2 文件位置与命名

目录：

```text
/sdcard/recordings/
```

完成文件：

```text
REC_YYYYMMDD_HHMMSS_NNN.avi
```

例如：

```text
REC_20261106_143025_001.avi
```

正在录制时会看到：

```text
REC_....avi.part
.record.idx
```

- `.avi.part`：尚未封装完成的当前视频；
- `.record.idx`：为了避免大量 AVI 索引占用 RAM，临时写在 SD 上的索引；
- 正常封段后会补写 AVI 头和 `idx1`，将 `.part` 重命名为 `.avi`，并删除 `.record.idx`。

### 10.3 分段与循环覆盖

- 每段 300 秒，即 5 分钟；
- 分辨率变化时立即结束旧段并创建新段；
- 剩余空间低于 256 MiB 时，按文件名删除最旧 `.avi`；
- 当前录像不会把 LCD 人脸框写入视频。

### 10.4 数据安全

录像每秒执行一次 `fflush()`，降低突然断电时的数据损失。仍需理解：

- FAT 文件系统无法保证突然拔卡完全无损；
- 当前活动段可能留下 `.avi.part`；
- 完成的 `.avi` 比活动 `.part` 更可靠；
- 不要在设备仍运行时直接拔出 SD 卡。

如果发生 SDMMC `0x107` 超时，录像器会停止，避免不断产生空文件；设备重启后才重新尝试录像。

---

## 11. 时间同步

ESP32 板上没有带后备电池的实时时钟。彻底断电后，它不知道真实年月日，因此必须联网校时。

当前流程：

1. Wi-Fi 通过 DHCP 获取 IP；
2. 强制设置主 DNS `223.5.5.5` 和备用 DNS `119.29.29.29`；
3. 优先使用 `ntp.aliyun.com` 做 SNTP 校时；
4. 8 秒内没有有效时间时，访问百度或腾讯的普通 HTTP 服务；
5. 从 HTTP `Date` 响应头解析 UTC 时间；
6. 失败时每 30 秒重试；
7. 时区设置为中国标准时间 `CST-8`。

只有出现以下日志才代表成功：

```text
time synchronized: 2026-11-06 14:30:25
```

可选的回退成功日志：

```text
time synchronized through HTTP Date fallback
```

仅出现：

```text
SNTP time synchronization started
```

不代表校时成功。

文件名为 `REC_19700101_...` 表示录制时系统仍未取得真实时间。设备完全离线且没有外部 RTC 时，软件无法推导真实日期。

---

## 12. 网络架构

### 12.1 Wi-Fi 模式

- 已保存路由器配置：进入 STA 模式，通过 DHCP 获取 IP 和 DNS；
- 未保存配置：创建 `ESP32-S31-CAM` 设置热点；
- 默认热点密码：`esp32s31`；
- LCD Wi-Fi 页面可输入 SSID 和密码；
- 配置保存在 NVS 命名空间 `camera_net`。

设备 ID 根据 Wi-Fi MAC 后三字节生成，例如：

```text
S31CAM-F3C56C
```

### 12.2 TCP 视频服务

- 端口：5000；
- 最大客户端：2；
- 建立连接后先发送 `HELLO`；
- 随后持续发送 `JPEG_FRAME` 和变化后的 `FACE_RESULTS`；
- 同一 TCP 连接也接收人脸管理和阈值命令。

TCP 是字节流，上位机解析器必须同时处理：

- 一个包被拆成多次接收；
- 多个包一次接收；
- 错位后的 magic 重新同步。

### 12.3 HTTP 接口

| 方法 | 路径                   | 用途                     |
| ---- | ---------------------- | ------------------------ |
| GET  | `/`                    | 简单浏览器预览页面       |
| GET  | `/stream.mjpeg`        | HTTP MJPEG 实时流        |
| GET  | `/api/v1/status`       | 设备、媒体、人脸、SD状态 |
| GET  | `/api/v1/capabilities` | 支持的能力和分辨率       |
| POST | `/api/v1/stream/start` | 恢复网络视频发送         |
| POST | `/api/v1/stream/stop`  | 暂停网络视频发送         |
| POST | `/api/v1/camera/mode`  | 切换分辨率               |
| POST | `/api/v1/ota`          | 上传 ESP 应用固件        |

暂停推流只暂停发送，不停止摄像头、LCD 或 SD 录像。

## 13. TCP 私有协议概览

每条消息由固定 32 字节大端帧头和可选负载组成：

| 偏移 | 长度 | 字段                         |
| ----:| ----:| ---------------------------- |
| 0    | 4    | magic，`S31V` / `0x53333156` |
| 4    | 1    | version，当前为 1            |
| 5    | 1    | message type                 |
| 6    | 2    | flags                        |
| 8    | 4    | sequence                     |
| 12   | 8    | timestamp_us                 |
| 20   | 2    | width                        |
| 22   | 2    | height                       |
| 24   | 4    | payload_length               |
| 28   | 4    | 前 28 字节 CRC32-BE          |

主要消息类型：

| 值  | 名称               | 方向          |
| ---:| ------------------ | ------------- |
| 1   | HELLO              | 板端 → 上位机 |
| 3   | JPEG_FRAME         | 板端 → 上位机 |
| 12  | FACE_RESULTS       | 板端 → 上位机 |
| 20  | FACE_ENROLL        | 上位机 → 板端 |
| 21  | FACE_DELETE        | 上位机 → 板端 |
| 22  | FACE_CLEAR         | 上位机 → 板端 |
| 23  | FACE_LIST          | 上位机 → 板端 |
| 24  | FACE_ENROLL_ACK    | 板端 → 上位机 |
| 25  | FACE_LIST_REPLY    | 板端 → 上位机 |
| 26  | FACE_THRESHOLD_SET | 上位机 → 板端 |
| 27  | FACE_THRESHOLD_ACK | 板端 → 上位机 |

更详细的负载格式见 `docs/TCP_PROTOCOL.md` 和 `host/qt_monitor/transport/protocol.py`。

---

## 14. Flash、NVS、SPIFFS 与 SD 卡的区别

### 14.1 Flash 分区表

| 分区     | 偏移       | 大小       | 用途                    |
| -------- | ----------:| ----------:| ----------------------- |
| nvs      | `0x9000`   | 24 KiB     | Wi-Fi配置、人脸姓名映射 |
| otadata  | `0xF000`   | 8 KiB      | OTA启动分区状态         |
| phy_init | `0x11000`  | 4 KiB      | RF校准数据              |
| ota_0    | `0x20000`  | 6.5 MiB    | 应用槽 A                |
| ota_1    | `0x6A0000` | 6.5 MiB    | 应用槽 B                |
| app_data | `0xD20000` | 2.8125 MiB | SPIFFS人脸库            |

### 14.2 三类持久化数据

| 类型        | 分区/介质             | 大小         | 典型内容                                |
| ----------- | --------------------- | ------------ | --------------------------------------- |
| NVS         | 内部 Flash `nvs`      | 24 KiB       | Wi-Fi SSID/密码、人脸姓名映射表         |
| SPIFFS      | 内部 Flash `app_data` | 2.8125 MiB   | `/spiffs/face_db.bin`（人脸特征向量库） |
| microSD/FAT | 外部 SD 卡            | 取决于卡容量 | `/sdcard/recordings/` MJPEG AVI 录像    |

三种存储互相独立：清空 NVS（`idf.py erase-flash`）不会删除 SPIFFS 人脸库；拔掉 SD 卡也不会丢失人脸数据。只有全片擦除 Flash 或格式化 SPIFFS 才会清空人脸库。

**NVS 中的姓名数据**：Wi-Fi 配置保存在 `camera_net` 命名空间，人脸姓名保存在 `media_cfg` 命名空间，互不干扰。姓名以单个 blob 键整体读写（32 × 24 = 768 字节），当前配置下 NVS 剩余空间可容纳约 900 人的姓名，远大于 `FACE_NAME_MAX` 上限。

**SPIFFS 中的人脸数据库**：`face_db.bin` 由 ESP-DL `HumanFaceRecognizer` 类直接以二进制格式管理，不通过标准文件 API 读写。每条记录为 512 维 `float32` 特征向量（~2048 字节）。数据库大小随录入人数线性增长，32 人满库约 64 KiB，SPIFFS 的 2.8 MiB 容量在可预见的嵌入式场景中不会成为瓶颈。

**PHY 校准数据**：`phy_init` 分区（4 KiB）存放 ESP32 WiFi/BLE 射频的自校准参数。芯片在首次上电时测量发射功率偏移、IQ 失衡等射频特性并将结果写入此分区，后续启动直接读取，避免每次校准增加数百毫秒的 WiFi 初始化延迟。数据损坏时驱动会自动重新校准并写回，功能上透明恢复。

---

## 15. OTA 升级

### 15.1 升级流程

1. 上位机选择 `build/ESP32_S31_CAM.bin`；
2. HTTP `POST /api/v1/ota`，并携带 `X-OTA-Token`；
3. 板端暂停推流、检测、识别和 SD 录像；
4. 停止摄像头采集；
5. 擦除非当前 OTA 分区；
6. 以 4096 字节块写入固件；
7. 校验镜像并设置下一启动分区；
8. 返回成功 JSON；
9. 软件重启；
10. 新镜像完成完整启动后调用确认接口，取消自动回滚。

默认 OTA Token：

```text
s31cam-ota
```

部署到不可信网络前必须修改。

### 15.2 为什么上传 100% 后还要等待

上位机的“100%”表示电脑已经把 HTTP 请求体交给网络栈，不表示设备已经完成：

- Flash 分区擦除；
- 固件写入；
- ESP 镜像校验；
- 切换启动分区；
- 重启。

这些步骤通常还需 10～30 秒。OTA 期间上位机会暂停状态轮询，板端也暂停 HTTP 校时，避免并发请求干扰升级。

成功日志示例：

```text
OTA target: ota_1
OTA image accepted: 6203200 bytes; rebooting
Loaded app from partition at offset 0x6a0000
confirming booted OTA image in ota_1
```

---

## 16. 编译固件

### 16.1 初始化 ESP-IDF 环境

若已经安装 ESP-IDF 6.1，可在 PowerShell 中使用 IDF 自带环境脚本，或者设置项目当前使用的环境：

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.1\venv'
$env:ESP_IDF_VERSION = '6.1'
$env:PYTHONUTF8 = '1'
```

确保 CMake、Ninja、RISC-V 工具链和 IDF Python 环境在 `PATH` 中。

### 16.2 构建

在项目根目录运行：

```powershell
idf.py build
```

成功后主要产物：

```text
build/ESP32_S31_CAM.bin
build/ESP32_S31_CAM.elf
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/ota_data_initial.bin
```

### 16.3 首次烧录

```powershell
idf.py -p COMx flash monitor
```

首次烧录需要写入 bootloader、分区表、OTA 数据和应用。后续 OTA 只上传应用 `.bin`，所以通常比串口全量烧录快很多。

### 16.4 仅监视串口

```powershell
idf.py -p COMx monitor
```

退出 IDF Monitor 通常使用 `Ctrl+]`。

---

## 17. 运行上位机

```powershell
cd host\qt_monitor
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe main.py
```

电脑和设备必须位于同一局域网。连接参数：

- IP：以 LCD 或串口显示为准；
- TCP 端口：5000；
- HTTP 端口：80。

### 17.1 上位机测试

必须从 `host/qt_monitor` 目录运行，否则 Python 找不到 `transport` 和 `recording` 包：

```powershell
cd host\qt_monitor
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

当前测试覆盖：

- TCP 拆包、粘包和重新同步；
- 人脸结果解析；
- 阈值协议；
- AVI 写入和异常恢复；
- 多设备缓存和导出；
- REST JSON 解析。

---

## 18. 常用日志如何阅读

### 正常启动

```text
app_main: ESP32-S31 vision system boot
board_service: RGB LCD and GT1151 touch initialized
storage_service: microSD mounted: ..., capacity ... MiB (1-bit, 10 MHz)
face_detect: MSR+MNP face/landmark model ready
face_recog: face recognition ready
recording_service: recording started: ...
network_service: router connected: ...
network_service: DNS configured: primary=223.5.5.5 backup=119.29.29.29
app_main: system entered READY state
```

### 人脸识别

```text
recog raw[0]: id=1 sim=0.3812
recog MATCH: id=1 sim=0.3812 margin=0.3812 thr=0.30
```

- `sim`：相似度；
- `margin`：第一名减第二名；
- `thr`：当前阈值；
- `MISS`：没有满足最终确认条件；
- `HIT`：媒体层确认身份。

### SD 录像

```text
recording started: ...
recording finalized: ..., 3000 frames
```

异常：

```text
sdmmc_write_blocks failed (0x107)
```

表示 SDMMC 总线超时，不是 AVI 格式错误。

### 校时

```text
SNTP time synchronization started
```

仅代表开始。必须继续看到：

```text
time synchronized: ...
```

### OTA

```text
OTA image accepted: ...; rebooting
```

说明设备端已接受完整镜像。随后连接断开和 `SW_CPU_RESET` 是正常升级重启。

---

## 19. 常见问题排查

### 19.1 文件名一直是 1970

原因：设备尚未取得真实时间。

检查：

1. 是否获得路由器 IP；
2. 是否打印公共 DNS 配置成功；
3. 是否出现 `getaddrinfo returns 202`；
4. 是否出现 `time synchronized`；
5. 网络是否允许 UDP NTP 或普通 HTTP 出网。

### 19.2 SD 卡出现大量 0 字节 `.part`

检查串口是否存在：

```text
sdmmc_write_blocks failed
SD_TRANS
```

当前代码在写入失败后会停止录像，不应无限创建空文件。如果仍出现，请确认设备运行的是最新固件，并检查 SD 卡、电源和卡座接触。

### 19.3 `.avi.part` 能否直接播放

通常不能保证。它表示 AVI 尚未完成索引和文件头回填。正常等待 5 分钟封段后会得到 `.avi`。

### 19.4 录像为什么没有人脸框

这是设计行为。框由 LCD 或上位机显示层叠加，板端 SD 保存的是原始 JPEG 视频。

### 19.5 置信度为什么只有 0.35～0.50

这是当前嵌入式模型、录入图像、摄像头角度和光线共同决定的相似度范围，不等同于概率。应结合误识别率调阈值，而不是要求数值必须接近 1.0。

### 19.6 OTA 看起来停在 100%

等待 10～30 秒并观察串口。上传 100% 后设备还要擦写、校验和重启。不要在此阶段断电。

### 19.7 `CPU_LOCKUP_RESET` 或 LVGL 看门狗

如果 PC 位于 I2C 主机驱动，优先检查 GT1151 触摸通信；如果位于 SDMMC，检查 SD 总线；如果位于模型推理，检查是否把同步重任务放进了实时/LVGL任务。

---

## 20. 修改代码时的建议

1. **保持采集任务轻量**：网络、SD和模型推理应异步处理。
2. **JPEG引用必须释放**：每次成功 `acquire_latest()` 后都要对应 `release()`。
3. **LVGL必须加锁**：不要从普通 FreeRTOS 任务直接操作控件。
4. **不要随意增大并发客户端数**：每个客户端都会占任务栈、socket和JPEG引用。
5. **不要让SD恢复到未经验证的高速模式**：持续写入比一次文件测试更容易暴露信号问题。
6. **协议两端同步修改**：消息类型或负载改变时，同时更新板端、Python解析器和测试。
7. **OTA分区大小是硬上限**：应用镜像必须小于 `0x680000`。
8. **修改分区表后不能只做普通OTA**：分区表、bootloader变化通常需要串口完整烧录。
9. **区分检测分数和识别相似度**：二者含义不同，不能共用同一阈值。
10. **先保留完整串口日志**：问题发生前后各保留至少30秒，比只截最后一行更容易定位。

---

## 21. 当前已知限制

- 没有电池 RTC，完全离线冷启动无法获得真实日期；
- 板端活动 `.avi.part` 尚未提供完整的断电自动修复流程；
- 人脸特征库与人名表分属 SPIFFS 和 NVS 两个独立存储，无跨分区事务保证；
- 识别模型适合嵌入式实时场景，精度和鲁棒性不等同于服务器模型；
- OTA Token 是共享静态密钥，HTTP OTA 本身未使用 TLS，适合可信局域网；
- `settings_service` 的部分界面文字仍是英文；
- HTTP 时间回退依赖外网域名和公共 DNS；
- 当前录像为 MJPEG AVI，文件较大，但实现简单、逐帧独立、适合嵌入式恢复。

---

## 22. 推荐阅读顺序

如果要继续维护项目，建议按以下顺序阅读：

1. `main/main.c`：入口很短，先理解整体启动；
2. `components/app_core/app_core.c`：理解组件顺序；
3. `components/media_service/media_service.c`：理解主数据流；
4. `components/jpeg_service/jpeg_service.c`：理解共享JPEG帧；
5. `components/face_detection_service/` 与 `face_recognition_service/`；
6. `components/recording_service/recording_service.c`；
7. `components/network_service/network_service.c`；
8. `components/stream_protocol/` 与 `host/qt_monitor/transport/`；
9. `host/qt_monitor/ui/device_panel.py`：理解用户操作如何映射到板端接口。

按照这个顺序，可以先建立“数据如何流动”的全局认识，再进入协议和UI细节。