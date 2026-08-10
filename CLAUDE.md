# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Wall-E 机器人复刻版的控制仓库，包含多套固件与一套树莓派 Web 控制端：

1. **Arduino 固件** (`archive/arduino-pi/wall-e/`)：原始方案，控制电机与舵机，通过 USB 串口接收指令（Arduino UNO + 树莓派）。
2. **Raspberry Pi Web 服务器** (`archive/arduino-pi/web_interface/`)：基于 Flask 的 Web 控制界面，通过串口向 Arduino 下发指令，并可选地接入 Pi 摄像头进行 MJPEG 视频推流。
3. **小智语音固件** (`wall-e_xiaozhi/`)：**当前主线**。单块 ESP32-S3 运行 vendored 小智 ESP-IDF 固件（上游 78/xiaozhi-esp32 @ e0074e9），板型代码在 `main/boards/walle/`：唤醒词 + 云端 LLM + MCP 动作工具 + 运动核心 + ST7789 状态屏（眼睛屏在 CAM 模组上，主控侧禁用）+ Web 控制面板（:80，API 兼容 Flask 版）+ Wi-Fi/4G 双网（Wi-Fi 优先，超时自动回退 ML307R-DL 4G（mini 核心板，GPIO2/3 经 J-4G 线束接入，吃 5V）；USB 串口协议默认禁用——GPIO19/20 原生 USB 被 PWMA/舵机 I2C 占用）。迁移细节见 `docs/XIAOZHI_MIGRATION.md`。
4. **ESP32-S3-CAM 推流固件** (`wall-e_esp32_cam/`)：第二块摄像板的 MJPEG 推流 + microSD 拍照录像 + **眼睛屏驱动**（2× 圆屏共享 SPI，`/eyes?expr=` 表情端点）。与主控经 UART 交互（CAM_PROTOCOL v1，见 `docs/CAM_PROTOCOL.md`）。

硬件接线、舵机标定、电池检测等说明详见 `README.md`（另有中文版 `README.zh-CN.md`）。`docs/` 下有八份中文文档：`docs/WIRING.md`（Arduino<->舵机板/电机板接线）、`docs/SERIAL_PROTOCOL.md`（串口通信协议，见下方「串口指令协议」）、`docs/HARDWARE.md`（硬件采购清单）、`docs/XIAOZHI_MIGRATION.md`（小智语音迁移：构建、引脚、MCP 工具、Web 面板）、`docs/NEW_HARDWARE_MIGRATION.md`（新硬件方案迁移进度与待办）、`docs/CAM_PROTOCOL.md`（主控↔CAM UART 交互协议，已定稿且两侧固件已实现，待实机联调）、`docs/VOICE_LLM_PLAN.md`（树莓派侧语音大模型方案，备选）、`docs/REID_FOLLOW_PLAN.md`（视觉目标再识别/跟随方案，尚未实现）。

> **新硬件 PCB 设计输入（自研方案 A：双层堆叠主板 + 摄像头背板 + 副板）** 见 `hardware/`：`另一套硬件方案.md`（第三方套件 BOM 转录）、`新硬件BOM与尺寸.md`（BOM 与模块实测尺寸）、`主板设计说明.md`（连接器逐脚定义 / 电源树 / 布局）、`主板布局示意图.html`（等比例布局图，含主板顶/底两面、摄像头背板、副板视图）；参考照片 `另一套硬件-{主板,副板,摄像头背板}.png`。

> 更详尽的 AI 助手导览（完整仓库结构树、技术栈、构建部署步骤、测试策略）见根目录 `AGENTS.md`；本文档只保留高频使用的命令与跨文件架构要点。

## 常用命令

```bash
# 手动启动 Web 服务器（调试时用，可看到错误日志）
python3 archive/arduino-pi/web_interface/app.py        # 默认监听 0.0.0.0:5000，生产用 waitress

# 在树莓派上首次安装依赖并注册开机自启服务
sudo chmod +x ./archive/arduino-pi/raspi-setup.sh && sudo ./archive/arduino-pi/raspi-setup.sh

# systemd 服务管理（服务名 walle.service）
sudo systemctl status|start|stop|enable|disable walle.service

# 查看可用串口（用于在 config.py / 设置页中选择 Arduino 端口）
dmesg | grep tty

# 小智固件（ESP-IDF，Windows 上用 eim/ESP-IDF PowerShell 激活环境）
cd wall-e_xiaozhi
idf.py set-target esp32s3
idf.py menuconfig                 # Xiaozhi Assistant -> Target Board -> "Wall-E Voice Robot"
idf.py build && idf.py -p COMx flash monitor
```

- 仓库**没有测试套件，也没有配置 linter**。Arduino 端通过 Arduino IDE 编译上传（`archive/arduino-pi/wall-e/wall-e.ino`，需在库管理器安装 `Adafruit PWMServoDriver`，可选 `U8g2`）。
- 配置覆盖：`app.py` 启动时若存在 `archive/arduino-pi/web_interface/local_config.py` 则优先加载它，否则加载 `config.py`。`local_config.py` 已被 gitignore，本地口令/端口等敏感改动应写在这里，不要改 `config.py`。

## 架构要点

### 串口指令协议（两侧契约）

Web 端 `ArduinoDevice.send_command(cmd)` 把字符串 + `\n` 写入串口；Arduino 端 `evaluateSerial()`（`archive/arduino-pi/wall-e/wall-e.ino`）解析。指令为「单字母前缀 + 数字」，缓冲区上限 `MAX_SERIAL_LENGTH = 5` 字符。**新增电机/舵机指令必须同时修改 `app.py` 的路由和 `wall-e.ino` 的 `evaluateSerial()`，否则两端不对应。**

| 前缀 | 范围 | 含义 | Flask 路由 |
|------|------|------|-----------|
| `X` | -100..100 | 左右转向（×2.55 → PWM） | `/motor` |
| `Y` | -100..100 | 前进/后退 | `/motor` |
| `S` | -100..100 | 转向偏置 | `/settings` |
| `O` | 0..250 | 电机死区补偿 | `/settings` |
| `M` | 0/1 | 舵机自动模式 关/开 | `/settings` |
| `A` | n | 播放动画 n（定义在 `animations.ino`） | `/animate` |
| `G/T/B` | 0..100 | 头部旋转 / 颈上 / 颈下（手动舵机） | `/servoControl` |
| `E/U` | 0..100 | 左眼 / 右眼 | `/servoControl` |
| `L/R` | 0..100 | 左臂 / 右臂 | `/servoControl` |
| `I/J` | 0..100 | 左眉毛 / 右眉毛（手动舵机） | `/servoControl` |
| `V` | 0..100 | 照明灯亮度（0=关；**仅小智固件**，PCA9685 空闲通道 `LIGHT_PWM_CHANNEL=9`） | `/servoControl` |
| `w/a/s/d/q`、`i/j/k/l` | 单字符 | WASD 行走 / 头部控制（仅 Arduino 串口监视器用） | — |

- Arduino 反向只发回 `Battery_<百分比>`，由 `ArduinoDevice.__parse_message()` 解析后供 `/arduinoStatus` 读取。
- 舵机位置 `0..100` 是归一化位置（非 PWM），由 `preset[][2]`（Arduino 版 `wall-e.ino:144` 由 `archive/arduino-pi/wall-e_calibration` 标定 / 小智版 `walle_motion.cc:53` 由顶层 `wall-e_esp32_calibration` 标定）线性映射到实际 PWM 脉宽；`-1` 表示该次动作跳过该舵机。逻辑关节顺序固定为 9 路：`0=head, 1=necT, 2=necB, 3=eyeR, 4=eyeL, 5=armL, 6=armR, 7=broL(左眉), 8=broR(右眉)`；PWM 板物理通道与逻辑关节之间隔了一层 `servoChannel[]` 映射（换线束只改这一张表）。
- 完整协议（含单字符调试指令全表、5 字符缓冲上限解析细节、`#define BAT_L` 启用条件）详见 `docs/SERIAL_PROTOCOL.md`。
- **小智主线落点**：串口任务与小智 Web 面板都是通用前缀转发，新指令只需在 `walle_motion.cc` 的 `EvaluateCommand(char prefix, int number)` 加分支 + `walle_mcp_tools.cc` 注册 MCP 工具 + `walle_web_server.cc` 内嵌 HTML 交互。照明灯（前缀 `V`）即按此模式实现。Arduino/树莓派旧方案则仍按上方「`app.py` 路由 + `wall-e.ino` `evaluateSerial()` + `static/js/main.js`」三处同步。

### Web 服务器结构

- `app.py` 是单体入口：定义 `ArduinoDevice` 类（独立线程读写串口、维护发送队列、解析电池消息），以及全部 Flask 路由。全局 `arduino` 实例在模块级创建。生产环境用 `waitress.serve`，调试模式才用 `app.run`。
- 视频推流由 `picamera2_stream.py` 独立实现：`PiCameraStreamer` 在**单独的 HTTP 服务器（端口 8080）**上提供 MJPEG 流，与 Flask 主服务（5000）分离。前端直接引用 8080 的流地址。
- 前端：`templates/index.html` + `static/js/main.js`（摇杆/按键/手柄 → 调用上述路由），`static/js/blockly/` + `automation*.js` 实现浏览器内的 CodeBlocks 拖拽编程（电机功率/速度常量见 `config.py` 的 `CODEBLOCK_*`）。

### 双 MCU 主线架构（主控 + CAM 模组，重要）

小智主线是**两块 ESP32-S3**：主控跑 `wall-e_xiaozhi/`（语音 + 运动 + Web 面板），CAM 模组跑 `wall-e_esp32_cam/`（MJPEG 推流 + microSD 拍照录像 + **眼睛屏**）。两者经 UART 交互（CAM_PROTOCOL v1，规范见 `docs/CAM_PROTOCOL.md`）：

- 主控侧 `walle_cam_link.cc`（`WalleCamLink`）：UART1 命令/事件 + 互斥 `Command()` + 连续超时判链路 down + HTTP 回退。MCP 工具（`walle_mcp_tools.cc` 里 `self.walle.camera` 的 photo/preview/record/stop）和 Web 面板（`POST /camview`）的摄像头调用**统一走这里**，不直接访问 CAM。链路 down 时拍照/录像/表情回退 `config.h` 的 `CAM_MODULE_URL` HTTP 端点；`SHOW`/`ABORT` 无 HTTP 等价、链路 down 即直接失败。
- CAM 侧 `cam_link.h`：`Serial1`（RX=14/TX=21）命令解析 + 拍照 3-2-1 倒计时 + 眼屏 JPEG 回放状态机（依赖 JPEGDEC）；`eye_display.h` 驱动 2× 圆屏（共享 HSPI：SCK=42/MOSI=45/DC=41/RST=46，CS 左=2/右=0），HTTP 端点 `/eyes?expr=neutral|sad|left|right`。
- **眼睛屏在 CAM 模组上**，主控侧 `EYE_DISPLAY_ENABLED=0`（`config.h`）；主控的显示输出是 ST7789 状态屏（`walle_status_display.cc`，独立 SPI：SCLK=14/MOSI=47/DC=40/RST=45/BL=42，1s 刷新电量/网络/状态）。`walle_cam_viewer.cc`（旧 HTTP 拉帧预览/AVI 回放）已废弃，仅保留备查。
- `self.walle.eyes` 保留主控机械眼舵机表情，并追加 `EYES <expr>` 经链路下发 CAM 眼屏。

### 计划中但尚未实现的视觉跟随 / 树莓派侧语音交互（重要）

> **状态说明（2026-07）**：语音交互的主线已改为小智单 MCU 方案（`wall-e_xiaozhi/`，已实现：唤醒词 + 云端 LLM + MCP 动作工具）。本节描述的树莓派侧语音/视觉管线仍是**未实现的备选方案**——若采用自建小智服务端接火山豆包，可参考 `docs/VOICE_LLM_PLAN.md` 的 LLM 函数调用设计；视觉跟随（Re-ID）在任何方案下都尚未落地。

⚠️ 仓库目前**只有** `app.py`、`config.py`、`picamera2_stream.py` 三个 Python 文件（外加 `static/`、`templates/`）。下述视觉/语音模块、对应配置段、`models/` 模型**都尚未提交进仓库**——它们只存在于 `docs/` 的设计文档里，属于计划中的工作，**不要去找这些文件，它们不存在**。落地实现以 `docs/VOICE_LLM_PLAN.md` 为准；其中规划的「感知 → 决策 → 控制」管线与接线契约（实现时参考）：

- `vision_tracker.py` — `VisionTracker`：基于 MediaPipe Tasks（TFLite 模型 `models/face_detector.tflite`）做人脸检测，返回 `(x,y,w,h,offset_x,offset_y,area_ratio)`，模型路径走配置项 `VISION_MODEL_PATH`。
- `robot_brain.py` — `RobotBrain`：状态机 `IDLE/FOLLOW/SEARCH/CHAT/AVOID`，按目标可见性、障碍距离、语音指令做状态转移。
- `follow_controller.py` — `FollowController`：把视觉目标的 `offset_x` / `area_ratio` 转成 Arduino 的 `X`/`Y` 指令（-100..100），含死区、限幅、方向反转。
- `voice_agent.py` — `VoiceAgent`：ASR（google 中文 / whisper）+ TTS（edge-tts，`zh-CN-XiaoxiaoNeural`，用 pygame 播放）+ 关键词指令解析（`follow/stop/greet`）。

配置段 `VISION_*` / `FOLLOW_*` / `VOICE_*` / `LLM_*` 计划加在 `config.py`（沿用全大写键名 + 注释风格），API Key 写 `local_config.py`。接线契约：`RobotBrain.update()` 的 `target_result` 应为 `VisionTracker.track()` 的 7 元组；`FollowController.compute()` 的输出经 `ArduinoDevice.send_command("X"..)` / `"Y"..` 下发。依赖（`mediapipe`、`speech_recognition`、`edge-tts`、`pygame`、`opencv-python`，Phase 1 再加 `openai`、`openwakeword`、`sounddevice`、`numpy`）未在 `raspi-setup.sh` 中安装，需另行 `pip install`。`docs/HARDWARE.md` 给出了为这套功能选型的硬件（Pi Camera Module 3、USB/I2S 麦克风、VL53L1X 测距、MAX98357A 功放等）。

## 编辑约定

- `config.py` 使用全大写键名，`app.py` 通过 `app.config['KEY']` 读取；新增可调参数请沿用此风格并加注释。
- Python 代码用类型注解（`str | None`、`Tuple[int, int]` 等），与现有模块保持一致。
- 舵机/电机相关改动必须同步 Arduino 端（见上方协议表），否则实际硬件不会响应。
