# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Wall-E 机器人复刻版的控制代码，由两部分组成：

1. **Arduino 固件** (`wall-e/`)：控制电机与舵机，通过 USB 串口接收指令。
2. **Raspberry Pi Web 服务器** (`web_interface/`)：基于 Flask 的 Web 控制界面，通过串口向 Arduino 下发指令，并可选地接入 Pi 摄像头进行 MJPEG 视频推流。

硬件接线、舵机标定、电池检测等说明详见 `README.md`。

## 常用命令

```bash
# 手动启动 Web 服务器（调试时用，可看到错误日志）
python3 web_interface/app.py        # 默认监听 0.0.0.0:5000，生产用 waitress

# 在树莓派上首次安装依赖并注册开机自启服务
sudo chmod +x ./raspi-setup.sh && sudo ./raspi-setup.sh

# systemd 服务管理（服务名 walle.service）
sudo systemctl status|start|stop|enable|disable walle.service

# 查看可用串口（用于在 config.py / 设置页中选择 Arduino 端口）
dmesg | grep tty
```

- 仓库**没有测试套件，也没有配置 linter**。Arduino 端通过 Arduino IDE 编译上传（`wall-e/wall-e.ino`，需在库管理器安装 `Adafruit PWMServoDriver`，可选 `U8g2`）。
- 配置覆盖：`app.py` 启动时若存在 `web_interface/local_config.py` 则优先加载它，否则加载 `config.py`。`local_config.py` 已被 gitignore，本地口令/端口等敏感改动应写在这里，不要改 `config.py`。

## 架构要点

### 串口指令协议（两侧契约）

Web 端 `ArduinoDevice.send_command(cmd)` 把字符串 + `\n` 写入串口；Arduino 端 `evaluateSerial()`（`wall-e/wall-e.ino`）解析。指令为「单字母前缀 + 数字」，缓冲区上限 `MAX_SERIAL_LENGTH = 5` 字符。**新增电机/舵机指令必须同时修改 `app.py` 的路由和 `wall-e.ino` 的 `evaluateSerial()`，否则两端不对应。**

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
| `w/a/s/d/q`、`i/j/k/l` | 单字符 | WASD 行走 / 头部控制（仅 Arduino 串口监视器用） | — |

- Arduino 反向只发回 `Battery_<百分比>`，由 `ArduinoDevice.__parse_message()` 解析后供 `/arduinoStatus` 读取。
- 舵机位置 `0..100` 在 Arduino 端通过 `preset[][2]`（`wall-e.ino` 约第 144 行，由 `wall-e_calibration` 标定得到）线性映射到实际 PWM 脉宽；`-1` 表示该次动作跳过该舵机。

### Web 服务器结构

- `app.py` 是单体入口：定义 `ArduinoDevice` 类（独立线程读写串口、维护发送队列、解析电池消息），以及全部 Flask 路由。全局 `arduino` 实例在模块级创建。生产环境用 `waitress.serve`，调试模式才用 `app.run`。
- 视频推流由 `picamera2_stream.py` 独立实现：`PiCameraStreamer` 在**单独的 HTTP 服务器（端口 8080）**上提供 MJPEG 流，与 Flask 主服务（5000）分离。前端直接引用 8080 的流地址。
- 前端：`templates/index.html` + `static/js/main.js`（摇杆/按键/手柄 → 调用上述路由），`static/js/blockly/` + `automation*.js` 实现浏览器内的 CodeBlocks 拖拽编程（电机功率/速度常量见 `config.py` 的 `CODEBLOCK_*`）。

### 进行中的自主跟随 / 语音交互集成（重要）

`web_interface/` 下新增了四个模块，构成一条「感知 → 决策 → 控制」管线，**但目前尚未被 `app.py` 导入或调用，属于半成品集成工作**：

- `vision_tracker.py` — `VisionTracker`：基于 MediaPipe Tasks（TFLite 模型 `models/face_detector.tflite`）做人脸检测，返回 `(x,y,w,h,offset_x,offset_y,area_ratio)`。模型路径在 `config.py` 的 `VISION_MODEL_PATH`，支持项目相对路径。
- `robot_brain.py` — `RobotBrain`：状态机 `IDLE/FOLLOW/SEARCH/CHAT/AVOID`，按目标可见性、障碍距离、语音指令做状态转移。
- `follow_controller.py` — `FollowController`：把视觉目标的 `offset_x` / `area_ratio` 转成 Arduino 的 `X`/`Y` 指令（-100..100），含死区、限幅、方向反转。
- `voice_agent.py` — `VoiceAgent`：ASR（google 中文 / whisper）+ TTS（edge-tts，`zh-CN-XiaoxiaoNeural`，用 pygame 播放）+ 关键词指令解析（`follow/stop/greet`）。

这些模块的配置已加入 `config.py` 的 `VISION_*` / `FOLLOW_*` / `VOICE_*` 段。接线时注意：`RobotBrain.update()` 期望 `target_result` 是 `VisionTracker.track()` 返回的 7 元组；最终 `FollowController.compute()` 的输出要经 `ArduinoDevice.send_command("X"..)` / `"Y"..` 下发。依赖（`mediapipe`、`speech_recognition`、`edge-tts`、`pygame`、`opencv-python`）未在 `raspi-setup.sh` 中安装，需另行 `pip install`。

## 编辑约定

- `config.py` 使用全大写键名，`app.py` 通过 `app.config['KEY']` 读取；新增可调参数请沿用此风格并加注释。
- Python 代码用类型注解（`str | None`、`Tuple[int, int]` 等），与现有模块保持一致。
- 舵机/电机相关改动必须同步 Arduino 端（见上方协议表），否则实际硬件不会响应。
