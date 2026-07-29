# 小智（xiaozhi-esp32）语音迁移文档

> 本文档记录 Wall-E 机器人从 Arduino 固件迁移到小智 ESP-IDF 语音方案的全部信息：源码溯源、构建烧录、引脚、MCP 工具清单、控制面架构、Wi-Fi 配网与 4G 回退。
>
> 上游溯源：`wall-e_xiaozhi/` 目录 vendored 自 https://github.com/78/xiaozhi-esp32 ，上游 commit `e0074e9`（feat: add ESP32-S31-Korvo-1 Board support, v2.4.0），MIT 许可证（`wall-e_xiaozhi/LICENSE`）。同步上游时按该 commit 做三方比对；我们对上游仅有的侵入性修改是 `main/Kconfig.projbuild` 与 `main/CMakeLists.txt` 中各一段 `walle` 板型注册。

## 1. 架构总览

```
一块 ESP32-S3 N16R8 同时承担：
  唤醒词/ASR/TTS（小智音频管线 + xiaozhi.me 官方云）
  MCP 工具（云端 LLM function calling → 动作）
  运动控制（9 舵机动力学 + TB6612 电机 + 动画队列）
  眼睛显示（GC9A01 圆屏，LVGL + emoji）
  状态显示（ST7789 1.3" 方屏，电量/Wi-Fi/助手状态/自动模式，1s 刷新）
  Web 控制面板（esp_http_server :80，API 兼容树莓派 Flask 版）
  USB 串口协议（调试 + 树莓派回退）
```

控制面统一入口：`WalleMotion::EvaluateCommand(prefix, number)`——MCP 工具、USB 串口、Web 路由全部走同一分发器，语义与 Arduino 版 `docs/SERIAL_PROTOCOL.md` 完全一致。

Arduino 版固件（`archive/wall-e_esp32/wall-e_esp32/`）**冻结保留**，作为树莓派方案回退，不再演进。

## 2. 环境搭建（Windows）

1. 安装 ESP-IDF（首选 **v6.0.2**；xiaozhi 兼容序列最低 **v5.5.2**）：
   ```bash
   eim install -i v6.0.2 -t esp32s3 -n true
   ```
2. 激活环境（eim 安装的版本）：
   ```bash
   eim run <idf-id> -- idf.py --version   # 或在 ESP-IDF PowerShell 中操作
   ```

## 3. 构建与烧录

```bash
cd wall-e_xiaozhi
idf.py set-target esp32s3
idf.py menuconfig        # Xiaozhi Assistant -> Target Board -> 选 "Wall-E Voice Robot"
idf.py build
idf.py -p COMx flash monitor
```

批量构建脚本也支持：`python scripts/build.py walle`（读取 `main/boards/walle/config.json`）。

首次烧录后：
1. 设备进入配网模式（或按 BOOT 键触发），手机连设备热点配 Wi-Fi。
2. 打开串口日志中的激活码，到 https://xiaozhi.me 控制台注册并绑定设备。
3. 唤醒词"你好小智"开始对话。

## 4. 引脚表（`main/boards/walle/config.h`）

| 功能 | GPIO | 说明 |
|------|------|------|
| INMP441 SCK/WS/SD | 40 / 41 / 44 | I2S 麦克风（输入 16kHz） |
| PCM5102 BCK/LRCK/DIN | 12 / 13 / 11 | I2S DAC（输出 24kHz），经 PAM8406 功放 |
| GC9A01 SCK/MOSI | 21 / 18 | SPI3，40MHz |
| GC9A01 CS/DC/RST/BL | 38 / 39 / 47 / 48 | 单块圆屏渲染双眼（BOM 只有 1 块屏 + 2 镜片） |
| TB6612 AIN1/AIN2 | 5 / 6 | 左电机方向 |
| TB6612 BIN1/BIN2 | 15 / 16 | 右电机方向 |
| TB6612 PWMA/PWMB | 4 / 7 | LEDC 20kHz 8bit |
| TB6612 STBY | 17 | 拉高使能 |
| LU9685 SDA/SCL | 8 / 9 | I2C @ 0x40，60Hz |
| LU9685 OE | 10 | 舵机输出使能（低有效） |
| 电池分压 | 1 (ADC1_CH0) | 分压比 0.180（100k+22k），满电 12.6V ≤ 3.1V |
| BOOT 按键 | 0 | 单击：配网/对话切换；双击：手动切 Wi-Fi/4G |
| ST7789 CS/DC（可选） | 42 / 14 | 阶段 3 状态屏，与 GC9A01 共用 SCK/MOSI |
| ML307A TX/RX（可选） | 2 / 3 | 4G 模组 UART（ESP32 视角），DTR 不接 |

## 5. MCP 工具清单（`walle_mcp_tools.cc`）

| 工具 | 参数 | 说明 |
|------|------|------|
| `self.walle.move` | direction, speed, duration_ms | 前进/后退/原地转向，定时自动停止 |
| `self.walle.stop` | — | 立即停止 |
| `self.walle.head` | rotation(0-100), neck(0-200) | 头部旋转 + 颈部（T/B 分段映射） |
| `self.walle.arms` | left, right (0-100, -1=不动) | 手臂 |
| `self.walle.eyes` | expression | neutral/sad/left/right 眼部表情 |
| `self.walle.play_animation` | id (0-2) | 复位/开机眨眼/好奇观察 |
| `self.walle.set_auto_mode` | on | 自主随机小动作 |
| `self.battery.get_level` | — | 电量百分比 |

## 6. Web 控制面板（`walle_web_server.cc`，阶段 3）

esp_http_server 跑在 **80 端口**，路由契约与树莓派 Flask 版（`archive/arduino-pi/web_interface/app.py`）一致：

| 路由 | 方法 | 表单参数 | 说明 |
|------|------|---------|------|
| `/` | GET | — | 内嵌单文件控制页（摄像头画面、方向键、9 路舵机滑杆、动画、自动模式、电量轮询） |
| `/motor` | POST | stickX, stickY (-1.0..1.0) | → `X`/`Y` 指令 |
| `/settings` | POST | type, value | motorOff→`O`、steerOff→`S`、animeMode→`M`、restart→`esp_restart()`；volume 静默接受 |
| `/animate` | POST | clip | → `A` 指令 |
| `/servoControl` | POST | servo(G/T/B/E/U/L/R/I/J), value(0..100) | → 对应舵机指令 |
| `/arduinoStatus` | POST | type=battery | 返回电量 JSON |
| `/gamepadStatus` | GET/POST | — | 恒报未连接（IDF 版暂无蓝牙手柄） |

**注意**：无登录鉴权，仅限可信局域网使用。`/tts`、`/audio`（本地 wav）未移植（云端 TTS 替代）。

## 7. ESP32-S3-CAM 摄像头接入（第二块板）

摄像头是**独立的网络外设**：第二块 ESP32-S3-CAM 跑主目录下的 Arduino 推流固件（`wall-e_esp32_cam/`），与主控仅电源线相连，画面走 Wi-Fi，浏览器直接拉它的流，不经过小智主控。

接入步骤：

1. 编辑 `wall-e_esp32_cam/camera_pins.h` 选板型（XIAO/Freenove/S3-EYE 预设），在 sketch 顶部填家庭 Wi-Fi 凭据（**小智固件不发 `WallE` AP**，留空会一直回退重试；不要把真实密码提交到 git）。
2. 用 Arduino IDE（Arduino-ESP32 core 3.x）烧录，从串口日志拿到 CAM 板的 IP。
3. 编辑 `main/boards/walle/walle_web_server.cc` 内嵌页面顶部的 `const stream_url = "http://<cam-ip>/stream";`，重新 `idf.py build flash`。
4. 打开 Web 控制面板（`http://<esp-ip>`），"Camera" 区即显示实时画面；`stream_url` 留空时该区域自动隐藏。

注意：语音 Opus 流与 MJPEG 同处 2.4GHz，开视频若导致语音卡顿，把 CAM 固件的 `CAM_FRAME_SIZE` 降到 QVGA 或提高 JPEG 压缩比（`CAM_JPEG_QUALITY`）。

远期替代路线：主控直接挂摄像头（上游已含 `esp32-camera` 组件），配合拍照 MCP 工具让云端多模态 LLM 看图——属新开发，未实现。

## 8. 状态屏（`walle_status_display.cc`，阶段 3）

ST7789 1.3" 240x240，与 GC9A01 共用 SPI3（CS=42/DC=14，RST/BL 共接）。通过 `lvgl_port_add_disp` 注册为 esp_lvgl_port 任务上的**第二个 LVGL 屏**（须在主屏初始化之后调用 `Init()`）。内容 1s 刷新：电量、网络（Wi-Fi IP 或 4G）、助手状态、自动模式。引脚与偏移在 `config.h` 的 `STATUS_DISPLAY_*`；画面偏移时调 `STATUS_DISPLAY_OFFSET_X/Y`。

## 9. 已知的放弃/降级项（相对 Arduino 版）

- **蓝牙手柄**：Bluepad32 是 Arduino 专用库，不可移植（远期可用 IDF esp_hid 重做）。
- **wav 音效播放**：ESP32-audioI2S 不可移植；音效由云端 TTS 替代。
- **自绘矢量眼睛**：改用 xiaozhi LVGL/emoji 显示框架，表情由云端 emotion 驱动。

## 10. 舵机标定

标定流程仍用 Arduino 版 `archive/wall-e_esp32/wall-e_esp32_calibration/`（在 Arduino IDE 烧一次标定 sketch，得到 `preset` 数组），把结果贴到 `wall-e_xiaozhi/main/boards/walle/walle_motion.cc` 顶部的 `preset[][2]`。逻辑关节顺序与第三方线束通道的映射（`servo_channel[]`）两处保持一致。

## 11. 服务端切换（官方云 → 自建）

固件启动时先 POST OTA 接口获取服务器配置。默认 `https://api.tenclass.net/xiaozhi/ota/`（xiaozhi.me 官方云）；自建 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 后，menuconfig 改 `CONFIG_OTA_URL` 或运行时写 NVS `ota_url` 指向自建地址即可，固件逻辑不变。自建服务端可接火山方舟豆包（对应 `docs/VOICE_LLM_PLAN.md` 方向）。

## 12. Wi-Fi 配网（热点配网）

采用上游热点配网方案（sdkconfig `CONFIG_USE_HOTSPOT_WIFI_PROVISIONING=y`，BluFi 未启用）。

**进入配网模式**：
- 手动：开机 starting 状态下**单击 BOOT 键**（GPIO0）——首次配网推荐用这条；
- 自动：开机连已保存 Wi-Fi 超时后进配网模式。**注意**：当前 `WIFI_AUTO_FALLBACK_4G 1` 会把这条超时路径拦截为自动切 4G（见下节）；若希望新设备超时后先进配网而非跳 4G，把该宏置 0。

**配网步骤**：
1. 设备开启 AP 热点，SSID 为 `Xiaozhi-XXXX`（XXXX=MAC 后两字节），屏幕弹出热点名与网址提示；
2. 手机/电脑连上该热点；
3. 浏览器打开 `http://192.168.4.1`（多数手机会自动弹出 captive portal）；
4. 网页选择/填写目标 Wi-Fi 的 SSID 与密码，提交；
5. 凭据存入 NVS，设备退出 AP 转 STA 连接路由器，此后开机自动重连。

配网网页同时支持修改 OTA 服务器地址（对应第 11 节自建服务端）与休眠设置。配网状态下双击 BOOT 可切到 4G；4G 状态下双击切回 Wi-Fi 后再配网。

## 13. 4G 回退（ML307A，可选）

板型基于上游 `DualNetworkBoard`（`main/boards/common/`）：Wi-Fi 与 ML307 4G 二选一，当前网络类型存 NVS（`network.type`），切换即重启。`78/esp-ml307` 组件已在 `main/idf_component.yml`，无需新增依赖。

**默认行为（`WIFI_AUTO_FALLBACK_4G 1`，`config.h`）**：默认走 Wi-Fi；连接超时（上游 `CONNECT_TIMEOUT_SEC`）本应变 AP 配网模式，walle 板包装了网络事件回调（`walle_board.cc` 的 `SetNetworkEventCallback`），把"非用户手动触发"的配网进入事件改判为 Wi-Fi 失败 → `SwitchNetworkType()` 写 NVS=ML307 并重启进 4G。用户手动单击 BOOT 进配网、双击 BOOT 切网时会置 `manual_config_request_`，不触发自动回退。

**手动切换**：starting / 配网状态下双击 BOOT 键，Wi-Fi ↔ 4G 互切（重启生效，掉电保存）。

**接线（ML307A Cat.1 模组）**：

| ML307A | ESP32-S3 | 说明 |
|--------|----------|------|
| TXD | GPIO3 (`ML307_RX_PIN`) | 交叉连接 |
| RXD | GPIO2 (`ML307_TX_PIN`) | 交叉连接 |
| GND | GND | 共地 |
| VCC | 独立 5V/2A | 模组峰值电流大，不要与舵机/电机共用薄弱电源 |

APN 一般自动识别（国内三大运营商 Cat.1 卡免配置）；DTR 脚未接（`GPIO_NUM_NC`），模组休眠唤醒由 AT 命令维持。Web 控制面板在 4G 下同样可用（PPP 网卡有独立 IP，从串口日志查询）。

**实机验证清单（待硬件）**：
1. 填错误 Wi-Fi 密码或断电路由器 → 超时后自动重启进 4G，状态屏网络行显示 `Net: 4G (ML307)`。
2. 双击 BOOT 在 Wi-Fi/4G 间手动切换。
3. 4G 下语音对话、MCP 动作、Web 面板（串口日志查 IP）正常。

**换模组**：NT26 改继承/构造对应 board 类即可；其他 AT 模组需适配 `78/esp-ml307` 的 AT 指令集，引脚宏集中在 `config.h`。
