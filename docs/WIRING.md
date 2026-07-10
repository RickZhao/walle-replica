# Wall-E 接线指南

Arduino UNO R3 与舵机控制板、电机控制板的接线。**所有引脚以 `wall-e/wall-e.ino` 的定义为唯一权威**，本文件逐条对照代码行号，便于核对。

实物接线图见 `images/wall-e_wiring_diagram.jpg`；电池检测电路见 `images/battery_level_circuit.jpg`；OLED 电路见 `images/oLed_circuit.jpg`。

## 总体架构

```
                    ┌─────────────────┐
                    │  Arduino UNO R3 │
   USB<────Pi──────│ Serial (115200) │
                    │                 │
   I2C (A4/A5) ─────│ A4 SDA  A5 SCL │────────┐
   电机控制三脚 ────│ D12 D13 D3 D11 │───┐    │
                    │ D9 D8 D10      │   │    │
                    └─────────────────┘   │    │
                                          ▼    ▼
                                ┌──────────────┐  ┌────────────────┐
                                │ 电机驱动板    │  │ PCA9685 舵机板  │
                                │ (Shield Rev2/│  │ (Adafruit 16ch)│
                                │  L298N/      │  │ I2C addr 0x40 │
                                │  TB6612)     │  │ OE <- D10      │
                                └──────┬───────┘  └────────┬───────┘
                                       ▼                   ▼ PWM 0-6
                                左/右行走电机 ×2      7 个舵机(头/颈/眼/臂)
```

## 1. 舵机控制板：Adafruit PCA9685（16 路 PWM）

走 **I2C**，与 Arduino 共两根线 + 电源。代码 `Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();` 默认地址 **0x40**（`wall-e.ino:98`），工作频率 60Hz（`wall-e.ino:178`）。

| PCA9685 引脚 | 接 Arduino | 说明 |
|-------------|-----------|------|
| `SDA` | **A4** | I2C 数据（UNO 硬件 I2C） |
| `SCL` | **A5** | I2C 时钟 |
| `OE` | **D10** | 输出使能 `SERVO_ENABLE_PIN`，LOW 使能舵机输出 / HIGH 关断 |
| `VCC` | 5V（逻辑） | 板载逻辑供电，可接 Arduino 5V |
| `V+` | **外部 5V** | 舵机动力，**必须外接独立电源**（7 个舵机 Arduino 带不动） |
| `GND` | GND | **与 Arduino 共地** |

### 舵机接 PCA9685 通道

与 `wall-e.ino:157` 注释定义、`preset[][2]`（`wall-e.ino:144`）下标一一对应：

| 通道 | 舵机 | 代码下标 |
|-----|------|---------|
| 0 | 头部旋转 (head rotation) | preset[0] |
| 1 | 颈上 (neck top) | preset[1] |
| 2 | 颈下 (neck bottom) | preset[2] |
| 3 | 右眼 (eye right) | preset[3] |
| 4 | 左眼 (eye left) | preset[4] |
| 5 | 左臂 (arm left) | preset[5] |
| 6 | 右臂 (arm right) | preset[6] |

> 通道 7+ 未使用；`NUMBER_OF_SERVOS = 7`（`wall-e.ino:86`）。

## 2. 电机控制板：三脚控制（DIR + PWM + BRAKE）

`MotorController.hpp` 注释明确**为 Arduino Motor Shield Rev.2 设计**，每个电机用 3 个引脚。引脚定义见 `wall-e.ino:32-38`：

| 功能 | 左电机 | 右电机 | 作用 |
|------|--------|--------|------|
| **DIR**（方向） | **D12** | **D13** | HIGH=正转，LOW=反转 |
| **PWM**（速度） | **D3** | **D11** | `analogWrite`，0-255 |
| **BRAKE**（刹车） | **D9** | **D8** | HIGH 刹车，LOW 释放 |

`setSpeed()` 逻辑（`MotorController.hpp:77-113`）：PWM>0 正转、<0 反转、=0 刹车；改变方向时先释放刹车，再设方向，最后出 PWM。

### 不同电机板的接法

| 电机板 | DIR 脚 | PWM 脚 | BRAKE 脚处理 |
|--------|--------|--------|-------------|
| **Arduino Motor Shield Rev2**（原设计） | 12/13 | 3/11 | 9/8 直接用 |
| **L298N** | IN1(左)/IN3(右) | ENA/ENB | BRAKE 接 EN 使能脚（HIGH=使能=不刹）；或悬空不用刹车 |
| **TB6612FNG** | AIN1/BIN1 | PWMA/PWMB | 接 STBY，或不用 |

> ⚠️ L298N 方向通常用 IN1+IN2 两脚组合，而代码只用单 DIR 脚。适配时把 IN2 接 DIR 反相（或用非门），或改代码为两脚方向。**最省事是用 Motor Shield Rev2，引脚完全对应。**

## 3. 电源（关键，接错烧板）

```
3S LiPo (12.6V满电) ──┬──> 电机驱动板 VMOT (12V)
                      │
                      ├──> DC-DC 降压 12V->5V (≥5A) ──> PCA9685 V+ (舵机动力)
                      │                            └──> Arduino 5V (可选，或 USB 供电)
                      │
                      └──> 分压电路 (可选电池检测) ──> Arduino A2
```

- **舵机电源必须独立**：7 个舵机瞬时电流大，从 Arduino 5V 取电会烧 UNO。PCA9685 的 `V+` 接 buck 输出 5V（≥5A）。
- **共地**：Arduino、PCA9685、电机板、buck 的 GND 必须全部连一起，否则 I2C/PWM 信号不稳。
- **舵机动力与逻辑分离**：PCA9685 的 `VCC`（逻辑）和 `V+`（舵机）是分开的，别混。
- 电池参数：`BATTERY_MAX_VOLTAGE = 12.6`（3S 满）、`BATTERY_MIN_VOLTAGE = 10.2`（`wall-e.ino:57-58`）。

## 4. 可选：电池检测（`#define BAT_L`）

`wall-e.ino:54` 默认注释掉。启用时：

- **A2** 接分压电路中点：R1=100kΩ 接电池正极，R2=47kΩ 接地，中点接 A2。
- `DIVIDER_SCALING_FACTOR = 0.3197`（`wall-e.ino:59`）对应这组电阻，换电阻值要改公式 `R2/(R1+R2)`。
- 每 `STATUS_CHECK_TIME = 10000ms` 测一次，结果以 `Battery_<百分比>` 串口上报（见 `docs/SERIAL_PROTOCOL.md`）。
- 实物图：`images/battery_level_circuit.jpg`。

## 5. 可选：OLED（`#define OLED`，须先开 `BAT_L`）

- SH1106 128×64 I2C，与 PCA9685 **共 I2C 总线**（SDA/SCL 并接，地址不冲突）。
- 构造函数默认 SH1106_128X64_NONAME_1_HW_I2C（`wall-e.ino:78`），换屏改这里。
- 显示电池百分比。实物图：`images/oLed_circuit.jpg`。

## 6. 接线速查（最小系统）

```
Arduino UNO R3
  A4  ──── PCA9685 SDA
  A5  ──── PCA9685 SCL
  D10 ──── PCA9685 OE
  D12 ──── 电机板 左 DIR
  D13 ──── 电机板 右 DIR
  D3  ──── 电机板 左 PWM
  D11 ──── 电机板 右 PWM
  D9  ──── 电机板 左 BRAKE
  D8  ──── 电机板 右 BRAKE
  GND ──── 所有板共地
  (PCA9685 V+ 接外部 5V buck，不接 Arduino)
```

## 安全注意

- **舵机电源务必独立 buck**：从 Arduino 5V 取电会烧。
- **所有 GND 共地**：I2C/PWM 不稳多半是地没连好。
- **电机板 12V 与舵机 5V 不要混接**。
- **首次上电先不接电机/舵机**，用 Arduino IDE 串口监视器（115200）看初始化日志：
  - `--- Wall-E Control Sketch ---`
  - `Starting up the servo motors`
  - `Startup complete; entering main loop`
- 舵机首次动作可能超出物理行程（未标定），先用 `wall-e_calibration/wall-e_calibration.ino` 标定 `preset[][2]` 再贴回 `wall-e.ino:144`。

## 验证依据

| 项 | 代码落点 |
|----|---------|
| 电机引脚定义 | `wall-e.ino:32-38` |
| 舵机通道与下标 | `wall-e.ino:157`、`preset[][2]`（`:144`） |
| PCA9685 地址 0x40 / 60Hz | `wall-e.ino:98`、`:178` |
| 电机三脚控制逻辑 | `MotorController.hpp:77-113` |
| SERVO_ENABLE_PIN (D10) | `wall-e.ino:38` |
| 电池电路与参数 | `wall-e.ino:54-59` |
| OLED 构造 | `wall-e.ino:78` |
| 串口协议 | `docs/SERIAL_PROTOCOL.md` |
| 硬件采购清单 | `docs/HARDWARE.md` |
