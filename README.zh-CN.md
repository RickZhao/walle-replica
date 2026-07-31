[![GPLv3 License](https://img.shields.io/badge/License-GPL%20v3-yellow.svg)](https://opensource.org/licenses/)
[![Issues](https://img.shields.io/github/issues-raw/chillibasket/walle-replica.svg?maxAge=25000)](https://github.com/chillibasket/walle-replica/issues)
[![GitHub last commit](https://img.shields.io/github/last-commit/chillibasket/walle-replica.svg?style=flat)](https://github.com/chillibasket/walle-replica/commits/master)

# Wall-E 机器人复刻版
Wall-E 复刻版机器人的控制代码与控制器代码。关于该机器人的更多信息，请访问 https://wired.chillibasket.com/3d-printed-archive/arduino-pi/wall-e/

<br />

> **本仓库（fork）在上游基础上新增了三套固件与自制硬件：**
>
> | 目录 | 框架 | 说明 |
> |------|------|------|
> | `wall-e_xiaozhi/` | ESP-IDF | **主线**：单块 ESP32-S3 语音机器人（小智方案：唤醒词 + 云端 LLM + MCP 动作工具 + Web 控制面板 + ML307A 4G 回退；眼睛屏在 CAM 模组上，主控侧禁用），详见 `docs/XIAOZHI_MIGRATION.md` |
> | `archive/wall-e_esp32/wall-e_esp32/` | Arduino (ESP32-S3) | ESP32 单机版：内置 Web 控制端、I2S 音频、蓝牙手柄、双 GC9A01 眼睛 + ST7789 状态屏。**已冻结**（保留作回退） |
> | `wall-e_esp32_cam/` | Arduino (ESP32-S3-CAM) | 第二块 ESP32-S3-CAM 的 MJPEG 推流固件 |
> | `hardware/walle-shield/` | KiCad 10 | 模块化背板 PCB，替代杜邦线飞线 |
> | `hardware/` | — | 第三方 ESP32-S3 方案 BOM 分析（`另一套硬件方案.md`） |
>
> 下文的原始方案（Arduino UNO `archive/arduino-pi/wall-e/` + 树莓派 `archive/arduino-pi/web_interface/`）仍在维护。中文技术文档见 `docs/`（串口协议、接线、硬件清单、小智迁移记录等）。

<br />
<br />

## 1. Arduino 代码（wall-e）
控制机器人电机与舵机的主程序。功能包括：
1. 动画队列，记录机器人下一步需要执行的舵机动作。
1. 随机动作生成器，使机器人能够自主运动并显得生动。
1. 所有舵机的速度控制，实现平滑的加速与减速。
1. 非阻塞式串口解析，便于对机器人进行远程遥控。
1. 使用分压电路监测电池电量。
<br />

## 2. 树莓派 Web 服务器（web_interface）
Web 界面使用 Python 编写，基于 *Flask* 搭建服务器。树莓派通过 USB 连接到 Arduino 微控制器。主要功能有：
1. 一个 JavaScript 摇杆，可轻松控制机器人移动。
1. 所有舵机的手动控制。
1. 一组可供机器人执行的动作动画。
1. 一组可播放的音效。
1. 设置页面，可修改电机参数、音量和视频选项。
1. 手柄支持；在任何现代浏览器上，均可使用连接的 Xbox 或 PlayStation 手柄控制机器人。
1. 简单的登录页，防止任何人都能访问控制界面（注意：这并非完整的访问控制系统，请勿在不可信/公共网络上使用本 Web 界面）。
1. **[新]** 支持文本转语音（TTS）。
1. **[新]** 在浏览器内使用 CodeBlocks 拖拽编辑器对机器人动作进行编程。

![](/images/wall-e_webinterface1.jpg)
*Web 界面与机器人示意图*

<br />
<br />


## 安装说明

### 1. Arduino

#### [a] 基础安装
1. 确保电子元件的接线与下方电路图一致。
1. 从 GitHub 仓库下载/克隆 "wall-e" 文件夹。
1. 在 Arduino IDE 中打开 `wall-e.ino`；`animations.ino`、`MotorController.hpp` 和 `Queue.hpp` 会自动在 IDE 的不同标签页中打开。
1. 安装 `Adafruit_PWMServoDriver.h` 库
    1. 进入 工具 → 包含库 → 管理库…（Sketch -> Include Library -> Manage Libraries...）
    1. 搜索 *Adafruit Servo*。
    1. 安装最新版本。
1. 用 USB 线将电脑连接到微控制器，在 *工具* 菜单中选择正确的 *开发板* 和 *端口*。
1. 将程序上传到微控制器。

![](/images/wall-e_wiring_diagram.jpg)
*机器人电子元件接线图*
<br />

> **使用 TB6612FNG 电机板**：上图默认对应 Arduino Motor Shield Rev2。若使用 TB6612FNG，请在 `wall-e.ino` 中取消注释 `#define MOTOR_DRIVER_TB6612FNG`，并用 D8/D9 作为第二路方向脚接 TB6612FNG 的 AIN2/BIN2，无需 74HC04 反相器。详细接线见 `docs/WIRING.md`。

> **新增：模块化底板 PCB** — 仓库现在提供完整的 KiCad 10 底板设计：`hardware/walle-shield/`。用一块 140×100 mm 的双层板替代大量杜邦线，只需焊接排母、端子、开关、保险丝和几颗电阻，然后把 Arduino、TB6612FNG 模块、PCA9685 模块、降压模块直接插上即可。Gerber、钻孔文件、BOM 已导出。详见 `hardware/walle-shield/README.md`。

<br />

#### [b] 测试主程序
1. 程序上传到 Arduino 后，在微控制器仍连接电脑的情况下给 12V 电池上电。
1. 打开 *串口监视器*（Arduino IDE 右上角按钮），波特率设为 115200。
1. 控制机器人移动：发送字符 'w'、'a'、's'、'd' 分别前进、左转、后退、右转；发送 'q' 停止所有移动。
1. 控制头部：发送 'j'、'l'、'i'、'k' 分别让头部左右倾斜、眼睛上下转动。此时舵机可能会超出应有限位并显得不协调，这可通过下方的舵机标定步骤解决。

<br />

#### [c] 舵机标定
1. 从 GitHub 仓库下载/克隆 "wall-e_calibration" 文件夹。
1. 在 Arduino IDE 中打开 `wall-e_calibration.ino`。
1. 上传程序到微控制器，打开串口监视器，波特率设为 115200。
1. 该程序用于标定每个舵机在期望行程内所需的最大和最小 PWM 脉冲长度。各舵机标准的 LOW 和 HIGH 位置可参见[作者网站上的](https://wired.chillibasket.com/3d-printed-archive/arduino-pi/wall-e/)示意图。
1. 启动程序并打开串口监视器后，2–3 秒后会提示已准备好标定第一个舵机（头部旋转）的 LOW 位置。
1. 发送字符 'a' 和 'd' 让电机分别后退/前进 -10 和 +10；如需精细控制，用 'z' 和 'c' 移动 -1 和 +1。
1. 电机到位后，发送字符 'n' 进入下一步，标定同一舵机的 HIGH 位置，随后对 7 个舵机依次重复该过程。
1. 全部标定完成后，程序会向串口监视器输出包含标定值的数组。
1. 复制该数组，粘贴到 *wall-e.ino* 程序的[第 144 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/wall-e/wall-e.ino#L144)至 150 行之间。数组格式如下：
    ```cpp
    int preset[][2] =  {{410,120},  // head rotation
                        {532,178},  // neck top
                        {120,310},  // neck bottom
                        {465,271},  // eye right
                        {278,479},  // eye left
                        {340,135},  // arm left
                        {150,360}}; // arm right
    ```

<br />

#### [d] 电池电量检测（可选）
使用电池供电时，跟踪剩余电量很重要。某些电池过放会损坏；若供电不足，树莓派的 SD 卡可能损坏。
1. 要在 Arduino 上启用电池电量检测，按下图连接电阻与接线。电阻（分压电路）将 12V 电压降至 5V 以下，确保 Arduino 可用模拟引脚安全测量。推荐电阻值为 `R1 = 100kΩ`、`R2 = 47kΩ`。
1. 在主程序 *wall-e.ino* 中取消[第 54 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/wall-e/wall-e.ino#L54) `#define BAT_L` 的注释。
1. 若使用不同的电阻值，按公式 `POT_DIV = R2 / (R1 + R2)` 修改程序第 54 行的分压增益系数。
1. 程序现在会每 10 秒自动检测一次电池电量，并在树莓派 Web 界面的“Status”区域显示。

![](/images/battery_level_circuit.jpg)
<br />
*电池电量检测电路接线图*

<br />

#### [e] OLED 显示屏（可选）（贡献者：[hpkevertje](https://github.com/hpkevertje)）
可集成一块小型 OLED 显示屏，在机器人前方电量指示面板上显示电池电量。该功能需要先启用上一节的电池电量检测电路，屏幕会在每次电量计算后更新。此功能使用 u8g2 显示库的页模式；在 Arduino UNO 上可能出现内存占用过高的警告，可忽略。
1. 要在 Arduino 上启用 OLED 显示功能，将 I2C OLED 屏接到舵机驱动板的 I2C 总线上（见图）。
1. 在 Arduino 库管理器中安装 U8g2 库：
    1. 进入 工具 → 包含库 → 管理库…（Sketch -> Include Library -> Manage Libraries...）
    1. 搜索 *U8g2*，发布者为 "oliver"。
    1. 安装最新版本。
1. 在主程序 *wall-e.ino* 中取消[第 74 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/wall-e/wall-e.ino#L74) `#define OLED` 的注释。
1. 若使用库支持的其他显示屏，可按[库参考页](https://github.com/olikraus/u8g2/wiki/u8g2setupcpp#constructor-reference)修改[第 78 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/wall-e/wall-e.ino#L78)的构造函数。默认构造针对 SH1106_128X64_NONAME 显示屏。

![](/images/oLed_circuit.jpg)
<br />
*OLED 显示屏接线图*

<br />

#### [f] 添加自定义舵机动画（可选）
我的代码自带两段还原电影的动画：Wall-E 开机时的眼睛动作，以及 Wall-E 好奇地四处张望的一组动作。从 2.7 版起，我让你能更方便地添加自定义舵机动画，让 Wall-E 做出其他动作……
1. 打开 `animations.ino`（与主程序在同一文件夹）。
1. 每条动画指令由所有舵机的目标位置和等待下一条指令的时间组成。
1. 在 switch 语句中插入一个新的 `case` 即可添加新动画，放在 `default` 之前。例如：
    ```cpp
    case 3:
            // --- Title of your new motion sequence ---
            //          time,head,necT,necB,eyeR,eyeL,armL,armR
            queue.push({  12,  48,  40,   0,  35,  45,  60,  59});
            queue.push({1500,  48,  40,  20, 100,   0,  80,  80});
            // Add as many additional movements here as you need to complete the animation
            // queue.push({time, head rotation, neck top, neck bottom, eye right, eye left, arm left, arm right})
            break;
    ```
1. 时间以毫秒为单位（例如 3.5 秒 = 3500）。
1. 舵机位置指令为 0 到 100 之间的整数，其中 `0 = LOW`、`100 = HIGH`，由 `wall-e_calibration.ino` 标定。
1. 若要某次动作跳过某个电机，可用 -1。

<br />
<br />


### 2. 树莓派 Web 服务器

#### [a] 硬件连接
1. 将树莓派电源线接到 12V 转 5V 降压模块的 USB 输出口。
2. 用 USB 数据线连接 Arduino 与树莓派。
3. 若有树莓派摄像头，将排线插入 CSI 摄像头接口。
4. 安装配置阶段，可给 HDMI 口接显示器、接 USB 键盘鼠标；也可以在另一台电脑上[通过 SSH](https://www.raspberrypi.com/documentation/computers/remote-access.html#ssh) 连接并配置树莓派。

<br />

#### [b] 基础安装
1. 给树莓派安装最新版 Raspberry Pi OS 桌面版，安装说明见[树莓派官网](https://www.raspberrypi.com/documentation/computers/getting-started.html)。确保树莓派已联网。
2. 在树莓派上打开“终端”。
3. 将仓库克隆到树莓派主目录：
```shell
cd ~
git clone https://github.com/chillibasket/walle-replica.git
```

<br />

> [!NOTE]
> 可通过编辑 "config.py" 配置 Web 界面设置：
> * 打开配置文件：`nano ~/walle-replica/archive/arduino-pi/web_interface/config.py`
> * [第 14 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L14)可修改 Web 界面密码，默认密码为 "walle"。
> * [第 15 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L15)可设置连接 Arduino 的默认串口，用 `dmseg | grep tty` 命令可列出所有已连接的串口。
> * [第 16 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L16)和[第 17 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L17)可配置启动 Web 服务器时是否自动连接 Arduino 和摄像头。

<br />

4. 配置完成后，运行安装脚本，它会自动安装所有依赖库（注意——这可能需要一些时间）：
```shell
cd ~/walle-replica
sudo chmod +x ./archive/arduino-pi/raspi-setup.sh
sudo ./archive/arduino-pi/raspi-setup.sh
```

<br />

#### [c] 使用 Web 服务器
1. 安装成功后，树莓派开机时 Web 服务器会自动启动，通过 [Systemd 服务](https://learn.sparkfun.com/tutorials/how-to-run-a-raspberry-pi-program-on-startup/all#method-3-systemd)实现。
1. 在树莓派上，浏览器访问 http://localhost:5000 即可打开 Web 界面。
1. 要在同一 WiFi 网络内的其他电脑上访问，先用命令 `hostname -I` 查看树莓派的 IP 地址。
1. 在同网络内任意电脑/设备上打开浏览器，输入树莓派 IP 加 `:5000` 即可，例如 `192.168.1.10:5000`。
1. 控制机器人前，需确保与 Arduino 的串口通信已启动。进入 Web 界面的 `Settings` 标签页，在下拉列表选择正确串口并点 `Reconnect`。若配置正确，此过程会自动完成。

<br />

> [!TIP]
> 控制 Web 界面的常用命令：
> * 停止 Web 界面自启服务：`sudo systemd stop walle.service`
> * 禁用开机自启：`sudo systemd disable walle.service`
> * 重新启用开机自启：`sudo systemd enable walle.service`
> * 服务停止后再次启动：`sudo systemd start walle.service`
> * 查看服务状态与错误：`sudo systemd status walle.service`
> * 如需手动从终端运行 Web 服务器（例如排查错误）：`python3 ~/walle-replica/archive/arduino-pi/web_interface/app.py`，按 `CTRL + C` 停止。

<br />

#### [d] 使用 Blocky 控制机器人（贡献者：[dkrey](https://github.com/dkrey)）
自 3.0 版起，Web 界面新增一个标签页，可用拖拽脚本语言控制机器人。从左侧栏拖动想要的动作到编辑区即可，例如驱动 Wall-E、控制执行器、播放音频。这是让孩子在玩中学习编程基础的好方式！

驱动电机的命令可能需要调整 "config.py" 底部[第 25 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L25)至[第 28 行](https://github.com/chillibasket/walle-replica/blob/master/archive/arduino-pi/web_interface/config.py#L28)的参数，以确保速度和转向量正确。

<br />

#### [e] 添加摄像头视频流（可选）

Web 服务器自动支持任何通过排线连接到树莓派 CSI 接口的摄像头。遗憾的是，本系统暂不支持 USB 摄像头，未来可能重新加入支持。

<br />

#### [f] 添加新音效（可选）
1. 默认情况下，树莓派会自动选择从 HDMI 口还是耳机口输出音频。可用以下命令强制始终使用耳机口：`amixer cset numid=3 1`
1. 确保所有音效文件为 `*.wav` 格式，大多数音乐/音频编辑器都能转换。
1. 文件名按以下格式命名：`[组名]_[文件名]_[时长毫秒].wav`，例如 `voice_eva_1200.wav`。Web 界面会按“组名”分组并按字母排序显示。
1. 将音效文件上传到树莓派：`~/walle-replica/archive/arduino-pi/web_interface/static/sounds/`
1. 刷新页面后文件应出现在 Web 界面。若未出现，可能需要修改文件夹权限：`sudo chmod -R 755 ~/walle-replica/archive/arduino-pi/web_interface/static/sounds`

<br />

#### [g] 将树莓派配置为 WiFi 热点 *（可选）*
若想在户外或展会控制机器人，可能没有可用的安全 WiFi。为此，树莓派可广播自己的 WiFi 网络，控制用的电脑/手机/平板直接连到该网络即可。

配置 WiFi 热点使用 [RaspAP 项目](https://raspap.com/)，它处理所有配置与工具。以下步骤基于其快速安装指南：

1. 更新 Raspbian、内核和固件（然后重启）：
    ```
    sudo apt-get update
    sudo apt-get dist-upgrade
    sudo reboot now
    ```
1. 在 raspi-config 的 Localisation Options 中设置正确的 WiFi 国家：`sudo raspi-config`
1. 运行快速安装器：`curl -sL https://install.raspap.com | bash`
    1. 安装过程中出现的前几个 yes/no 提示，输入 “y”（是）接受所有推荐设置。最后两个提示（Ad Blocking 和下一个）非必需，输入 “n”（否）。
1. 再次重启树莓派使配置生效：`sudo reboot`
1. 此时树莓派应广播一个 WiFi 网络，信息如下：
    1. SSID（WiFi 名）：`raspi-webgui`
    1. 密码：`ChangeMe`
1. 用电脑/手机/平板连上该 WiFi 后，在浏览器输入此地址打开 Wall-E Web 界面：`http://10.3.141.1:5000`
1. （推荐）修改 WiFi 名和密码：访问 `http://10.3.141.1` 的 WiFi 配置网页。默认用户名 `admin`、密码 `secret`。
    1. 点击左侧栏的 “Hotspot”，在 “Basic” 标签改 WiFi 名，在 “Security” 标签改 WiFi 密码。
    1. 修改管理 WiFi 设置界面的管理员密码，点击界面右上角的 “Admin” 图标。

<br />
<br />


## 更新日志

#### 2024 年 6 月 9 日（版本 3.0）
1. 对 Python 代码进行重大修订，遵循最佳实践，提升健壮性。
1. 新增文本转语音，并在 Web 界面集成 Blocky 脚本（感谢 [dkrey](https://github.com/dkrey) 贡献）。
1. 摄像头推流改用 PiCamera2，旧推流器已无法工作。
1. 新增安装脚本，让配置更快更简单。

#### 2021 年 10 月 31 日（版本 2.92）
1. 在 *app.py* 中新增选项，首次打开 Web 界面时自动连接 Arduino 并启动摄像头推流。

#### 2021 年 10 月 30 日（版本 2.91）
1. 摄像头推流端口从 8081 改为 8080，与启停脚本的修改保持一致。
1. 改进摄像头配置说明，显式列出所有步骤，不再链接到外部仓库。

#### 2021 年 5 月 29 日（版本 2.9）
1. 改进代码注释和变量/参数命名，便于理解。
1. 修复电机死区和微调参数相关的 bug。

#### 2021 年 3 月 21 日（版本 2.8）
1. 更新 OLED 显示屏使用说明。

#### 2020 年 12 月 19 日
1. 更新树莓派 WiFi 热点配置说明。
1. 新增离线 Lato 字体文件，防止断网时运行 Web 界面报错。

#### 2020 年 8 月 7 日（版本 2.7）
1. 在主程序和舵机标定程序中加入舵机缓启动功能，防止开机时舵机剧烈跳动。
1. 将动画队列的数据类型改为更省动态内存的类型；同时更新 Queue 类，使缓冲区内存可全局声明，编译器能跟踪队列实际占用内存。
1. 将预设舵机动画移到单独文件，便于添加自定义动画。

#### 2020 年 6 月 20 日
1. 修复 `Queue.hpp` 和 `MotoController.hpp` 类的若干 bug。
1. 统一代码注释风格。

#### 2020 年 2 月 16 日
1. 新增手柄支持；任意手柄（如 Xbox、PlayStation 手柄）均可用于操控机器人。
1. 改进串口处理；设置标签页以下拉菜单列出所有可用串口。
1. 视频流下方新增状态指示灯，显示 Arduino/手柄是否已连接。
1. 在 Arduino 代码和树莓派 Web 界面中新增电池电量检测支持。
1. 修复窗口缩放时虚拟摇杆失效的 bug。
1. 以及其他诸多 bug 修复！

#### 2020 年 1 月 25 日
1. 重构 Web 界面，使其在移动设备上显示更佳，并使用精美图标。
1. 新增手动舵机控制，可在 Web 界面直接单独控制每个舵机。
1. 改进错误处理，错误信息统一以底部弹窗显示。
1. 在 README 中新增开机自启服务器的说明。

#### 2019 年 10 月 31 日
1. 修复音频播放相关的若干 bug。
1. 新增 2 个示例音效文件，确保声音目录被纳入 git。

#### 2019 年 10 月 30 日
1. 新增 *wall-e_calibration.ino*，用于标定舵机的最大和最小脉冲宽度。
1. 更新 *wall-e.ino*，动画预设改用相对坐标而非绝对舵机脉冲宽度。这样可利用舵机标定数据，确保各版本机器人的动作一致。
