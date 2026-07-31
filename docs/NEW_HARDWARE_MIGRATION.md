# 新硬件方案固件迁移 — 进度与后续计划

> 基于 `hardware/另一套硬件方案.md`（第三方 Wall-E 套件）适配小智固件（`wall-e_xiaozhi/main/boards/walle/`）。
> 本文档记录已完成工作、关键决策与回家后续执行的步骤。每次执行一步，确认后再继续下一步。

## 关键决策（已与用户确认）

1. 直接改 `boards/walle/config.h`，不新建板型（旧硬件接线配置被覆盖）。
2. **保留 4G 回退**：ML307A 仍用 GPIO2/3（新引脚表未占用，BOM 无模组，需要时自行加接）。
3. 摄像头改走 **UART 串口链路**（主控 GPIO9/10 ↔ CAM GPIO14/21），Wi-Fi MJPEG 预览保留。
4. 4 个按键全部实现：BOOT(0)、音量+(38)、音量-(39)、长按重启(41)。
5. 眼睛屏（1.28 寸 GC9A01）接线在第三方文档中缺失，**待用户提供**；固件侧 `EYE_DISPLAY_ENABLED=0` 暂时禁用。

## 新硬件引脚映射（已落地到 config.h）

| 功能 | 引脚 |
|---|---|
| 舵机 I2C | SDA=20, SCL=21, OE 未接 |
| 电机 TB6612 | PWMA=19, AIN1/2=17/18, PWMB=11, BIN1/2=12/13, STBY 接 5V（`MOTOR_STBY_WIRED=0`） |
| 麦克风 INMP441 | SCK=5, WS=4, SD=6 |
| PCM5102 DAC | BCK=15, LCK=16, DIN=7 |
| ST7789 状态屏（独立 SPI2） | SCLK=14, MOSI=47, DC=40, RST=45, BL=42, CS 无（NC） |
| 按键 | BOOT=0, VOL+=38, VOL-=39, 重启(长按)=41 |
| CAM UART | TX=9, RX=10, 115200 |
| 电池 ADC | GPIO1（文档无，待硬件确认分压） |
| 4G ML307A | TX=2, RX=3（保留） |
| 眼睛屏 GC9A01 | **待用户提供**（旧值与新硬件冲突，已禁用） |

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

### Step 4：CAM UART 串口链路（替代 HTTP 控制通道）

- 主控新增 `walle_cam_link.cc/.h`：UART1（TX=9/RX=10，115200），行式文本协议：
  `CAPTURE` / `REC_START` / `REC_STOP` / `LIST` / `READ <path> <offset> <len>`（对应现有 HTTP 端点 `/capture`、`/record`、`/files`、`/file` Range）。
- `wall-e_esp32_cam/wall-e_esp32_cam.ino`：新增 UART 命令模式（CAM 侧 TX=GPIO21/RX=GPIO14），复用现有拍照/AVI 录像/SD 文件逻辑；Wi-Fi `/stream` 推流保留（Web 面板预览仍走 Wi-Fi）。
- 主控侧切换调用方：`walle_mcp_tools.cc`（`self.walle.camera`）、`walle_cam_viewer.cc`（预览/回放拉文件）、`walle_web_server.cc`（`/camview` 与摄像头区按钮）由 HTTP 改为 UART 链路，接口语义不变。
- 验证：`idf.py build`；有硬件后串口拍照 → 眼睛屏预览（待 Step 5）。

### Step 5：眼睛屏引脚（阻塞项 — 等用户提供 1.28 寸圆屏接线）

- 拿到接线后更新 `config.h` 的 `DISPLAY_SPI_*` / `DISPLAY_BACKLIGHT_PIN`，`EYE_DISPLAY_ENABLED` 置 1。
- 注意避开已占用引脚；GPIO48 及 8/35/36/37（N16R8 注意 35-37 被 PSRAM 占用，不可用）候选。
- 候选空闲引脚：GPIO8、GPIO48、GPIO21/20/18 等已被占，需按实际接线排。

### Step 6：文档同步

- 更新 `docs/XIAOZHI_MIGRATION.md` 与 `AGENTS.md`：新引脚、UART 摄像头链路、USB 串口禁用原因、眼睛屏开关。

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
