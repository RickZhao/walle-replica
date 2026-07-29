# Wall-E 复刻版 硬件采购清单

本清单按子系统分组，标注 **必需 / 可选 / 新功能所需**。基础搭建照第 1–3 节即可；要把 `archive/arduino-pi/web_interface/` 下那套半成品的视觉跟随 / 语音交互（`vision_tracker.py`、`robot_brain.py`、`follow_controller.py`、`voice_agent.py`）跑起来，再加第 4–5 节。

> 代码依据：`archive/arduino-pi/wall-e/wall-e.ino`（引脚映射、`NUMBER_OF_SERVOS = 7`、`BATTERY_MAX_VOLTAGE = 12.6`）、`archive/arduino-pi/web_interface/config.py`（`VISION_*` / `FOLLOW_*` / `VOICE_*` 段）、`README.md`（CSI 摄像头、3S 电池、分压电阻）。

---

## 1. 主控与计算

| 部件 | 推荐型号 | 等级 | 说明 |
|------|----------|------|------|
| 树莓派 | **Raspberry Pi 4B（4GB）** 或 **Pi 5（4GB）** | 必需 | 跑 Flask + 视觉 + 语音。Pi 4 跑 MediaPipe 人脸检测 15fps 勉强够，Pi 5 更从容。Pi Zero 2 W 跑持续视觉太吃力，不推荐 |
| Arduino | **Arduino UNO R3** | 必需 | 代码用 UNO 引脚映射（PWM 3/11、方向 12/13，TB6612FNG 时 D8/D9 作为第二路方向脚），README 亦提到 UNO 内存警告 |
| 通信线 | USB-A ↔ USB-B（UNO 方口线） | 必需 | Arduino ↔ Pi 数据通信 |
| **模块化底板 PCB** | `hardware/walle-shield/` 生成的 140×100 mm 2 层板 | **强烈推荐** | 替代杜邦线；底板只焊直插器件，Arduino/电机板/舵机板/降压模块直接插。见 `hardware/walle-shield/README.md` |

## 2. 驱动与执行

| 部件 | 推荐型号 | 等级 | 说明 |
|------|----------|------|------|
| 舵机驱动板 | **Adafruit PCA9685** 16 路 PWM | 必需 | 代码用 `Adafruit_PWMServoDriver`，I2C 地址 0x40 |
| 电机驱动 | **Arduino Motor Shield Rev2**（原设计），或 **TB6612FNG**（更省电），或 **L298N** | 必需 | 默认支持 Arduino Motor Shield Rev2（DIR + PWM + BRAKE）。TB6612FNG 需在 `wall-e.ino` 启用 `MOTOR_DRIVER_TB6612FNG`，用 D8/D9 作第二路方向脚，无需 74HC04。L298N 需单 DIR 反相接 IN2/IN4 |
| 行走电机 | 12V 减速直流电机 ×2（约 200 RPM） | 必需 | 驱动左右履带 |
| 头部/颈部舵机 | **MG90S 金属齿** ×3（头转、颈上、颈下） | 必需 | 承重大，用金属齿 |
| 眼睛舵机 | **SG90** ×2 | 必需 | 轻载，微型即可 |
| 手臂舵机 | **MG90S** ×2 | 必需 | — |

> 共 7 个舵机，与 `NUMBER_OF_SERVOS = 7` 对应。

## 3. 电源

| 部件 | 推荐型号 | 等级 | 说明 |
|------|----------|------|------|
| 电池 | **3S LiPo 11.1V**（满电 12.6V，≥2200mAh） | 必需 | `BATTERY_MAX_VOLTAGE = 12.6` 即 3S |
| 降压模块 | 12V→5V **DC-DC Buck，≥5A** | 必需 | 给 Pi 供电（Pi 4 要 3A，Pi 5 要 5A） |
| 分压电阻 | R1 = 100kΩ、R2 = 47kΩ | 可选 | 电池检测。`DIVIDER_SCALING_FACTOR = 0.3197` 即此值 |
| 电源开关 | 自锁船型开关（KCD1 等） | 必需 | 总电源通断；底板预留 `SW1` 焊盘 |
| 保险丝座 | 5×20 mm 直插保险丝座 | 推荐 | 底板 `F1`，建议配 10 A 保险丝 |
| 接线端子 | 5.08 mm 间距 2P 直插端子 | 必需（用底板时） | 底板 `J6/J4/J5/J17/J7/J8` 等 |

## 4. 视觉跟随（新功能，正在集成）

| 部件 | 推荐型号 | 等级 | 说明 |
|------|----------|------|------|
| 摄像头 | **Raspberry Pi Camera Module 3**（CSI 排线） | 必需 | ⚠️ README 明确不支持 USB 摄像头（`picamera2_stream.py` 走 CSI）。Module 3 带自动对焦、低光更好，利于人脸检测 |
| 测距传感器 | **VL53L1X** ToF（I2C），或 HC-SR04 超声波 | 可选但强推 | `FOLLOW_SAFETY_DISTANCE = inf` 注释“无测距传感器时不启用”——要真正安全跟随必须加装。VL53L1X 量程 4m、体积小、I2C 直连 |

## 5. 语音交互（新功能，正在集成）

> **录放音栈**：`voice_agent.py` 录音用 `speech_recognition` 的 `sr.Microphone(sample_rate=16000)`（PyAudio/ALSA），播放用 `edge-tts` 生成 mp3 → `pygame.mixer`（ALSA default output）。任何 ALSA 能识别的设备都能直接用，**无需改代码**。
>
> **Pi 4B 硬约束**：没有模拟麦克风输入（无 ADC），麦克风只能选 **USB 或 I2S**，不能直接插 3.5mm 模拟麦；扬声器则 3.5mm / I2S / USB 均可。

### 麦克风（Pi 4B 适用）

| 方案 | 具体型号 | 价位 | 接入 | 优缺 |
|------|----------|------|------|------|
| **USB 麦克风**（最省事） | mini USB 麦克风 / Kinobo Makyo | ¥15–40 | 插 USB，ALSA 自动认 | 零配置，`arecord -l` 直接出设备；远场一般，单麦无降噪 |
| USB 麦阵列（远场+降噪） | **ReSpeaker USB 4-Mic Array**（Seeed） | ~¥200 | 插 USB | DSP 降噪/AEC，室外抗噪好；贵、体积大 |
| I2S 麦克风（省 USB） | **INMP441** / **ICS-43434** 模块 | ¥10–15 | 接 GPIO I2S（BCM 18/19/21），需 `dtoverlay` 配置 | 便宜小体积、不占 USB；要改 `/boot/config.txt` |
| **GPIO 麦+喇叭一体 HAT** | **ReSpeaker 2-Mics Pi HAT**（Seeed） | ~¥180 | 直接插 GPIO | 双麦阵列 + 板载 3.5mm 输出 + 降噪，一举两得；贵 |

### 扬声器（Pi 4B 适用）

| 方案 | 具体型号 | 价位 | 接入 | 优缺 |
|------|----------|------|------|------|
| **3.5mm 有源小音箱**（最省事） | 便携迷你音箱（自带功放/电池） | ¥20–40 | Pi 3.5mm，`amixer cset numid=3 1` 强制耳机口（README 已有此命令） | 零配置，`aplay`/pygame 直出；体积稍大、要充电 |
| **I2S 功放 + 喇叭**（最省空间） | **MAX98357A** + 3W/4Ω 喇叭 | ¥10–15 | 接 GPIO I2S，需 `dtoverlay=hifiberry-dac` | 数字直出音质好、嵌入机体最干净；要配置 |
| USB 声卡 + 有源喇叭 | CM108 USB 声卡 + 小喇叭 | ¥15 | USB | 灵活但线多 |

### 三套组合推荐

- **方案 A — 最省事（室内，零配置）**：mini USB 麦克风（~¥15）+ 3.5mm 便携小音箱（~¥30）。插上即用，README 的 3.5mm 配置命令已就绪；缺点是占 1 个 USB + 3.5mm，音箱要单独充电。
- **方案 B — 最省空间嵌入式（推荐放机体里）**：mini USB 麦克风（~¥15）+ MAX98357A + 3W 喇叭（~¥15）。喇叭干净嵌入机体、不占 3.5mm、不用单独供电；缺点是麦远场一般。
- **方案 C — 室外抗噪（贵但值）**：**ReSpeaker 2-Mics Pi HAT**（~¥180）+ 3.5mm 接 MAX98357A + 喇叭。双麦阵列 + AEC 降噪，室外风噪/远场明显优于单 USB 麦；缺点是贵、要装驱动。

### ALSA 接入注意

1. 多声卡时（如同时插 USB 麦 + I2S 功放），`sr.Microphone()` 和 `pygame` 默认走 ALSA default device，需在 `~/.asoundrc` 把默认输入指向麦、默认输出指向喇叭。用 `arecord -l` / `aplay -l` 查设备名。
2. `voice_agent.py` 里 `sr.Microphone(sample_rate=16000)` 已固定 16kHz，USB 麦都支持；I2S 麦（INMP441）也支持但要确认 dtoverlay 采样率。
3. pygame 播放走 ALSA，I2S 功放配好后会自动成为 default，无需改 `voice_agent.py`。

> Whisper ASR 在 Pi 上较重，建议默认 `VOICE_ASR_PROVIDER = "google"`（在线、轻量）；要离线再上 whisper-tiny。

## 6. 可选显示

| 部件 | 推荐型号 | 等级 | 说明 |
|------|----------|------|------|
| OLED | **SH1106 1.3" 128×64 I2C** | 可选 | `#define OLED` 默认构造就是它，显示电量 |

## 7. 底板专属物料（如果用 `hardware/walle-shield/`）

| 部件 | 推荐型号 | 数量 | 说明 |
|------|----------|------|------|
| PCB | 140×100 mm，2 层，1.6 mm | 1 | Gerber 在 `hardware/walle-shield/gerber/` |
| Arduino 排母 | 1×10 + 1×8 直插排母 | 各 1 | 固定 Arduino UNO R3 |
| 模块排母 | 2.54 mm 直插排母 | 若干 | `J2/J2M/J3/J3PWM/J16` 及 7 路舵机接口 |
| 电源指示灯 | 3 mm LED（红/绿）+ 1 kΩ 限流电阻 | 各 1 | 底板 `D1`/`R3` |
| 安装铜柱 | M3×10 mm 铜柱 + M3 螺丝 | 4 套 | 固定底板 |

## 8. 算力升级（仅当视觉/语音吃力时）

- **首选**：直接上 **Pi 5**，比加加速棒省心。
- **Google Coral USB Accelerator（Edge TPU）**：⚠️ 仓库自带的 `models/face_detector.tflite` **不是 Edge-TPU 编译版**，直接插 Coral 不会加速。要用 Coral 须自行用 `edgetpu_compiler` 重新编译模型，否则白买。除非愿意折腾编译，否则不建议。

## 9. 结构

- 3D 打印件来自 [wired.chillibasket.com/3d-printed-wall-e](https://wired.chillibasket.com/3d-printed-archive/arduino-pi/wall-e/)（原作者图纸，与舵机位、电机位完全匹配，建议直接用）。

---

## 一句话采购建议

基础版照第 1–3 节；想省掉杜邦线再加第 7 节底板物料；要把仓库里那套半成品的视觉/语音跑起来，再加 **Pi Camera Module 3 + USB 麦克风 + 小扬声器 + VL53L1X**，主控选 Pi 4B/5 即可，暂时不用上 Coral。

## 接线与代码接入提示

- **测距传感器**：VL53L1X 接 I2C（与 PCA9685 共线，地址不冲突），读距后写入 `FOLLOW_SAFETY_DISTANCE`，`FollowController` / `RobotBrain` 的 `AVOID` 状态才能生效。
- **麦克风/扬声器**：`config.py` 的 `VOICE_*` 段已配好；依赖（`speech_recognition`、`edge-tts`、`pygame`、`opencv-python`、`mediapipe`）未在 `raspi-setup.sh` 安装，需另行 `pip install`。
- **舵机/电机改动**：若新增指令，必须同步 `app.py` 路由与 `wall-e.ino` 的 `evaluateSerial()`（见 `CLAUDE.md` 协议表）。
