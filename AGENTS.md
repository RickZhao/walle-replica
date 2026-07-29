# Wall-E Robot Replica — AI Agent Guide

> 本文件面向不了解本项目的 AI 编程助手。此前项目根目录下**不存在** `AGENTS.md`，本文档根据仓库实际内容（源码、`docs/`、`CLAUDE.md`、`README.md`、`README.zh-CN.md` 等）整理而来。

## 1. 项目概述

这是一个可动 Wall-E 机器人复刻版的完整控制仓库。当前**主线是小智单 MCU 语音方案**（`wall-e_xiaozhi/`），两套旧方案已移入 `archive/` 归档（仍可编译回退，见 `archive/README.md`）：

1. **Arduino 固件**（`archive/arduino-pi/wall-e/`）：直接驱动电机、舵机，通过 USB 串口接收指令。
2. **Raspberry Pi Web 服务器**（`archive/arduino-pi/web_interface/`）：基于 Flask 的 Web 控制界面，通过串口向 Arduino 下发指令；可选接入 CSI 摄像头做 MJPEG 视频推流；支持浏览器虚拟摇杆、游戏手柄、TTS、Blockly 拖拽编程。

**当前存在三套固件格局**（2026-07 起）：

| 固件 | 框架 | 定位 | 状态 |
|------|------|------|------|
| `archive/arduino-pi/wall-e/` | Arduino (UNO) | 原始方案：UNO + 树莓派 | 维护中 |
| `archive/wall-e_esp32/wall-e_esp32/` | Arduino (ESP32-S3) | ESP32 单机版：内置 Web 控制端/音频/手柄/显示屏，可脱离树莓派 | **冻结**（保留作 Pi 方案回退） |
| `wall-e_xiaozhi/main/boards/walle/` | ESP-IDF | **主线**：小智语音交互单 MCU 方案（唤醒词 + 云端 LLM + MCP 动作 + GC9A01 眼睛 + 4G 回退） | **开发中** |

硬件接线、舵机标定、电池检测等说明见 `README.md` / `README.zh-CN.md`，详细接线与串口协议见 `docs/`，小智迁移细节见 `docs/XIAOZHI_MIGRATION.md`。

## 2. 仓库结构与代码组织

```
walle-replica/
├── wall-e_xiaozhi/               # ★ 主线：vendored 小智语音固件（ESP-IDF，上游 78/xiaozhi-esp32 @ e0074e9，MIT）
│   ├── main/boards/walle/       # Wall-E 板型（单 MCU 语音方案主战场）
│   │   ├── config.h             # 全部引脚定义（音频/显示/电机/舵机/电池/状态屏/Web/4G 模组）
│   │   ├── walle_board.cc       # 板类：GC9A01 显示 + NoAudioCodecSimplex + 按键 + 运动核心/状态屏/Web 启动
│   │   │                        #   + DualNetworkBoard：Wi-Fi 优先、连接超时自动回退 ML307A 4G（双击 BOOT 手动切网）
│   │   ├── walle_motion.cc/.h   # 运动核心：舵机动力学、电机斜坡、动画队列、电量（移植自 Arduino 版）
│   │   ├── walle_mcp_tools.cc   # MCP 工具注册（云端 LLM function calling → 动作/照明灯/摄像头）
│   │   ├── walle_serial.cc      # USB 串口协议任务（调试 + 树莓派回退）
│   │   ├── walle_web_server.cc  # Web 控制面板（esp_http_server :80，API 兼容 Flask 版，含摄像头画面区与预览/回放按钮）
│   │   ├── walle_cam_viewer.cc/.h # 眼睛屏照片预览/AVI 回放（HTTP Range 拉取 cam SD 卡文件，esp_new_jpeg 解码 → SetPreviewImage）
│   │   ├── walle_status_display.cc # ST7789 状态屏（第二个 LVGL 屏：电量/网络/状态，1s 刷新）
│   │   ├── pca9685.cc/.h        # LU9685/PCA9685 I2C 舵机驱动（自写，小智原生无此驱动）
│   │   └── config.json          # 批量构建配置
│   └── （其余为小智上游源码，详见 docs/XIAOZHI_MIGRATION.md）
├── archive/                     # 非主线方案归档（详见 archive/README.md）
│   ├── arduino-pi/              # 原始方案：Arduino UNO + 树莓派（跟随上游维护）
│   │   ├── wall-e/              # UNO 主控固件：wall-e.ino 主程序（初始化、串口解析、舵机/电机动力学、电池上报）、
│   │   │                        #   animations.ino 动画队列（case 0/1/2）、MotorController.hpp、Queue.hpp、display.ino（OLED 电量）
│   │   ├── wall-e_calibration/  # UNO 版舵机标定 sketch（交互式标定 9 舵机 LOW/HIGH PWM，输出 preset 数组）
│   │   ├── web_interface/       # 树莓派 Flask Web 控制端：app.py 单体入口（路由 + ArduinoDevice 串口线程）、
│   │   │                        #   config.py（全大写键）、gamepad.py、picamera2_stream.py（:8080 MJPEG）、
│   │   │                        #   walle.service、templates/ + static/（摇杆、Blockly、音效）
│   │   └── raspi-setup.sh       # 树莓派一键安装/自启脚本
│   └── wall-e_esp32/            # ESP32-S3 单机版（已冻结，作树莓派方案回退）
│       ├── wall-e_esp32/        # 主控固件（串口协议与 UNO 版一致）：内置 Web 控制端、PCM5102 I2S 音频、
│       │                        #   蓝牙手柄（Bluepad32）、GC9A01×2 眼睛 + ST7789 状态屏；data/ 为 LittleFS 镜像
│       └── wall-e_esp32_calibration/ # ESP32 版标定 sketch（小智固件标定也用这份）
├── wall-e_esp32_cam/            # 第二块 ESP32-S3-CAM 固件（主线配套摄像头外设）：MJPEG 推流（/stream）
│                                #   + microSD 拍照录像（/capture、/record、/files、/file，XIAO Sense 板载卡槽 SPI）
├── docs/                        # 中文技术文档
│   ├── SERIAL_PROTOCOL.md       # 串口通信协议（必读）
│   ├── WIRING.md                # Arduino 接线指南
│   ├── HARDWARE.md              # 硬件采购清单
│   ├── VOICE_LLM_PLAN.md        # 语音 + LLM 方案（树莓派侧备选，尚未实现）
│   ├── REID_FOLLOW_PLAN.md      # Re-ID 视觉跟随方案（尚未实现）
│   └── XIAOZHI_MIGRATION.md     # 小智语音迁移：构建、引脚、MCP 工具、Web 面板
├── hardware/                    # PCB 设计文件与第三方方案资料
│   └── 另一套硬件方案.md         # 第三方 ESP32-S3 方案 BOM（由同名 xlsx 转录）
├── models/                      # 空目录；设计文档提到的 TFLite 模型尚未提交
├── images/                      # 接线图、电路图
├── README.md / README.zh-CN.md
└── CLAUDE.md                    # 给 Claude Code 的既有项目指引
```

## 3. 技术栈

### 3.1 Arduino 端
- **语言**：C++（Arduino 框架）
- **开发环境**：Arduino IDE（推荐）
- **必需库**：`Adafruit_PWMServoDriver`（PCA9685 / LU9685 16 路 PWM 舵机板）
- **可选库**：`U8g2`（SH1106 OLED 电量显示）
- **控制器**：Arduino UNO R3（`archive/arduino-pi/wall-e/`，按 UNO 引脚映射编写）或 ESP32-S3（`archive/wall-e_esp32/wall-e_esp32/`，需 Arduino-ESP32 core 3.x，Tools → USB CDC On Boot → Enabled）

### 3.2 树莓派 Web 端
- **语言**：Python 3
- **Web 框架**：Flask
- **生产服务器**：waitress（`APP_DEBUG=False` 时 `waitress.serve`）
- **串口通信**：`pyserial`
- **视频流**：`picamera2` + `MJPEGEncoder`，独立 HTTP 服务跑在端口 8080
- **游戏手柄**：`pygame`
- **TTS 与音效**：`espeak-ng`、`rubberband-cli`、`aplay`、pygame（播放）
- **前端**：HTML + Bootstrap + jQuery + 自定义 VirtualJoystick + joypad.js + Blockly
- **依赖安装方式**：`raspi-setup.sh` 通过 `apt-get` 安装系统包，**没有** `requirements.txt`、`pyproject.toml` 或 `package.json`

## 4. 构建、运行与部署

### 4.1 Arduino 固件
1. 在 Arduino IDE 安装 `Adafruit_PWMServoDriver` 库。
2. 打开 `archive/arduino-pi/wall-e/wall-e.ino`，`animations.ino`、`MotorController.hpp`、`Queue.hpp` 会在同一窗口自动打开。
3. 根据 `archive/arduino-pi/wall-e_calibration/wall-e_calibration.ino` 标定得到 `preset[][2]` 数组，贴回 `wall-e.ino` 第 159–167 行。
4. 选择正确的 Board / Port，上传。
5. 串口监视器波特率设为 **115200**。

#### ESP32-S3 版本（`archive/wall-e_esp32/wall-e_esp32/`）
1. 安装 esp32 开发板包（Arduino-ESP32 core **3.x**）和库：`Adafruit_PWMServoDriver`、`ESPAsyncWebServer` + `AsyncTCP`（ESP32Async 组织维护，兼容 core 3.x）、`ESP32-audioI2S`（schreibfaul1，音频播放）、`Bluepad32`（蓝牙手柄）、`Arduino_GFX`（moononournation，GC9A01/ST7789 显示屏）。
2. 打开 `archive/wall-e_esp32/wall-e_esp32/wall-e_esp32.ino`；选择 ESP32-S3 开发板，设置 **Tools → USB CDC On Boot → Enabled**，**Tools → Partition Scheme → Custom**（使用 sketch 目录下的 `partitions.csv`）。
3. 引脚映射在文件顶部 `#define`（默认：TB6612 用 GPIO 4/5/6/7/15/16，STBY=17，I2C SDA=8/SCL=9，OE=10，电池 ADC=GPIO1）。
4. 用 `archive/wall-e_esp32/wall-e_esp32_calibration/` 标定后把 `preset` 贴回主 sketch；沿用同一块 60Hz 舵机板和机械结构时，UNO 版的 `preset` 值可直接复用。
5. 上传后串口协议与 UNO 版**完全一致**，`archive/arduino-pi/web_interface/` 无需任何改动，只需把 `local_config.py` 的串口指向 ESP32-S3 的端口。

#### ESP32-S3 内置 Web 控制端（可脱离树莓派）
`archive/wall-e_esp32/wall-e_esp32/` 自带与 Flask 版路由兼容的 HTTP 服务器（`web_server.cpp`），Wi-Fi 遥控不再依赖树莓派：
1. 编辑 `archive/wall-e_esp32/wall-e_esp32/web_config.h`：填 `WIFI_SSID`/`WIFI_PASSWORD` 走 STA 模式；留空则启动 AP 热点（默认 `WallE` / `walle1234`）。**不要把真实密码提交到 git**。
2. 上传固件后，用 Arduino IDE 的 **LittleFS Data Upload** 插件把 `archive/wall-e_esp32/wall-e_esp32/data/` 上传到 LittleFS 分区（首次或前端变更时执行）。
3. 浏览器访问 `http://<esp-ip>`（AP 模式为 `http://192.168.4.1`），登录密码同 `web_config.h`（默认 `walle`）。
4. 已移植路由：`/`、`/login`、`/login_request`、`/motor`、`/settings`（motorOff/steerOff/animeMode/volume/streamer/restart）、`/animate`、`/servoControl`、`/arduinoConnect`（恒 Connected）、`/arduinoStatus`（电量）、`/gamepadStatus`（真实蓝牙手柄状态）、`/audio`（PCM5102 I2S 播放 wav）。HTTP 与 USB 串口共用 `evaluateCommand()`，行为一致。
5. 尚未移植：`/tts`（云端 TTS，返回明确 Error）；BOM 扩展项（INMP441 麦克风、ASR-Pro 离线语音）。
6. 显示屏（默认启用，`web_config.h` 的 `DISPLAYS_ENABLED`）：两块 GC9A01 圆屏做眼睛（矢量绘制，表情跟随 `i/j/k/l` 指令，2.5–6s 随机眨眼），一块 ST7789 做状态屏（电量/Wi-Fi/手柄/自动模式，1s 刷新）。三屏共用 SPI（SCK=21、MOSI=18，RST=47、BL=48 共接），各自 CS/DC 见 `web_config.h`。若 ST7789 画面偏移，调 `status_display.cpp` 构造函数的 offset 参数；眨眼动画期间 loop 有 ~40ms 阻塞，舵机控制由 dt 补偿，属正常。
6. 蓝牙手柄：默认启用（`web_config.h` 的 `BT_GAMEPAD_ENABLED`），手柄进入配对模式即可连接；映射与 `gamepad.py` 一致（左摇杆行走、右摇杆头/颈、LT/RT 降臂、LB/RB 升臂、ABXY 眼部表情、Back 切自动模式、十字键随机音效/动画）。
7. 摄像头（第二块 ESP32-S3-CAM）：编辑 `wall-e_esp32_cam/camera_pins.h` 选板型、在 sketch 顶部填 Wi-Fi 凭据（留空则连主控 `WallE` AP），烧录后从串口拿到 IP，填入 `archive/wall-e_esp32/wall-e_esp32/data/index.html` 顶部的 `stream_url`（如 `http://192.168.4.3/stream`），重新上传 LittleFS 数据。小智主线面板则在 `walle_web_server.cc` 内嵌 HTML 的 `stream_url` 填同一地址。
   - **microSD 拍照/录像**（XIAO Sense 板载卡槽，SPI：CS=2、SCK=7、MISO=8、MOSI=9，插 FAT32 卡即可）：固件端点 `/capture`（拍照存 `/photos/`）、`/record?action=start|stop`（MJPEG→AVI 存 `/videos/`，帧率 `REC_FPS`）、`/files`（列表）、`/file?path=`（GET 下载 / DELETE 删除，GET 支持 HTTP Range 供主控分段取帧）。小智 Web 面板摄像头区按钮直连这些端点；语音工具 `self.walle.camera` 通过 `config.h` 的 `CAM_MODULE_URL` 调用。SD 挂载失败不影响推流。
   - **眼睛屏预览/回放**（`walle_cam_viewer.cc`）：`self.walle.camera` 的 `photo` 拍完自动预览，另有 `preview`（最新照片）/`replay`（最新 AVI 按帧回放）/`stop`；Web 面板 Preview/Replay/Stop view 按钮走主控 `POST /camview`；回放期间单击 BOOT 停止。照片显示 5s 后自动恢复眼睛（`SetPreviewImage` 预览定时器），回放结束/停止即恢复。
8. 前端 `data/index.html`/`login.html` 由 `archive/arduino-pi/web_interface/templates/` 去 Jinja 化生成（静态路径、`CODEBLOCK_*` 与音效列表为构建期烘焙值）；Pi 端模板改动后需重新生成（会覆盖 `stream_url` 行）。

### 4.2 树莓派 Web 服务

#### 首次安装
```bash
cd ~/walle-replica
sudo chmod +x ./archive/arduino-pi/raspi-setup.sh
sudo ./archive/arduino-pi/raspi-setup.sh
```
该脚本会：
- 安装 `espeak-ng rubberband-cli python3-pygame python3-serial python3-flask python3-picamera2 python3-waitress`
- 把当前用户加入 `input` 组（游戏手柄权限，需重启生效）
- 将 `archive/arduino-pi/web_interface/walle.service` 复制到 `/etc/systemd/system/`，并替换模板中的 `username` 与 `/path-to-directory`
- 启用并启动 `walle.service`

#### 手动运行（调试用）
```bash
cd ~/walle-replica/web_interface
python3 app.py            # 默认监听 0.0.0.0:5000
```

#### systemd 服务管理
```bash
sudo systemctl status|start|stop|enable|disable walle.service
```

#### 查看可用串口
```bash
dmesg | grep tty
```

## 5. 核心运行架构

- **`app.py`** 是 Flask 单体入口：
  - 启动时优先加载 `local_config.py`（如果存在），否则加载 `config.py`。
  - 全局 `arduino = ArduinoDevice()` 创建串口线程，通过发送队列向后端写指令。
  - 服务启动时（`__main__` 里的 `autostart_systems()`）根据 `AUTOSTART_ARDUINO` / `AUTOSTART_CAM` 自动连接 Arduino / 启动摄像头，无需先打开网页；Arduino 连接失败会重试 5 次（间隔 2s）。
- **串口线程**：`ArduinoDevice.__communication_thread()` 持续读取串口回显/电量，并逐条写入队列中的指令。
- **摄像头流**：`PiCameraStreamer` 在**独立进程外 HTTP 服务（端口 8080）** 提供 `/stream.mjpg`；前端 `<img>` 直接引用 `http://<host>:8080/stream.mjpg`。
- **游戏手柄**：
  - 浏览器端：插在访问网页的设备上的手柄走 `static/js/main.js` 的 `joypad`。
  - 树莓派端：插在树莓派上的手柄走 `gamepad.py` 后台线程。
  - 两者都会调用 `arduino.send_command(...)`，后发的覆盖先发的。
- **登录**：简单 session cookie，密码在 `config.py` 的 `LOGIN_PASSWORD`，默认 `walle`。

## 6. 串口通信协议（核心契约）

**物理链路**：USB 串口，波特率 **115200**，帧以 `\n` 或 `\r` 结尾。

**帧格式**：`<1 字符前缀><数字>`，例如 `X100`、`A2`、`O40`。

**关键限制**：Arduino 端缓冲区上限 `MAX_SERIAL_LENGTH = 5` 字符，因此前缀 1 字符 + 数字最多 4 字符。负号也算 1 字符（`X-100` 正好 5 字符）。超长指令会被截断。

### 6.1 Web → Arduino（带数字参数）

| 前缀 | 范围 | 含义 | 路由 |
|------|------|------|------|
| `X` | -100..100 | 左右转向（×2.55 → PWM） | `/motor` |
| `Y` | -100..100 | 前进/后退 | `/motor` |
| `S` | -100..100 | 转向偏置 / 微调 | `/settings` |
| `O` | 0..250 | 电机死区补偿 | `/settings` |
| `M` | 0/1 | 舵机自动模式 关/开 | `/settings` |
| `A` | n | 播放动画 n | `/animate` |
| `G/T/B` | 0..100 | 头部旋转 / 颈上 / 颈下 | `/servoControl` |
| `E/U` | 0..100 | 左眼 / 右眼 | `/servoControl` |
| `L/R` | 0..100 | 左臂 / 右臂 | `/servoControl` |
| `I/J` | 0..100 | 左眉毛 / 右眉毛 | `/servoControl` |
| `V` | 0..100 | 照明灯亮度（0=关灯；**仅小智固件**，PCA9685 空闲通道 `LIGHT_PWM_CHANNEL`） | `/servoControl` |

### 6.2 调试单字符指令（仅 Arduino 串口监视器）

`w a s d q` 行走控制，`i j k l` 眼部表情，`f g h` 头部姿态，`b n m` 手臂姿态。详见 `docs/SERIAL_PROTOCOL.md`。注意单字符 `m`（手臂动作）与大写 `M`+数字（自动模式开关）是两条独立指令。

### 6.3 Arduino → Web（反向）

仅在 `wall-e.ino` 中启用 `#define BAT_L` 时，Arduino 每 10 秒发送一次 `Battery_<百分比>`。`app.py` 的 `__parse_message()` 解析后供 `/arduinoStatus` 读取。

### 6.4 舵机位置映射

`0..100` 是归一化位置，不是 PWM。Arduino 按 `preset[][2]`（`wall-e.ino:159`）线性映射：

```cpp
setpos[i] = int(number * 0.01 * (preset[i][1] - preset[i][0]) + preset[i][0]);
```

下标含义：`0=head`、`1=necT`、`2=necB`、`3=eyeR`、`4=eyeL`、`5=armL`、`6=armR`、`7=broL`、`8=broR`。

**ESP32-S3 版**（`archive/wall-e_esp32/wall-e_esp32/`）在上述逻辑关节顺序与 PWM 板物理通道之间加了一层 `servoChannel[]` 映射（默认适配第三方线束：通道 `0=eyeL`、`1=eyeR`、`2=head`、`3=necT`、`4=necB`、`5=armL`、`6=armR`、`7=broL`、`8=broR`）。`preset`、`setpos`、动画、串口指令仍全部使用逻辑关节顺序；接线或线束不同时只需改 `servoChannel[]` 一个数组（标定 sketch 内有同一张表）。

### 6.5 新增指令的约定

**新增电机/舵机指令必须三处同步**：
1. `app.py` 增加 Flask 路由，调用 `arduino.send_command("前缀" + str(值))`。
2. `archive/arduino-pi/wall-e/wall-e.ino` 的 `evaluateSerial()` 增加 `else if (firstChar == '?')` 分支。
3. `archive/arduino-pi/web_interface/static/js/main.js` 增加前端交互（按钮/摇杆/滑杆）。

避开已占用前缀（见上表），否则硬件不会响应或行为错乱。完整协议细节见 `docs/SERIAL_PROTOCOL.md`。

小智主线固件的对应落点是 `walle_motion.cc` 的 `EvaluateCommand()`——串口任务和小智 Web 面板都是通用前缀转发，新指令只需在 `EvaluateCommand()` 加分支 + MCP 工具 + 内嵌 HTML 交互。照明灯（前缀 `V`）即按此模式实现：PCA9685 空闲通道 9 驱动，见 `config.h` 的 `LIGHT_PWM_CHANNEL`。

## 7. 关键配置文件

- **`archive/arduino-pi/web_interface/config.py`**：默认配置。键名全大写，覆盖 Web 端口、登录密码、默认串口、是否自动连接 Arduino/摄像头、TTS 命令、音效文件夹、CodeBlocks 电机参数、游戏手柄参数等。
- **`archive/arduino-pi/web_interface/local_config.py`**：可选本地覆盖文件，**已加入 `.gitignore`**。本地密码、API Key、端口等敏感或环境相关改动应写在这里，不要直接修改 `config.py`。
- **`archive/arduino-pi/web_interface/walle.service`**：systemd 服务模板，含占位符 `username` 与 `/path-to-directory`，由 `raspi-setup.sh` 替换。
- **`raspi-setup.sh`**：树莓派首次安装与自启脚本。

## 8. 代码风格与开发约定

- **Python 配置**：`config.py` 使用全大写键名，`app.py` 通过 `app.config['KEY']` 读取。新增可调参数请沿用此风格并加注释。
- **Python 代码**：使用类型注解（如 `Serial | None`、`Queue`、`dict` 等），与现有模块保持一致。
- **Arduino 代码**：引脚定义、常量使用 `#define`；舵机标定数组 `preset[][2]` 是行为正确性的关键。
- **注释语言**：代码注释以英文为主；Python 模块级 docstring 与中文文档混用。
- **敏感信息**：密码、API Key 等写在 `local_config.py`，不要提交到 Git。
- **无依赖管理文件**：不要凭空创建 `requirements.txt` 或 `package.json`；新增 Python 依赖应同步更新 `raspi-setup.sh` 并说明。
- **无 linter/格式化器**：仓库未配置；保持与周围代码风格一致即可。

## 9. 测试策略

仓库**没有自动化测试套件**。所有验证依赖手动冒烟测试：

1. **Arduino 串口监视器**：
   - 发送 `w/a/s/d/q` 看电机转向/停止。
   - 发送 `j/l/i/k` 看头部/眼睛动作。
   - 若舵机行程不对，先用 `wall-e_calibration.ino` 标定再贴回 `preset`。
2. **Web 界面**：
   - 登录后连接 Arduino，测试虚拟摇杆 `/motor`。
   - 手动舵机滑杆 `/servoControl`。
   - 动画按钮 `/animate`。
   - 播放 `/audio` 与 `/tts`。
   - 摄像头开关 `/settings`（streamer）。
3. **游戏手柄**：
   - 浏览器端手柄插在访问设备上测试 `joypad`。
   - 树莓派端手柄插在 Pi 上测试 `gamepad.py`，查看 `/gamepadStatus` 轮询。
4. **电池检测**：启用 `#define BAT_L` 后，每 10 秒应收到 `Battery_87` 类消息，`/arduinoStatus` 正确显示。
5. **服务日志**：
   ```bash
   sudo journalctl -u walle.service -f
   ```

## 10. 安全注意事项

- Web 登录只是简单的 session cookie + 明文密码比较，**不是完整的访问控制系统**。
- 默认密码 `walle`、默认串口 `/dev/ttyACM0`、Flask secret key 都是硬编码默认值，正式使用前应在 `local_config.py` 中修改。
- 不要在不可信的公共网络上直接暴露 `0.0.0.0:5000`。
- API Key、LLM/Voice 相关凭证按设计应写入 `local_config.py`，不要写入 `config.py` 或提交到版本库。
- `app.py` 的 `/settings` 路由包含 `restart` 和 `shutdown` 动作，调用 systemd / `shutdown -h now`，需确保运行用户有对应 sudo 权限（`raspi-setup.sh` 已按 root 执行）。

## 11. 路线图中尚未实现的模块（重要）

> 语音交互主线已由小智固件（`wall-e_xiaozhi/`）实现（唤醒词 + 云端 LLM + MCP 动作工具）；本节列出的是**树莓派侧**语音/视觉管线，仍是计划中的备选/远期功能。

`docs/VOICE_LLM_PLAN.md` 与 `docs/REID_FOLLOW_PLAN.md` 描述的是**计划中的功能**，对应源码文件目前**不存在**：

- `archive/arduino-pi/web_interface/vision_tracker.py`
- `archive/arduino-pi/web_interface/robot_brain.py`
- `archive/arduino-pi/web_interface/follow_controller.py`
- `archive/arduino-pi/web_interface/voice_agent.py`
- `archive/arduino-pi/web_interface/voice_llm.py`
- `archive/arduino-pi/web_interface/robot_tools.py`
- `archive/arduino-pi/web_interface/reid_tracker.py`
- `archive/arduino-pi/web_interface/gait_features.py`
- `models/face_detector.tflite`
- `models/mobilefacenet.tflite`

`models/` 目录当前为空。若你接到实现视觉跟随 / 语音对话的任务，应以这两份设计文档为准，并按文档要求更新 `config.py`、`app.py`、`raspi-setup.sh` 及前端。

## 12. 快速参考

| 动作 | 命令 |
|------|------|
| 手动启动 Web 服务 | `python3 archive/arduino-pi/web_interface/app.py` |
| 首次安装/自启 | `sudo ./archive/arduino-pi/raspi-setup.sh` |
| 查看服务状态 | `sudo systemctl status walle.service` |
| 查看串口 | `dmesg \| grep tty` |
| 编辑配置（推荐） | `nano archive/arduino-pi/web_interface/local_config.py` |
| Arduino 串口波特率 | 115200 |
| 视频流地址 | `http://<pi-ip>:8080/stream.mjpg` |
| Web 界面地址 | `http://<pi-ip>:5000` |
| 小智固件构建 | `cd wall-e_xiaozhi && idf.py build` |
| 小智固件烧录+日志 | `idf.py -p COMx flash monitor` |
| 小智 Web 控制面板 | `http://<esp-ip>`（80 端口，无鉴权） |

## 13. 参考文档

- `README.md` / `README.zh-CN.md`：完整搭建与使用说明
- `docs/SERIAL_PROTOCOL.md`：串口协议细节
- `docs/WIRING.md`：Arduino 与舵机板/电机板接线
- `docs/HARDWARE.md`：硬件采购清单
- `docs/XIAOZHI_MIGRATION.md`：小智语音迁移（构建、引脚、MCP 工具、Web 面板、服务端切换）
- `docs/VOICE_LLM_PLAN.md`：语音 + LLM 接入方案（树莓派侧备选）
- `docs/REID_FOLLOW_PLAN.md`：Re-ID 目标跟随方案
