# 主控 ESP32-S3 ↔ ESP32-S3-CAM 交互协议（CAM_PROTOCOL v1）

> 本文档定义小智主控（`wall-e_xiaozhi/`，ESP32-S3 N16R8）与摄像头模组（`wall-e_esp32_cam/`，通用 ESP32-S3-CAM，OV3660）之间 UART 链路的通信协议。
> 链路接线按 `hardware/另一套硬件方案.md`：主控 TX=GPIO9 → CAM RX(GPIO14)，主控 RX=GPIO10 ← CAM TX(GPIO21)（CAM 侧引脚以 `docs/NEW_HARDWARE_MIGRATION.md` Step 4.5 板型核对为准）。
> **状态：规范已定稿；CAM 侧固件已实现**（`wall-e_esp32_cam/cam_link.h`，未实机验证；主控侧 `walle_cam_link` 已实现，两侧待联调）。

## 0. 设计原则

- **v1 是纯控制通道**：只传命令/状态/表情，不传文件内容。拍照回看由 CAM 在自己的眼睛屏上**本地解码显示**（零传输）；浏览器浏览/下载文件走 CAM 的 HTTP 直连（`/files`、`/file`），不经主控。
- **v1 支持录像控制**（2026-07-31 修订）：`REC START/STOP` 命令 + `EVT REC_DONE` 异常中止事件；录像文件仍不经 UART 传输，浏览器 `/file` 下载观看（眼屏录像回放留 v2）。
- **拍照是完整交互流程**：准备 → 倒计时 3-2-1（眼屏显数字）→ 拍摄 → 眼屏本地回放 → 恢复表情；"茄子"语音提示由主控 TTS 负责。
- **HTTP 全部保留**：CAM 现有 HTTP 端点不删，作为主控的回退通道与浏览器直连入口。
- 行式文本、一命令一响应，调测期可用串口监视器直接手敲。

## 1. 物理层

| 项 | 值 |
|---|---|
|  UART | 主控 UART1（CAM_UART_TX_PIN=9 / CAM_UART_RX_PIN=10）↔ CAM Serial1（TX=21 / RX=14） |
| 波特率 | **115200**，8N1，TTL 3.3V |
| 流控 | 无硬件流控 |
| 线长 | <30cm，共地 |
| 备注 | 现场误码再议降速或 CRC 帧（v2） |

## 2. 帧格式

- 行式 ASCII，`\n` 结尾（`\r` 忽略），单行 ≤ 256 字节，字段以空格分隔。
- **主控是唯一发起方**；CAM 只发响应与异步事件。
- 命令 = 大写动词 + 参数；响应首字段 `OK` / `ERR`；异步事件首字段 `EVT`。
- 一命令一响应（`LIST` 为多行块例外）；未知命令回 `ERR UNKNOWN_CMD`。

## 3. 命令表（主控 → CAM）

| 命令 | 参数 | 响应 | 说明 |
|---|---|---|---|
| `HELLO` | `<proto_ver>`（当前 1） | `OK CAM <fw_ver> <proto_ver>` | 建链/版本握手，双方取 min 版本 |
| `PING` | — | `OK PONG` | 链路探测/保活（可选） |
| `STATUS` | — | `OK sd=<0\|1> busy=<0\|1> rec=<0\|1> expr=<name> ip=<ip> rssi=<-dBm> uptime=<s>` | 状态快照 |
| `EYES` | `neutral\|sad\|left\|right` | `OK EYES <expr>` | 眼屏表情（词汇与主控 `self.walle.eyes` 一致） |
| `PHOTO` | — | `OK FILE <path>` / `ERR SD/BUSY …` | 拍照全流程（见 §4）；拍摄完成后即响应，回放由 CAM 自行收尾 |
| `REC` | `START` / `STOP` | `OK REC ON <path>` / `OK REC OFF <path> <frames>` | 录像控制（MJPEG→AVI 存 `/videos/`）；录像中眼屏保持表情（可选叠加 REC 红点） |
| `SHOW` | `<path>` / `LATEST` | `OK SHOW <path>` / `ERR NOENT` | 眼屏回放指定/最新照片（默认 5s 后恢复表情；仅照片，录像回放留 v2） |
| `ABORT` | — | `OK ABORT` | 中止进行中的拍照流程或回放，恢复表情 |
| `LIST` | `photos [max]` | 多行块：`OK BEGIN <n>` → n 行 `F <path> <size>` → `OK END` | 调试/文件浏览（可选实现） |
| `WIFI_CREDS` | `<url_encoded_ssid> <url_encoded_password>` | `OK WIFI <ip>` / `ERR WIFI <code>` | 主控推送 WiFi 凭证（见 §4.2） |

## 4. PHOTO 拍照流程（CAM 侧状态机）

主控发 `PHOTO` 后，CAM 自动执行以下序列（期间对第二个 `PHOTO` 回 `ERR BUSY`）：

| 阶段 | 时长 | 眼屏内容 | 说明 |
|---|---|---|---|
| PREPARE 准备 | ~0.5s | 注视/中性表情（双眼睁开） | 等帧率/曝光稳定 |
| COUNTDOWN 倒计时 | 3s | 依次显示大号数字 **3 → 2 → 1**（1s/个） | "茄子"语音由**主控 TTS** 同步说出（MCP 工具返回提示 LLM；眼屏不渲染中文，免字库） |
| CAPTURE 拍摄 | <1s | 保持最后一帧倒计时或瞬间黑屏提示 | 抓帧存 `/photos/IMG_nnnn.jpg`；**完成后立即回 `OK FILE <path>`** |
| PREVIEW 回放 | 5s | 双眼屏解码显示刚拍的照片 | CAM 本地 JPEG 解码（帧缓冲已在 PSRAM），零传输 |
| RESTORE 恢复 | — | 恢复原表情 | 自动 |

- 录像进行中收到 `PHOTO` 回 `ERR BUSY`（摄像头被录像任务占用）；拍照流程进行中收到 `REC START` 同样回 `ERR BUSY`。
- 主控 `PHOTO` 响应超时：**8s**（覆盖准备+倒计时+拍摄）。
- 回放期间收到 `ABORT` 或任意 `EYES` → 立即中断回放并恢复表情。
- 主控侧时序：发 `PHOTO` 后由 MCP 工具返回值引导 LLM 口播"3、2、1，茄子"（语音与屏显倒计时近似同步即可，无需精确对齐）。

### 4.2 WIFI_CREDS WiFi 凭证同步（2026-08-09 新增）

主控连上 WiFi 后自动将当前 SSID/密码推送给 CAM，CAM 存入 NVS 并联网。双方共享同一 WiFi 后，MJPEG 推流和 HTTP 回退 API 可通过局域网直连。

**同步时机**：
1. CAM 上电 → 发 `EVT BOOT` → 主控 `HELLO` 握手完成后推送
2. 主控端重新配网（用户修改 WiFi）→ 连接成功后推送
3. CAM 重启后从 NVS 读取上次保存的凭证自动连接，无需等待推送

**命令格式**：
```
>> WIFI_CREDS <url_encoded_ssid> <url_encoded_password>
<< OK WIFI <ip_address>
<< ERR WIFI AUTH_FAIL  (连接失败)
<< ERR BAD_ARG          (参数缺失/SSID为空)
```

- SSID 和密码分别做 URL-encode（空格 → `%20`、特殊字符 → `%XX`），避免行内空格截断
- CAM 收到后：存入 NVS → 断开当前 WiFi → 连接新 AP → 返回 IP
- 响应超时：**15s**（覆盖断开+重连流程）

**CAM 侧 NVS 存储**：
- namespace `"wifi"`，keys `"ssid"` / `"password"`（与主控 `SsidManager` 命名一致）
- 上电时优先从 NVS 加载凭证连接，无凭证则等待 UART 推送
- **不再因 WiFi 连接失败而重启**——等待主控推送正确凭证

## 5. 异步事件（CAM → 主控）

| 事件 | 参数 | 说明 |
|---|---|---|
| `EVT BOOT` | `<proto_ver>` | CAM 上电/重启后主动发一次；主控收到后重发 `HELLO` 并刷新状态 |
| `EVT REC_DONE` | `<path> <frames>` | 录像**异常中止**（SD 满/写失败/连续丢帧）时上报；正常 `REC STOP` 的响应即 `OK REC OFF`，不重复发事件 |
| `EVT ERR` | `<code> <msg>` | 异步错误（**预留，v1 不实现**） |

## 6. 错误格式与码

`ERR <CODE> [msg]`，码表：

| 码 | 含义 |
|---|---|
| `UNKNOWN_CMD` | 未知命令（主控应降级忽略该能力） |
| `BAD_ARG` | 参数非法 |
| `BUSY` | 拍照流程进行中（或 SHOW 回放中收到 PHOTO、录像中收到 PHOTO） |
| `SD` | SD 卡不可用 |
| `NOENT` | SHOW 指定路径不存在 / 无照片 |
| `STATE` | 状态不允许的操作（如未录像收到 `REC STOP`、录像中重复 `REC START`） |
| `IO` | 摄像头/传感器故障 |
| `WIFI` | WiFi 操作失败（认证失败/超时等，后跟原因如 `AUTH_FAIL`） |

## 7. 超时与重试

| 命令 | 响应超时 | 重试 |
|---|---|---|
| 控制类（HELLO/PING/STATUS/EYES/ABORT/LIST） | 500ms | 1 次 |
| `SHOW` | 1s | 1 次 |
| `REC START` | 1s | 1 次 |
| `REC STOP` | 2s（flush AVI 索引） | **不重试** |
| `PHOTO` | 8s | **不重试**（防重复拍摄） |
| `WIFI_CREDS` | 15s | 1 次 |

- 超时计一次链路故障；连续 3 次判链路 down，主控回退 HTTP 调用，并周期性 `PING` 探测恢复。

## 8. 并发与互斥

- 主控侧串行化（互斥锁）：任一时刻最多一条在途命令；`LIST` 块接收期间不接受新命令。
- CAM 侧命令处理不阻塞 HTTP 服务：拍照流程跑在状态机/独立任务里，不卡 `/stream`。

## 9. 版本策略与 v2 预留

- `proto_ver` 单调递增整数；新命令只增不改；低版本主控收到 `ERR UNKNOWN_CMD` 降级。
- v2 预留方向（本文档仅列出，不定格式）：
  - `PLAY <path|LATEST>`：录像在眼屏本地回放（逐帧 JPEG 解码）；
  - `READ <path> <off> <len>`：二进制文件读取（服务云端多模态看图）；
  - 提速 460800/921600、CRC 帧。

## 10. 与 HTTP 的并存/回退

CAM 的 HTTP 端点全部保留：

| 端点 | 用途 |
|---|---|
| `/stream` | 浏览器 MJPEG 预览（必须走 Wi-Fi） |
| `/capture`、`/record`、`/files`、`/file` | 浏览器直连 + 主控 HTTP 回退 |
| `/eyes` | 浏览器/调试直连表情切换 |

主控封装 `CamLink` 层：**UART 优先、故障回退 HTTP**，调用方（MCP 工具 / Web 路由）无感。

## 11. 调用点映射（实现期对照表）

| 调用方 | 动作 | 协议映射 |
|---|---|---|
| `self.walle.camera`（MCP） | `photo` | `PHOTO`（工具返回提示 LLM 口播"3、2、1，茄子"） |
| | `preview` | `SHOW LATEST` |
| | `stop` | `ABORT` |
| | `record_start` / `record_stop` | `REC START` / `REC STOP`（异常中止由 `EVT REC_DONE` 上报） |
| | `replay` | 眼屏暂不支持录像回放（留 v2 `PLAY`；可引导浏览器 `/file` 下载播放） |
| `self.walle.eyes`（MCP） | `neutral/sad/left/right` | 现有舵机表情（i/j/k/l）保留，**追加** `EYES <expr>` 下发 CAM（机械眼 + 屏幕眼同步） |
| Web 面板摄像头区 | Preview / Stop view | `SHOW LATEST` / `ABORT` |
| `walle_cam_viewer.cc` | — | 维持现状（HTTP 拉文件、无显示出口），CAM 本地回放接管后退役（实现期决策） |

## 12. 示例会话

```
<- EVT BOOT 1
-> HELLO 1
<- OK CAM 1.1.0 1
-> STATUS
<- OK sd=1 busy=0 rec=0 expr=neutral ip=192.168.4.3 rssi=-52 uptime=61
-> EYES sad
<- OK EYES sad
-> PHOTO
   （CAM 眼屏：准备 → 3 → 2 → 1 → 拍摄；主控 TTS 同步口播"3、2、1，茄子"）
<- OK FILE /photos/IMG_0007.jpg
   （CAM 眼屏继续回放照片 5s 后自动恢复 sad 表情）
-> REC START
<- OK REC ON /videos/VID_0003.avi
-> REC STOP
<- OK REC OFF /videos/VID_0003.avi 412
-> SHOW LATEST
<- OK SHOW /photos/IMG_0007.jpg
-> ABORT
<- OK ABORT
-> LIST photos 3
<- OK BEGIN 2
<- F /photos/IMG_0006.jpg 38122
<- F /photos/IMG_0007.jpg 40217
<- OK END
```

## 13. 测试方法

- **CAM 侧**：USB-TTL 对测（或 Arduino 串口监视器模拟主控）逐条发命令，观察眼屏流程与响应文本。
- **主控侧**：临时日志打印收发行；拔掉 UART 线验证 HTTP 回退路径。
- **实机前置**：先完成 `docs/NEW_HARDWARE_MIGRATION.md` Step 4.5，确认 CAM 侧 GPIO21(TX)/GPIO14(RX) 空闲可用。

## 后续任务（实现另起）

1. **CAM 侧**：`cam_link.h`（Serial1 行解析 + 命令分发，复用现有拍照/眼屏函数）+ 拍照状态机（准备/倒计时屏显/拍摄/JPEG 本地解码回放，JPEGDEC 库）——**已完成**（2026-08-01，未实机验证）。
2. **主控侧**：`walle_cam_link.cc/.h`（UART1 收发任务 + 互斥 `Command()` + HTTP 回退）——**已完成**（2026-08-01，未实机验证）。
3. **调用点切换**：`walle_mcp_tools.cc`（camera/eyes 工具）、`walle_web_server.cc`（/camview 与摄像头区按钮）；MCP 工具描述按协议更新（photo 倒计时口播、record_start/stop 走 REC）——**已完成**（2026-08-01）。
4. **实机验证**（依赖 Step 4.5）+ 文档收尾。
