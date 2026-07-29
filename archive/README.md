# Archive — 非主线方案归档

本目录收录仓库中**不再作为主线演进**的两套完整方案，代码保持可编译、可回退使用。

## arduino-pi/ — 原始方案（Arduino UNO + 树莓派）

上游 chillibasket/walle-replica 的经典架构：

| 目录/文件 | 说明 |
|-----------|------|
| `wall-e/` | Arduino UNO 主控固件（舵机动力学、动画队列、串口协议、电量） |
| `wall-e_calibration/` | UNO 版舵机标定 sketch |
| `web_interface/` | 树莓派 Flask Web 控制端（虚拟摇杆、手柄、TTS、Blockly、摄像头推流） |
| `raspi-setup.sh` | 树莓派一键安装/自启脚本 |

状态：**维护中**（跟随上游），使用文档见 `README.md` / `README.zh-CN.md` 的安装说明章节。

## wall-e_esp32/ — ESP32-S3 单机版（已冻结）

| 目录 | 说明 |
|------|------|
| `wall-e_esp32/` | ESP32-S3 移植版固件：内置 Web 控制端、PCM5102 I2S 音频、蓝牙手柄、GC9A01×2 眼睛 + ST7789 状态屏 |
| `wall-e_esp32_calibration/` | ESP32 版舵机标定 sketch（小智固件标定也用这份） |

状态：**冻结**（2026-07 起），不再演进，保留作树莓派方案回退。注意：固件内的相对路径引用（如 `data/` LittleFS 目录、标定 sketch 与主 sketch 的关系）已随归档目录迁移，使用时按 `archive/wall-e_esp32/` 下的实际位置打开。

> ESP32-S3-CAM 推流固件（`wall-e_esp32_cam/`）因与小智主线配套使用，**不在归档内**，位于仓库主目录。

## 主线

当前主线为 **小智单 MCU 语音方案**（`../wall-e_xiaozhi/`），迁移文档：`../docs/XIAOZHI_MIGRATION.md`。
