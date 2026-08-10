# Wall-E Robot Replica — AI Agent Guide

> 本文件面向不了解本项目的 AI 编程助手。此前项目根目录下**不存在** `AGENTS.md`，本文档根据仓库实际内容（源码、`docs/`、`CLAUDE.md`、`README.md`、`README.zh-CN.md` 等）整理而来。

## 1. 项目概述

这是一个可动 Wall-E 机器人复刻版的完整控制仓库。当前**主线是小智单 MCU 语音方案**（`wall-e_xiaozhi/`），两套旧方案已移入 `archive/` 归档（仍可编译回退，见 `archive/README.md`）：

1. **Arduino 固件**（`archive/arduino-pi/wall-e/`）：直接驱动电机、舵机，通过 USB 串口接收指令。
2. **Raspberry Pi Web 服务器**（`archive/arduino-pi/web_interface/`）：基于 Flask 的 Web 控制界面，通过串口向 Arduino 下发指令；可选接入 CSI 摄像头做 MJPEG 视频推流；支持浏览器虚拟摇杆、游戏手柄、TTS、Blockly 拖拽编程。

**当前存在两套主控固件格局**（2026-08 起）：

| 固件 | 框架 | 定位 | 状态 |
|------|------|------|------|
| `archive/arduino-pi/wall-e/` | Arduino (UNO) | 原始方案：UNO + 树莓派 | 维护中 |
| `wall-e_xiaozhi/main/boards/walle/` | ESP-IDF | **主线**：小智语音交互单 MCU 方案（唤醒词 + 云端 LLM + MCP 动作 + 4G 回退；眼睛屏在 CAM 模组上，主控侧禁用） | **开发中** |

> 原 ESP32-S3 单机版（`archive/wall-e_esp32/`）已于 2026-08 移除；其舵机标定 sketch 保留并移至仓库主目录 `wall-e_esp32_calibration/`（小智主线标定用）。

硬件接线、舵机标定、电池检测等说明见 `README.md` / `README.zh-CN.md`，详细接线与串口协议见 `docs/`，小智迁移细节见 `docs/XIAOZHI_MIGRATION.md`。

## 2. 仓库结构与代码组织

```
walle-replica/
├── wall-e_xiaozhi/               # ★ 主线：vendored 小智语音固件（ESP-IDF，上游 78/xiaozhi-esp32 @ e0074e9，MIT）
│   ├── main/boards/walle/       # Wall-E 板型（单 MCU 语音方案主战场）
│   │   ├── config.h             # 全部引脚定义（音频/显示/电机/舵机/电池/状态屏/Web/4G 模组）
│   │   ├── walle_board.cc       # 板类：GC9A01 显示（禁用中：第三方套件眼屏接在 CAM 模组上）+ NoAudioCodecSimplex + 按键 + 运动核心/状态屏/Web 启动
│   │   │                        #   + DualNetworkBoard：Wi-Fi 优先、连接超时自动回退 ML307A 4G（双击 BOOT 手动切网）
│   │   ├── walle_motion.cc/.h   # 运动核心：舵机动力学、电机斜坡、动画队列、电量（移植自 Arduino 版）
│   │   ├── walle_mcp_tools.cc   # MCP 工具注册（云端 LLM function calling → 动作/照明灯/摄像头）
│   │   ├── walle_serial.cc      # USB 串口协议任务（默认禁用：GPIO19/20 原生 USB 被 PWMA/舵机 I2C 占用，日志走 UART0）
│   │   ├── walle_web_server.cc  # Web 控制面板（esp_http_server :80，API 兼容 Flask 版，含摄像头画面区与预览/回放按钮）
│   │   ├── walle_cam_link.cc/.h # 主控↔CAM UART 链路层（CAM_PROTOCOL v1，docs/CAM_PROTOCOL.md）：UART1 命令/事件 +
│   │   │                        #   互斥 Command() + 超时判 down + HTTP 回退；MCP/Web 摄像头调用点统一走这里
│   │   ├── walle_cam_viewer.cc/.h # **已废弃**（HTTP 拉帧预览/AVI 回放；眼屏在 CAM 模组上，主控 NoDisplay 无出口；
│   │   │                        #   预览/回放已由 CAM 本地回放 + CamLink SHOW/ABORT 接管，仅保留代码备查）
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
├── wall-e_esp32_calibration/    # ESP32-S3 版舵机标定 sketch（小智主线标定用；原 archive/wall-e_esp32/ 冻结固件已移除）
├── wall-e_esp32_cam/            # 第二块 ESP32-S3-CAM 固件（主线配套摄像头外设）：MJPEG 推流（/stream）
│                                #   + microSD 拍照录像（/capture、/record、/files、/file；板型预设与 SD 引脚当前按
│                                #     XIAO Sense 写，实际模块为通用 ESP32-S3-CAM（OV3660），待实机核对，见 docs/NEW_HARDWARE_MIGRATION.md Step 4.5）
│                                #   + 眼睛屏驱动 eye_display.h（2× 圆屏共享 SPI：SCK=42/MOSI=45/DC=41/RST=46，CS 左=2/右=0，
│                                #     /eyes 端点；接线已确认，GC9A01 驱动假设待实机验证）
│                                #   + cam_link.h（CAM_PROTOCOL v1 CAM 侧：Serial1 RX=14/TX=21 命令解析 + 拍照倒计时/眼屏
│                                #     JPEG 回放状态机，依赖 JPEGDEC 库；引脚待 Step 4.5 核对）
├── docs/                        # 中文技术文档
│   ├── SERIAL_PROTOCOL.md       # 串口通信协议（必读）
│   ├── WIRING.md                # Arduino 接线指南
│   ├── HARDWARE.md              # 硬件采购清单
│   ├── XIAOZHI_MIGRATION.md     # 小智语音迁移：构建、引脚、MCP 工具、Web 面板
│   ├── NEW_HARDWARE_MIGRATION.md # 新硬件方案迁移进度与待办（Step 4.5 CAM 板型核对阻塞项）
│   ├── CAM_PROTOCOL.md          # 主控↔CAM UART 交互协议（规范已定稿；两侧均已实现，待实机联调）
│   ├── VOICE_LLM_PLAN.md        # 语音 + LLM 方案（树莓派侧备选，尚未实现）
│   └── REID_FOLLOW_PLAN.md      # Re-ID 视觉跟随方案（尚未实现）
├── hardware/                    # 新硬件 PCB 设计输入 + 第三方方案资料
│   ├── 另一套硬件方案.md         # 第三方 ESP32-S3 套件 BOM 转录与引脚表
│   ├── 新硬件BOM与尺寸.md        # 新硬件 BOM + 模块实测尺寸（方案 A 设计输入）
│   ├── 主板设计说明.md           # 方案 A 双层堆叠主板 + 摄像头背板 + 副板：连接器逐脚定义/电源树/布局
│   ├── 主板布局示意图.html        # 等比例布局示意图（主板顶/底两面 + 摄像头背板 + 副板）
│   ├── 另一套硬件-{主板,副板,摄像头背板}.png  # 第三方套件参考实物照片
│   └── walle-shield/             # 旧 Arduino UNO 模块化底板（KiCad 10，140×100）
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
- **控制器**：Arduino UNO R3（`archive/arduino-pi/wall-e/`，按 UNO 引脚映射编写）。（原 ESP32-S3 单机版已移除；ESP32-S3 主线走小智 ESP-IDF 固件，见 `wall-e_xiaozhi/`。）

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

#### ESP32-S3 单机版（已移除）

原 `archive/wall-e_esp32/wall-e_esp32/`（内置 Web 控制端、PCM5102 I2S 音频、蓝牙手柄、GC9A01×2 眼睛 + ST7789 状态屏，可脱离树莓派）已于 2026-08 移除——自小智主线落地后即冻结，无回退价值。其舵机标定 sketch 保留并移至仓库主目录 `wall-e_esp32_calibration/`，小智主线标定仍用这份（烧一次标定 sketch 得到 `preset` 数组，贴到 `wall-e_xiaozhi/main/boards/walle/walle_motion.cc` 顶部；沿用同一块 60Hz 舵机板和机械结构时，UNO 版 `preset` 可直接复用）。

#### ESP32-S3-CAM 推流固件（`wall-e_esp32_cam/`，主线配套）

第二块 ESP32-S3-CAM：MJPEG 推流（`/stream`）+ microSD 拍照录像（`/capture`、`/record?action=start|stop`、`/files`、`/file?path=`）+ 眼睛屏驱动（`eye_display.h`，2 块 1.28 寸圆屏共享 HSPI，`/eyes?expr=neutral|sad|left|right`）。编辑 `wall-e_esp32_cam/camera_pins.h` 选板型、在 sketch 顶部填 Wi-Fi 凭据，安装 `Arduino_GFX` 与 `JPEGDEC` 库，烧录后从串口拿到 IP，填入主控 Web 面板的 `stream_url`（`wall-e_xiaozhi/main/boards/walle/walle_web_server.cc` 内嵌 HTML）。板型预设与 SD 引脚当前按 XIAO Sense 写，实际模块为通用 ESP32-S3-CAM（OV3660），待实机核对（见 `docs/NEW_HARDWARE_MIGRATION.md`）。主控↔CAM 经 UART 交互（CAM_PROTOCOL v1，见 `docs/CAM_PROTOCOL.md`）；MCP 工具 `self.walle.camera` 的 photo/preview/record/stop 走 `WalleCamLink`（UART 优先，链路 down 回退 `CAM_MODULE_URL` HTTP 端点）。

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

**小智主线 / ESP32-S3 标定 sketch**（`wall-e_esp32_calibration/`）在逻辑关节顺序与 PWM 板物理通道之间加了一层 `servoChannel[]` 映射（默认适配第三方线束：通道 `0=eyeL`、`1=eyeR`、`2=head`、`3=necT`、`4=necB`、`5=armL`、`6=armR`、`7=broL`、`8=broR`）。`preset`、`setpos`、动画、串口指令仍全部使用逻辑关节顺序；接线或线束不同时只需改 `servoChannel[]` 一个数组（标定 sketch 与 `walle_motion.cc` 内有同一张表）。

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
- `docs/CAM_PROTOCOL.md`：主控↔ESP32-S3-CAM UART 交互协议（命令/拍照流程/表情；规范已定稿，主控侧 `walle_cam_link` 与 CAM 侧 `cam_link.h` 均已实现，待实机联调）
- `docs/VOICE_LLM_PLAN.md`：语音 + LLM 接入方案（树莓派侧备选）
- `docs/REID_FOLLOW_PLAN.md`：Re-ID 目标跟随方案
