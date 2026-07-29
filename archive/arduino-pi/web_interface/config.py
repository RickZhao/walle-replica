"""
Wall-E Web Interface - Flask APP configuration.
"""

import os
BASEDIR = os.path.abspath(os.path.dirname(__file__))

# Secret key used for login session cookies
SECRET_KEY = b'\xccCL\xb2&S\xcb\xfa&\x0e\x90\x03\xe7h5\x0f\x1e\r\xef\xd6 2\x05&'

# Web Interface Settings
APP_PORT = 5000                                         # Port of the application
APP_DEBUG = False                                       # Enable / Disable Python Server Debugging
LOGIN_PASSWORD = "walle"                                # Password for web-interface
ARDUINO_PORT = "/dev/ttyACM0"                           # Default port which will be selected
AUTOSTART_ARDUINO = True                                # False = no auto connect, True = automatically try to connect to default port
AUTOSTART_CAM = True                                    # False = no auto start, True = automatically start up the camera
SOUND_FOLDER = os.path.join(BASEDIR, "static/sounds/")  # Location of the folder containing all audio files
ESPEAK_CMD = ['espeak-ng', '-v', 'en', '-b', '1']       # ESpeak Command and Language
RB_CMD = ['rubberband', '-t', '1.1', '-p', '2', '-c', '6', '-f', '1.8', '-q']  # Rubberband for pitch shifting TTS
AUDIOPLAYER_CMD = ['aplay']                             # Command for local audioplayer
SOUND_FORMAT = "wav"                                    # Audio file format

# Values for Codeblock Movement
CODEBLOCK_MOTORPOWER = 0.8   # Motorpower at which the speed below is reached
CODEBLOCK_MOTORSPEED = 17    # this WALLE_E drives at 17 cm/s at given MOTORPOWER
CODEBLOCK_TURNPOWER = 0.5    # Motorpower for the turn movement
CODEBLOCK_TURNTIME = 1.8     # the time (s) it takes to move 90° at given TURNPOWER

# ---------------------------------------------------------------
# Gamepad (Python-side, pygame.joystick)
#   后台线程在树莓派上直接读 /dev/input/js0，把状态转成与前端
#   摇杆相同的串口指令下发，Arduino 端零改动。与浏览器端手柄
#   (static/js/main.js) 双轨并存：手柄插在树莓派上走本模块，插在
#   访问网页的设备上走浏览器端。两者都调 arduino.send_command，
#   后发覆盖先发。
#   headless 部署：pygame 用 SDL_VIDEODRIVER=dummy 初始化，不依赖桌面。
#   权限：运行用户须在 input 组（raspi-setup.sh 已加 usermod -aG input）。
# ---------------------------------------------------------------
GAMEPAD_ENABLED = True                       # False = 不启动手柄后台线程
GAMEPAD_AUTOSTART = True                     # True = app 启动时自动启动手柄线程
GAMEPAD_POLL_INTERVAL = 0.02                  # 轮询事件间隔（秒），20ms 更跟手；仅值变化时才 send_command
GAMEPAD_DEADZONE = 0.2                       # 摇杆死区，绝对值小于此值视为 0（中点发 X0/Y0 归零，防电机不释放）
GAMEPAD_HEAD_MULTIPLIER = 5                  # 右摇杆 -> 头部旋转增量倍率（与 main.js headMultiplier 一致）
GAMEPAD_NECK_MULTIPLIER = 5                  # 右摇杆 -> 颈部增量倍率（与 main.js 一致）
GAMEPAD_ARMS_MULTIPLIER = 6                   # LT/RT -> 手臂增量倍率（与 main.js armsMultiplier 一致）
