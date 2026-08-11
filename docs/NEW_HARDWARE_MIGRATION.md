# 新硬件方案固件迁移 — 进度与后续计划

> 基于 `hardware/另一套硬件方案.md`（第三方 Wall-E 套件）适配小智固件（`wall-e_xiaozhi/main/boards/walle/`）。
> 本文档记录已完成工作、关键决策与回家后续执行的步骤。每次执行一步，确认后再继续下一步。

## 关键决策（已与用户确认）

1. 直接改 `boards/walle/config.h`，不新建板型（旧硬件接线配置被覆盖）。
2. **4G 回退**：ML307R-DL mini 核心板用 GPIO2/3，经 J-4G 8P 线束接入（吃 5V，SIM 在模块上，见 `hardware/主板设计说明.md` §1）。
3. 摄像头改走 **UART 串口链路**（主控 GPIO9/10 ↔ CAM GPIO14/21），Wi-Fi MJPEG 预览保留。CAM 模组已核对确认（见 Step 4.5：艾尔赛 ESP32CAM_V1.1，OV3660）。
4. 4 个按键全部实现：BOOT(0)、音量+(38)、音量-(39)、长按重启(41)。
5. 眼睛屏（1.28 寸圆屏 ×2）**已确认接在 ESP32-CAM 模组上、由 CAM 控制**；接线已确认（2026-07-31）：共享 SPI（SCL=42、SDA=45、DC=41、RST=46），片选丝印 L/R 各自独立（左=GPIO2、右=GPIO0），VDD=3V3；驱动 IC 待确认（疑似 GC9A01）。主控侧 `EYE_DISPLAY_ENABLED=0` 为终态，不再等待主控接线。

## 新硬件引脚映射（已落地到 config.h）

| 功能 | 引脚 |
|---|---|
| 舵机 I2C | SDA=20, SCL=21, OE 未接 |
| 电机 TB6612 | PWMA=19, AIN1/2=17/18, PWMB=11, BIN1/2=12/13, STBY 接 5V（`MOTOR_STBY_WIRED=0`） |
| 麦克风 INMP441 | SCK=5, WS=4, SD=6 |
| PCM5102 DAC | BCK=15, LCK=16, DIN=7 |
| ST7789 状态屏（独立 SPI2） | SCLK=14, MOSI=47, DC=40, RST=45, BL=42, CS 无（NC） |
| 按键 | BOOT=0, VOL+=38, VOL-=39, 重启(长按)=41 |
| CAM UART | TX=9, RX=10, 115200（仅宏落地，固件未实现；CAM 侧 21/14 待板型核对） |
| 电池 ADC | GPIO1（文档无，待硬件确认分压） |
| 4G ML307R-DL | TX=2, RX=3（mini 核心板，J-4G 接入） |
| 眼睛屏×2（接 CAM 模组） | 主控无引脚；CAM 侧共享 SPI：SCL=42, SDA(MOSI)=45, DC=41, RST=46，片选 L/R：左=2、右=0（已确认）；驱动 IC 待确认 |

注意：**GPIO19/20 是 ESP32-S3 原生 USB D-/D+**，被 PWMA 和舵机 I2C 占用 → USB 串口任务已禁用（`WALLE_SERIAL_ENABLED=0`），日志走 UART0（GPIO43/44）。

## 已完成（Step 1–3，已编译通过）

- **Step 1** `hardware/另一套硬件方案.md`：删"按钮 SCK=GPIO5"笔误行，补 5 条备注（USB 冲突、ST7789 无 CS、眼睛屏待确认等），表尾加眼睛屏占位行。
- **Step 2** `config.h` 全量重写（见上表）。
- **Step 3** 板级代码：
  - `walle_board.cc`：音量+/-（单击 ±10，长按最大/静音）、长按重启键；眼睛屏 `EYE_DISPLAY_ENABLED` 开关（当前 0，NoDisplay）；4G 保留。
  - `walle_status_display.cc`：ST7789 独立 SPI2 总线，CS=NC，BL 拉高；眼睛屏禁用时自初始化 LVGL port。
  - `walle_motion.cc`：`MOTOR_STBY_WIRED` 编译开关。OE=NC 由 `pca9685.cc` 原生兼容。
  - `walle_serial.cc`：`WALLE_SERIAL_ENABLED=0` 默认禁用。
  - 验证：`idf.py build` 通过（xiaozhi.bin 2.7MB，余量 35%）。

## 待执行

### Step 4：CAM UART 串口链路（✅ 固件已实现 2026-08-01，未编译/未实机联调）

> 前置依赖：Step 4.5 已确认（艾尔赛 ESP32CAM_V1.1 / OV3660，摄像头引脚与 Freenove 预设匹配，CAM 侧 21/14 可用）。曾指出的 `CAM_LINK_RX_PIN=14` 与 XIAO 预设 Y6 撞脚问题，随板型切换到 Freenove 预设（Y6=12）已消除。

**协议规范见 `docs/CAM_PROTOCOL.md`（2026-07-31 定稿）**，要点：

- v1 为纯控制通道（命令/状态/表情），**不传文件**；拍照为完整流程：准备 → 眼屏倒计时 3-2-1 → 拍摄 → CAM 眼屏本地回放 → 恢复表情（"茄子"口播由主控 TTS）；**含录像控制**（`REC START/STOP` + `EVT REC_DONE`，2026-07-31 修订）。
- 命令集：`HELLO/PING/STATUS/EYES/PHOTO/REC/SHOW/ABORT/LIST`；行式文本，一命令一响应；HTTP 端点全部保留作回退。

实施步骤（详见协议文档「后续任务」）——**均已完成（2026-08-01），待实机验证**：

- ✅ 主控新增 `walle_cam_link.cc/.h`：UART1（TX=9/RX=10，115200）收发 + 互斥 Command() + HTTP 回退；`config.h` 新增 `CAM_UART_ENABLED`。
- ✅ `wall-e_esp32_cam/`：新增 `cam_link.h`（Serial1 TX=21/RX=14，以板型核对为准）+ 拍照状态机（倒计时屏显、拍摄、JPEGDEC 本地解码回放）+ `eye_display.h` overlay 绘制入口（大号数字/RGB 位图）；新依赖 **JPEGDEC（bitbank2）**。
- ✅ 主控侧切换调用方：`walle_mcp_tools.cc`（`self.walle.camera` → PHOTO/SHOW/ABORT/REC；`self.walle.eyes` 追加 EYES 下发）、`walle_web_server.cc`（`/camview` preview/stop）；`walle_cam_viewer` 标注废弃仅备查。
- 验证（未做）：CAM 固件 Arduino IDE 编译（装 JPEGDEC）、`idf.py build`；实机按 `docs/CAM_PROTOCOL.md` §13 测试。

### Step 4.5：CAM 板型核对（✅ 已完成，2026-08 实机调通）

- 板型确认：**艾尔赛 ESP32CAM_V1.1（303ESPCAM01，OV3660 实机 PID 0x3660，8M PSRAM/16M Flash）**，厂商资料 `hardware/ESP32S3_CAM/`；`camera_pins.h` 用 Freenove 预设，摄像头 DVP 引脚调通（SCCB 4/5）。**模块仅在排针上引出 GPIO 0/2/14/21/41/42/45/46**。
- 引脚需求已固化到 `docs/PCB_REQUIREMENTS.md`：**SD 卡不可用**——板载 TF 槽实接 SDMMC 39/40/41/42，其中 39/40 未引出、41/42 与眼屏 DC/SCK 冲突；此前"SD 与摄像头共用 8/9"系固件 XIAO 旧预设误判，实际 DVP 与 SD 无冲突。`CAM_SD_ENABLED` 保持禁用。
- 眼屏引脚保持 **2/0/41/42/45/46**（模块唯一可用 IO，第三方选型即此）；背板设计见 `hardware/主板设计说明.md` §5。
- 剩余待实机验证：`/stream` 推流、`/capture` 拍照、`/eyes` 表情。

### Step 5：眼睛屏（屏在 CAM 模组上；CAM 侧驱动已实现，待实机验证）

用户已确认（2026-07-31）：**两块** 1.28 寸圆屏接在 ESP32-CAM 模组、由 CAM 控制。接线（CAM 侧）：

| 信号 | 左屏 | 右屏 | 说明 |
|---|---|---|---|
| RST | GPIO46 | GPIO46 | 共享复位 |
| L/R（片选，厂商丝印） | GPIO2 | GPIO0 | 每屏独立，等效 CS；模块仅引出此两脚，无法更换（CS 空闲高，实机验证正常）|
| DC | GPIO41 | GPIO41 | 共享 |
| SDA（MOSI） | GPIO45 | GPIO45 | 共享 |
| SCL | GPIO42 | GPIO42 | 共享 |
| VDD / GND | 3V3 / GND | 3V3 / GND | |

注意：GPIO0/45/46 为 strapping 引脚（GPIO0=启动模式、GPIO45=VDD_SPI 电压、GPIO46=ROM 日志），上电瞬间电平需兼容（CS 空闲高、SCL 空闲低一般无碍，实机验证）。

- 主控侧 `EYE_DISPLAY_ENABLED=0` 是**终态**；`config.h` 的 `DISPLAY_SPI_*` 占位值（旧接线，与新硬件冲突）不再等待更新。
- ✅ **CAM 侧驱动已实现（2026-07-31 初版，2026-08 随板型确认更新）**：`wall-e_esp32_cam/cam_main.cpp`——两块圆屏共享 SPI（SCK=42/MOSI=45/DC=41/RST=46），片选 L=**2**/R=**0**，按 **GC9A01 假设**驱动（若黑屏换 ST7789 或查厂商资料）；矢量眼睛绘制复用 archive 固件（lens/iris/pupil/highlight + 2.5–6s 随机眨眼）；新增 HTTP 端点 **`/eyes?expr=neutral|sad|left|right`**（词汇与主控 `self.walle.eyes` 一致）；UART 表情指令随 Step 4 一并接入。
- SD 卡不可用（模块未引出 SD 引脚，见 Step 4.5），`CAM_SD_ENABLED` 保持禁用。
- 实机验证清单：烧录后双眼显示 neutral 表情并周期眨眼；`/eyes?expr=sad|left|right` 切换正确；GC9A01 假设不成立时更换驱动 IC 实现。
- 连带影响：`walle_cam_viewer.cc` 的照片预览/AVI 回放在主控侧无显示出口（NoDisplay），功能空转；待 CAM 侧显示方案明确后决定去留（可由 CAM 屏显示，或 Web 面板查看）。

### Step 6：文档同步（✅ 已完成，2026-07-31）

- 已更新 `docs/XIAOZHI_MIGRATION.md`：§4 引脚表全量换新、§1/§7/§7.1/§8/§9 同步（UART 预留未实现、CAM 板型待核对、眼屏在 CAM 模组、ST7789 独立总线、USB 串口禁用原因）。
- 已更新 `AGENTS.md`：眼屏在 CAM 模组（主控侧禁用）、SD 引脚 XIAO 预设待核对。
- 已更新 `hardware/另一套硬件方案.md` 备注 4（眼屏接 CAM 的项目侧确认）及 `config.h`/`walle_board.cc`/`camera_pins.h` 相关注释。

## 构建环境备忘（Windows）

Git Bash 直接跑 `idf.py` 会因 `MSYSTEM` 环境变量被 IDF v6 拒绝。本机已验证可用的方式：

```powershell
# wall-e_xiaozhi/_build_check.ps1（未提交，路径本机相关）
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:IDF_PATH = 'C:\esp\v6.0.2\esp-idf'
$env:ESP_IDF_VERSION = '6.0'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.2\venv'
$env:PATH = 'C:\Espressif\tools\python\v6.0.2\venv\Scripts;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;' + $env:PATH
Set-Location 'D:\OpenSource\walle-replica\wall-e_xiaozhi'
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' 'C:\esp\v6.0.2\esp-idf\tools\idf.py' build
```

在 PowerShell 里直接执行官方 `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` 再 `idf.py build` 也可以（不要在 Git Bash 里套）。
