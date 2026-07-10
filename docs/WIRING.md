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

## 2. 电机控制板

`MotorController.hpp` 默认支持两种工作模式，通过 `wall-e.ino` 顶部的宏选择：

```cpp
#define MOTOR_DRIVER_TB6612FNG       // 双方向脚驱动（TB6612FNG）
// #define MOTOR_DRIVER_ARDUINO_SHIELD // 原设计 Arduino Motor Shield Rev2
```

### 2.1 Arduino Motor Shield Rev2（原设计，单 DIR + PWM + BRAKE）

每个电机用 3 个引脚。引脚定义见 `wall-e.ino`：

| 功能 | 左电机 | 右电机 | 作用 |
|------|--------|--------|------|
| **DIR**（方向） | **D12** | **D13** | HIGH=正转，LOW=反转 |
| **PWM**（速度） | **D3** | **D11** | `analogWrite`，0-255 |
| **BRAKE**（刹车） | **D9** | **D8** | HIGH 刹车，LOW 释放 |

`setSpeed()` 逻辑：PWM>0 正转、<0 反转、=0 刹车；改变方向时先释放刹车，再设方向，最后出 PWM。

### 2.2 TB6612FNG（双 DIR 脚模式）

TB6612FNG 每路电机需要两个互补的方向输入（`AIN1/AIN2`、`BIN1/BIN2`）。代码已经支持用 Arduino 直接输出这两路互补信号，**无需 74HC04 反相器**。

接线前，确保 `wall-e.ino` 中取消注释：

```cpp
#define MOTOR_DRIVER_TB6612FNG
```

#### 引脚分配

| 功能 | 左电机 | 右电机 | 接 TB6612FNG |
|------|--------|--------|-------------|
| **DIR1** | **D12** | **D13** | AIN1 / BIN1 |
| **DIR2**（互补） | **D9** | **D8** | AIN2 / BIN2 |
| **PWM** | **D3** | **D11** | PWMA / PWMB |

#### 完整接线表

| Arduino UNO | TB6612FNG | 说明 |
|-------------|-----------|------|
| D12 | AIN1 | 左电机方向 1 |
| D9 | AIN2 | 左电机方向 2（互补） |
| D3 | PWMA | 左电机 PWM |
| D13 | BIN1 | 右电机方向 1 |
| D8 | BIN2 | 右电机方向 2（互补） |
| D11 | PWMB | 右电机 PWM |
| 5V | VCC | 逻辑电源 |
| 12V | VM | 电机动力（3S LiPo） |
| GND | GND | 与 Arduino、buck、电池负极共地 |

> **STBY 说明**：市面上很多 TB6612FNG 模块已经把 `STBY` 内部接到 `VCC`，板上只标 `VCC`。直接把 5V 接到 `VCC` 即可。如果你的模块有独立 `STBY` 引脚，请把它也接到 5V。

> 电机方向若反了，对调该侧电机的两根输出线即可。

### 2.3 不同电机板的接法速查

| 电机板 | DIR 脚 | PWM 脚 | 其他处理 |
|--------|--------|--------|---------|
| **Arduino Motor Shield Rev2** | 12/13 | 3/11 | BRAKE 9/8 直接用；选择 `MOTOR_DRIVER_ARDUINO_SHIELD` |
| **L298N** | IN1(左)/IN3(右) | ENA/ENB | IN2/IN4 接 DIR 反相（或用非门）；BRAKE 接 EN 使能脚或悬空 |
| **TB6612FNG** | AIN1/BIN1 (12/13) | PWMA/PWMB (3/11) | AIN2/BIN2 接 D9/D8；选择 `MOTOR_DRIVER_TB6612FNG` |

> ⚠️ 最省事、引脚完全对应的是 **Arduino Motor Shield Rev2**；TB6612FNG 性价比更高，但需要在代码里切换到双方向脚模式。

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

### 3.1 PCA9685 的 V+ 与 VCC（易混淆，最易接错）

PCA9685 板上 `V+` 和 `VCC` 是**两个独立电源域**，务必分清：

| 脚 | 供什么电 | 电流 | 接法 |
|----|---------|------|------|
| **V+** | 舵机电机动力 | 7 舵机瞬时可达 3–5A | buck 输出 5V，**绝不接 Arduino** |
| **VCC** | PCA9685 芯片逻辑 | ~几十 mA | Arduino 5V 或 buck 5V，二选一 |

- 若 buck 已供 V+，**V+ 不要再接 Arduino**（反而有烧板风险）。
- VCC 必须有电（接 Arduino 5V 或 buck 5V 之一），否则芯片不工作。
- 推荐接法（Arduino 完全不参与给 PCA9685 供电，只供信号 + 共地）：

```
buck 5V ──┬──> PCA9685 V+   (舵机动力)
          └──> PCA9685 VCC  (芯片逻辑，也用 buck)
Arduino A4/A5/D10 ──> PCA9685 SDA/SCL/OE  (仅信号)
Arduino GND ──────── PCA9685 GND + buck GND (共地)
```

### 3.2 共地具体接法（降压模块如何共地）

降压模块（LM2596 / XL4015 / MP1584 等）通常 4 个脚：`IN+ IN- OUT+ OUT-`。
`IN-` 与 `OUT-` 在模块内部通常**连通**（非隔离型），任选一个作"buck 地"即可。

> ⚠️ 隔离型降压模块 IN-/OUT- 分开，要接 `OUT-`（输出侧地）。Wall-E 用普通非隔离降压，连通，接哪个都行。

**共地 = 所有设备负极连成同一电位**。两种接法：

**接法 A：汇到一根公共地线**
```
电池负极 ──┬── buck IN-
           │
           └── Arduino GND(任一) ──── PCA9685 GND
                          │
                          └── 电机驱动板 GND
```

**接法 B：以 Arduino GND 为分配器**（UNO 有 3 个 GND 脚刚好够分）
```
电池负极 ───────── buck IN-
buck OUT- ─────── Arduino GND ①   (电池地经 buck 到这)
Arduino GND ② ── PCA9685 GND      (信号地)
Arduino GND ③ ── 电机驱动板 GND    (电机地)
```

**Wall-E 完整地线接法**：
```
3S LiPo 负极 ─────────────┬── buck IN-
                          │
buck OUT+ (5V) ─── PCA9685 V+   (舵机动力正)
buck OUT- (GND) ──┬─────── PCA9685 GND  (舵机动力地)
                  │
                  └────── Arduino GND(任一)  ← 共地关键！
                              │
                              └── 电机驱动板 GND
```

核心：**buck 的 `OUT-`（或 `IN-`，连通）用一根线接 Arduino 任一 `GND` 脚**，即完成共地。

### 3.3 为什么必须共地

PCA9685 的信号（SDA/SCL/OE）是 Arduino 发的电压，电压必须有参考点：
- Arduino 的"5V"是相对它自己的 GND。
- PCA9685 收信号，也相对它自己的 GND 判断。
- 两者 GND 不连，Arduino 的"5V"在 PCA9685 看来是任意值，**I2C 通信乱、舵机乱转或不动**。

共地 = 给双方一个共同的 0V 参考。不共地，信号浮空，必出问题。

### 3.4 验证共地是否成功（万用表蜂鸣档）

通断档（蜂鸣），一根表笔接 Arduino GND：
1. 另一端接 PCA9685 GND -> 应响（连通）。
2. 另一端接 buck `OUT-` -> 应响。
3. 另一端接电池负极 -> 应响。

三处都响即共地成功；哪处不响，就是那根地线没接。

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

### 6.1 Arduino Motor Shield Rev2

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

### 6.2 TB6612FNG

```
Arduino UNO R3
  A4  ──── PCA9685 SDA
  A5  ──── PCA9685 SCL
  D10 ──── PCA9685 OE
  D12 ──── TB6612 AIN1
  D9  ──── TB6612 AIN2
  D3  ──── TB6612 PWMA
  D13 ──── TB6612 BIN1
  D8  ──── TB6612 BIN2
  D11 ──── TB6612 PWMB
  5V  ──── TB6612 VCC
  12V ──── TB6612 VM
  GND ──── TB6612 GND + PCA9685 GND + buck GND（共地）
  (PCA9685 V+ 接外部 5V buck，不接 Arduino)
```

### 6.1 Arduino 多个 GND 脚接哪个

UNO 板上标 `GND` 的脚通常有 **3 个**（电源区 2 个并排、模拟区 AREF 旁 1 个），**板内是同一根铜线、物理连通**，接任意一个都等价，不影响功能。

选哪个只看布线方便：离要接的线最近即可。UNO 有多个 GND 的好处是可以当地线分配器用——几个设备的地线各接一个 GND 脚，避免挤在一个脚上。

| 要接的信号 | 建议接的 GND |
|----------|------------|
| A4/A5（I2C） | 模拟区附近 GND（离 A4/A5 近） |
| D8-D13（电机/使能） | 数字侧电源区 GND |
| 电源输入（电池/buck） | 电源区 GND |

关键原则是**共地**（见 3.2），不是接哪个脚——所有设备负极都汇到 Arduino GND（任一即可）即可。

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
