# Wall-E 串口通信协议

Arduino 固件（`wall-e/wall-e.ino`）与树莓派 Web 服务（`web_interface/app.py`）之间的串口通信契约。新增/修改指令必须遵守本文档，否则两端不对应、硬件不响应。

## 传输层

| 项 | 值 | 落点 |
|---|---|---|
| 物理链路 | USB 串口（Arduino UNO 默认 `/dev/ttyACM0`） | - |
| 波特率 | **115200**（两端必须一致） | `wall-e.ino:186`、`app.py:110` |
| 帧定界 | 以 `\n`（或 `\r`）结尾 | `wall-e.ino:225` |
| 帧格式 | `<前缀1字符><数字>`，如 `X100`、`A2`、`O40` | - |
| 帧上限 | **`MAX_SERIAL_LENGTH = 5` 字符** | `wall-e.ino:91` |

## 解析逻辑（关键，容易踩坑）

`readSerial()`（`wall-e.ino:219`）+ `evaluateSerial()`（`wall-e.ino:258`）的解析有几点不直观，写新指令必须知道：

1. **第 1 个字符**进 `firstChar`（指令前缀），**从第 2 字符起**才进 `serialBuffer`，末尾补 `\0`。
2. `evaluateSerial()` 里 `number = atoi(serialBuffer)`，所以**数字部分是 serialBuffer，不含前缀**。
3. **缓冲区满 5 字符即触发解析**（`serialLength == MAX_SERIAL_LENGTH`，`wall-e.ino:241`），不等 `\n`。后果：
   - 前缀 1 字符 + 数字最多 4 字符，即**数字范围受 5 字符上限约束**。
   - 负号算 1 字符：`X-100` 共 5 字符，正好满触发；`S-100` 同理。
   - 超过 5 字符的指令会被**截断**。
4. 每条指令执行后，Arduino **回显** `Serial.print(firstChar); Serial.println(number);`（`wall-e.ino:263`）。回显不是 `Battery_`，Web 端 `__parse_message()` 只认含 `"Battery"` 的行，其他回显被丢弃。
5. Arduino **不会 ACK** 业务指令是否执行成功，Web 端靠 `is_connected()` 判断链路。
6. Web 端 `ArduinoDevice.send_command(cmd)` 把 `cmd + "\n"` 入队，由 `__communication_thread`（`app.py:188`）后台线程逐条写出，天然线程安全。

## Web -> Arduino：带数字指令

Web 路由调 `arduino.send_command("前缀" + str(值))`。

| 前缀 | 数字范围 | 含义 | Arduino 落点 | Web 路由 |
|------|---------|------|-------------|---------|
| `X` | -100..100 | 左右转向（`×2.55 -> PWM`，`turnValue`） | `wall-e.ino:268` | `/motor` (stickX) |
| `Y` | -100..100 | 前进/后退（`×2.55 -> PWM`，`moveValue`） | `wall-e.ino:269` | `/motor` (stickY) |
| `S` | -100..100 | 转向偏置/微调（`turnOffset`，直行修正） | `wall-e.ino:270` | `/settings` (steerOff) |
| `O` | 0..250 | 电机死区补偿（`motorDeadzone`） | `wall-e.ino:271` | `/settings` (motorOff) |
| `M` | 0 / 1 | 舵机自动模式 关 / 开（`autoMode`） | `wall-e.ino:281/282` | `/settings` (animeMode) |
| `A` | n | 播放动画 n（动画定义在 `animations.ino`） | `wall-e.ino:276` | `/animate` |
| `G` | 0..100 | 头部旋转（-> `setpos[0]`） | `wall-e.ino:303` | `/servoControl` |
| `T` | 0..100 | 颈上（-> `setpos[1]`） | `wall-e.ino:299` | `/servoControl` |
| `B` | 0..100 | 颈下（-> `setpos[2]`） | `wall-e.ino:295` | `/servoControl` |
| `E` | 0..100 | 左眼（-> `setpos[4]`） | `wall-e.ino:307` | `/servoControl` |
| `U` | 0..100 | 右眼（-> `setpos[3]`） | `wall-e.ino:311` | `/servoControl` |
| `L` | 0..100 | 左臂（-> `setpos[5]`） | `wall-e.ino:287` | `/servoControl` |
| `R` | 0..100 | 右臂（-> `setpos[6]`） | `wall-e.ino:291` | `/servoControl` |

## Web -> Arduino：单字符指令

**无数字参数**，前缀即整条指令。用于 Arduino IDE 串口监视器手动调试，Web 端不发（无对应路由）。

| 字符 | 含义 | 落点 |
|------|------|------|
| `w` | 前进（`moveValue = pwmspeed`，头回正） | `wall-e.ino:320` |
| `s` | 后退 | `wall-e.ino:330` |
| `a` | 左转 + 看左 | `wall-e.ino:335` |
| `d` | 右转 + 看右 | `wall-e.ino:340` |
| `q` | 停止（`moveValue/turnValue = 0`，头回正） | `wall-e.ino:325` |
| `j` | 头左倾（眼） | `wall-e.ino:349` |
| `l` | 头右倾（眼） | `wall-e.ino:353` |
| `i` | 悲伤头 | `wall-e.ino:357` |
| `k` | 中性头 | `wall-e.ino:361` |
| `f` | 头抬起 | `wall-e.ino:369` |
| `g` | 头前探 | `wall-e.ino:373` |
| `h` | 头低下 | `wall-e.ino:377` |
| `b` | 左臂低 / 右臂高 | `wall-e.ino:385` |
| `n` | 双臂中性 | `wall-e.ino:389` |
| `m` | 左臂高 / 右臂低 | `wall-e.ino:393` |

> ⚠️ `m`（单字符，双臂动作）与 `M`+数字（自动模式开关）**大小写不同**，是两条独立指令，勿混。

## Arduino -> Web：反向（电池电量）

Arduino **只**主动发回电池电量，格式：

```
Battery_<百分比>
```

- 例：`Battery_87`
- 落点：`wall-e.ino:648`，由 `checkBatteryLevel()` 每 `STATUS_CHECK_TIME = 10000ms` 发一次。
- **仅在 `#define BAT_L`（`wall-e.ino:54`）启用时**才有。未启用时 Arduino 不发任何东西，Web 端 `/arduinoStatus` 永远返回 `No battery level available`。
- Web 端 `__parse_message()`（`app.py:222`）按 `_` split，第二段须为整数才存入 `battery_level`，供 `/arduinoStatus` 路由读取。

## 舵机位置映射（0..100 -> 实际 PWM）

舵机指令（`G/T/B/E/U/L/R`）的 `0..100` 是归一化位置，不是 PWM。Arduino 端按 `preset[][2]`（`wall-e.ino:144`，由 `wall-e_calibration/wall-e_calibration.ino` 标定得到）线性映射：

```
setpos[i] = int( number * 0.01 * (preset[i][1] - preset[i][0]) + preset[i][0] )
```

- `0` -> `preset[i][0]`（LOW 端），`100` -> `preset[i][1]`（HIGH 端）。
- `-1` 表示**该次动作跳过该舵机**（仅用于 `animations.ino` 的 `queue.push({time, head, necT, necB, eyeR, eyeL, armL, armR})` 数组里，不适用于实时指令）。
- 关节-下标对应（`wall-e.ino:157` 注释）：

| 下标 | 关节 |
|------|------|
| 0 | head 头转 |
| 1 | necT 颈上 |
| 2 | necB 颈下 |
| 3 | eyeR 右眼 |
| 4 | eyeL 左眼 |
| 5 | armL 左臂 |
| 6 | armR 右臂 |

## 接线契约（最重要）

**新增电机/舵机指令必须三处同步**，否则两端不对应、硬件不响应或行为错乱：

1. `app.py` 加 Flask 路由（调 `arduino.send_command("前缀" + str(值))`）
2. `wall-e.ino` 的 `evaluateSerial()` 加对应 `else if (firstChar == '?')` 分支
3. 前端 `static/js/main.js` 加调用该路由的交互（按钮/摇杆/滑杆）

且指令前缀**避开已用的**：
- 带数字：`X Y S O M A G T B E U L R`
- 单字符：`w a s d q j l i k f g h b n m`

全部已占用，新指令须另选字母。
