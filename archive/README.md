# Archive - 非主线方案归档

本目录收录仓库中**不再作为主线演进**的归档方案，代码保持可编译、可回退使用。

## arduino-pi/ - 原始方案（Arduino UNO + 树莓派）

上游 chillibasket/walle-replica 的经典架构：

| 目录/文件 | 说明 |
|-----------|------|
| `wall-e/` | Arduino UNO 主控固件（舵机动力学、动画队列、串口协议、电量） |
| `wall-e_calibration/` | UNO 版舵机标定 sketch |
| `web_interface/` | 树莓派 Flask Web 控制端（虚拟摇杆、手柄、TTS、Blockly、摄像头推流） |
| `raspi-setup.sh` | 树莓派一键安装/自启脚本 |

状态：**维护中**（跟随上游），使用文档见 `README.md` / `README.zh-CN.md` 的安装说明章节。

## 已移除：wall-e_esp32/ - ESP32-S3 单机版

原 ESP32-S3 单机版固件（内置 Web 控制端、PCM5102 I2S 音频、蓝牙手柄、GC9A01×2 眼睛 + ST7789 状态屏，可脱离树莓派）已于 2026-08 移除：该方案自小智主线（`../wall-e_xiaozhi/`）落地后即冻结，已无回退价值。其舵机标定 sketch 保留并移至仓库主目录 `../wall-e_esp32_calibration/`（小智主线标定仍用这份）。

> ESP32-S3-CAM 推流固件（`wall-e_esp32_cam/`）因与小智主线配套使用，**不在归档内**，位于仓库主目录。

## 主线

当前主线为 **小智单 MCU 语音方案**（`../wall-e_xiaozhi/`），迁移文档：`../docs/XIAOZHI_MIGRATION.md`。
