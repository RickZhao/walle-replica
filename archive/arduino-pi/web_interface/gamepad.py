"""
Wall-E Gamepad Controller (Python-side, pygame.joystick)

后台线程在树莓派上直接读 /dev/input/js0，把手柄摇杆/按键状态
转成与前端摇杆 (static/js/main.js) 相同的串口指令，经现有
ArduinoDevice.send_command 下发。Arduino 端零改动，串口协议不变。

与浏览器端手柄逻辑双轨并存：
  - 手柄插在树莓派上 -> 走本模块（headless，SDL_VIDEODRIVER=dummy）
  - 手柄插在访问网页的设备上 -> 走浏览器端 Gamepad API
两者都调 arduino.send_command，后发覆盖先发。

映射表与 main.js 完全一致，详见 docs/SERIAL_PROTOCOL.md。
"""

import os
import logging
import time
from threading import Event, Thread
from typing import Optional


class GamepadController:
    """Read a gamepad via pygame.joystick and dispatch serial commands to Arduino."""

    # W3C standard gamepad button index -> (action) mapping
    # 与 main.js pressButton() 一一对应
    # 按钮 0..15 见 https://www.w3.org/TR/gamepad/
    BUTTON_MAPPINGS = {
        # 0=A/Cross, 1=B/Circle, 2=X/Square, 3=Y/Triangle -> 眼部表情（单字符）
        0: ("char", "i"),   # 悲伤眼
        1: ("char", "l"),   # 右倾头
        2: ("char", "j"),   # 左倾头
        3: ("char", "k"),   # 中性眼
        # 4=LB, 5=RB -> 升臂；6=LT, 7=RT -> 降臂（轴/增量在轴循环里处理，这里只在按下时置方向）
        8: ("toggle_anime", None),   # Back/Share -> 切换自动/手动舵机模式
        # 10=L3, 11=R3, 14=D-Pad<, 15=D-Pad> -> 见 _handle_button
    }

    def __init__(self, arduino, config: dict):
        """
        :param arduino:  ArduinoDevice 实例（共享发送队列，天然线程安全）
        :param config:   Flask app.config 字典
        """
        self.arduino = arduino
        self.config = config

        self.exit_flag: Event = Event()
        self.thread: Optional[Thread] = None
        self.active: bool = False
        self.connected: bool = False            # 是否有手柄连着（供 /gamepadStatus 轮询）

        # 复刻 main.js 的运行时状态
        self.head_rotation: float = 50.0         # moveHead[0]，0..100
        self.neck: float = 125.0                 # moveHead[1]，0..200（分段映射 T/B）
        self.arm_left: float = 50.0              # moveArms[1]，0..100
        self.arm_right: float = 50.0             # moveArms[3]，0..100

        # 上一帧摇杆值，仅在变化时才下发（避免刷爆串口队列）
        self._last_x: int = 0
        self._last_y: int = 0

        self._pygame_inited: bool = False
        self._joystick = None
        self._auto_mode: bool = False

    # ---------------------------------------------------------
    def is_enabled(self) -> bool:
        """配置是否启用手柄"""
        return bool(self.config.get("GAMEPAD_ENABLED", False))

    # ---------------------------------------------------------
    def start(self) -> bool:
        """启动手柄后台线程"""
        if not self.is_enabled():
            logging.info("Gamepad disabled by config (GAMEPAD_ENABLED=False)")
            return False

        if self.thread is not None and self.thread.is_alive():
            return True

        # headless 初始化：不依赖 X11/桌面，纯读输入设备
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
        os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

        try:
            import pygame
            pygame.init()
            pygame.joystick.init()
            self._pygame = pygame
            self._pygame_inited = True
        except Exception as ex:
            logging.error(f"Failed to init pygame for gamepad: {repr(ex)}")
            return False

        self.exit_flag.clear()
        self.thread = Thread(target=self.__gamepad_thread, daemon=True)
        self.thread.start()
        self.active = True
        logging.info("Gamepad background thread started")
        return True

    # ---------------------------------------------------------
    def stop(self):
        """停止手柄后台线程"""
        self.exit_flag.set()
        if self.thread is not None:
            self.thread.join(timeout=1.0)
            self.thread = None
        self.active = False
        self.connected = False

        if self._pygame_inited:
            try:
                if self._joystick is not None:
                    self._joystick.quit()
                    self._joystick = None
                self._pygame.joystick.quit()
                self._pygame.quit()
            except Exception as ex:
                logging.error(f"Error quitting pygame: {repr(ex)}")
        self._pygame_inited = False

    # ---------------------------------------------------------
    def status(self) -> dict:
        """供 /gamepadStatus 路由返回"""
        return {
            "enabled": self.is_enabled(),
            "active": self.active,
            "connected": self.connected,
        }

    # ---------------------------------------------------------
    def _send(self, command: str):
        """下发指令，复用 ArduinoDevice 队列（线程安全）"""
        if self.arduino is not None and self.arduino.is_connected():
            self.arduino.send_command(command)

    # ---------------------------------------------------------
    def __gamepad_thread(self):
        """主循环：轮询 pygame 事件，映射成串口指令"""
        pygame = self._pygame
        poll = float(self.config.get("GAMEPAD_POLL_INTERVAL", 0.02))
        deadzone = float(self.config.get("GAMEPAD_DEADZONE", 0.2))

        while not self.exit_flag.is_set():
            try:
                for event in pygame.event.get():
                    if event.type == pygame.JOYDEVICEADDED:
                        if self._joystick is None and pygame.joystick.get_count() > 0:
                            self._joystick = pygame.joystick.Joystick(event.device_index)
                            self._joystick.init()
                            self.connected = True
                            logging.info(f"Gamepad connected: {self._joystick.get_name()}")
                            # 连上时归零运动，防残留
                            self._send_motor(0, 0)
                    elif event.type == pygame.JOYDEVICEREMOVED:
                        if self._joystick is not None:
                            logging.info(f"Gamepad disconnected: {self._joystick.get_name()}")
                            self._joystick.quit()
                            self._joystick = None
                        self.connected = False
                        self._send_motor(0, 0)   # 断连归零
                    elif event.type == pygame.JOYAXISMOTION and self._joystick is not None:
                        self._handle_axis(event.axis, event.value, deadzone)
                    elif event.type == pygame.JOYBUTTONDOWN and self._joystick is not None:
                        self._handle_button(event.button)

                # 持续输入的增量类（臂）需要按住时持续累加，
                # pygame 只在按下/松开发事件，这里在轴循环外再按住状态推进。
                self._poll_held_arms()

            except Exception as ex:
                logging.error(f"Gamepad thread error: {repr(ex)}")

            time.sleep(poll)

    # ---------------------------------------------------------
    def _apply_deadzone(self, value: float, deadzone: float) -> float:
        """死区处理：绝对值小于阈值归零"""
        return 0.0 if abs(value) < deadzone else value

    # ---------------------------------------------------------
    def _handle_axis(self, axis: int, value: float, deadzone: float):
        """摇杆轴事件 -> 串口指令

        复刻 main.js moveAxis() + sendMovementValues()：
          axis 0/1: 左摇杆 X/Y -> 行走 X/Y
          axis 2:   右摇杆 X -> 头部旋转 G（增量累加）
          axis 3:   右摇杆 Y -> 颈部 T/B（增量累加 + 分段映射）
          axis 4/5: LT/RT -> 手臂 L/R（按住方向，在 _poll_held_arms 推进）
        """
        v = self._apply_deadzone(value, deadzone)

        if axis == 0:   # 左摇杆 X -> 转向
            x = int(v * 100)
            x = max(-100, min(100, x))
            self._send_motor(x, self._last_y)
            self._last_x = x
        elif axis == 1:  # 左摇杆 Y -> 前进/后退
            # main.js: stickY = -moveXY[3]，即上推为正
            y = int(-v * 100)
            y = max(-100, min(100, y))
            self._send_motor(self._last_x, y)
            self._last_y = y
        elif axis == 2:  # 右摇杆 X -> 头部旋转
            if v != 0:
                mult = float(self.config.get("GAMEPAD_HEAD_MULTIPLIER", 5))
                self.head_rotation += v * mult
                self.head_rotation = max(0, min(100, self.head_rotation))
                self._send(f"G{int(self.head_rotation)}")
        elif axis == 3:  # 右摇杆 Y -> 颈部
            if v != 0:
                mult = float(self.config.get("GAMEPAD_NECK_MULTIPLIER", 5))
                self.neck += v * mult
                self.neck = max(0, min(200, self.neck))
                self._send_neck()
        # axis 4/5 (LT/RT) 作为触发器轴：按下为 1，松开为 0
        # 在 _poll_held_arms 里按当前轴值推进手臂增量

    # ---------------------------------------------------------
    def _send_motor(self, x: int, y: int):
        """下发行走指令（值变化才发）"""
        if not (self.arduino and self.arduino.is_connected()):
            return
        self.arduino.send_command(f"X{x}")
        self.arduino.send_command(f"Y{y}")

    # ---------------------------------------------------------
    def _send_neck(self):
        """颈部增量 -> T/B 分段映射，复刻 main.js :808-824"""
        n = self.neck
        if n < 100:
            self._send(f"T{int(n)}")
            self._send("B0")
        elif n < 160:
            self._send(f"T{int(200 - n)}")
            self._send(f"B{int(n - 100)}")
        else:
            self._send(f"T{int(n) - 110}")
            self._send("B60")

    # ---------------------------------------------------------
    def _poll_held_arms(self):
        """按住 LT/RT 持续移动手臂。

        main.js 用 button_4/5/6/7 的 hold 状态推进 moveArms[0]/[2]。
        pygame 把 LT/RT 当轴(axis 4/5)或按钮，这里取轴值（0..1）。
        若手柄把 LT/RT 映射成按钮，则 axis 数 <4，走 _handle_button。
        """
        if self._joystick is None:
            return
        try:
            naxes = self._joystick.get_numaxes()
        except Exception:
            return

        mult = float(self.config.get("GAMEPAD_ARMS_MULTIPLIER", 6))
        changed = False

        if naxes > 4:   # axis 4 = LT
            lt = self._joystick.get_axis(4)
            if lt > 0.2:
                self.arm_left = max(0, self.arm_left - mult)
                changed = True
        if naxes > 5:   # axis 5 = RT
            rt = self._joystick.get_axis(5)
            if rt > 0.2:
                self.arm_right = max(0, self.arm_right - mult)
                changed = True

        if changed:
            self._send(f"L{int(self.arm_left)}")
            self._send(f"R{int(self.arm_right)}")

    # ---------------------------------------------------------
    def _handle_button(self, button: int):
        """按键事件 -> 串口指令，复刻 main.js pressButton()"""
        # 眼部表情 / 头部姿态（单字符指令，仅串口监视器协议，但 Arduino 接受）
        if button == 0:
            self._send("i")          # 悲伤眼
        elif button == 1:
            self._send("l")          # 右倾头
        elif button == 2:
            self._send("j")          # 左倾头
        elif button == 3:
            self._send("k")          # 中性眼
        elif button == 4:            # LB -> 升左臂
            self.arm_left = min(100, self.arm_left + 6)
            self._send(f"L{int(self.arm_left)}")
        elif button == 5:            # RB -> 升右臂
            self.arm_right = min(100, self.arm_right + 6)
            self._send(f"R{int(self.arm_right)}")
        elif button == 6:            # LT(button) -> 降左臂
            self.arm_left = max(0, self.arm_left - 6)
            self._send(f"L{int(self.arm_left)}")
        elif button == 7:            # RT(button) -> 降右臂
            self.arm_right = max(0, self.arm_right - 6)
            self._send(f"R{int(self.arm_right)}")
        elif button == 8:            # Back/Share -> 切换自动/手动模式
            self._auto_mode = not self._auto_mode
            self._send(f"M{1 if self._auto_mode else 0}")
        elif button == 10:           # L3 -> 手臂回中性
            self.arm_left = 50
            self.arm_right = 50
            self._send("n")
        elif button == 11:           # R3 -> 头部回中性
            self.head_rotation = 50
            self.neck = 125
            self._send("G50")
            self._send("g")
        elif button == 14:           # D-Pad < -> 随机播放声音
            self._play_random_sound()
        elif button == 15:           # D-Pad > -> 随机播放动画
            self._play_random_animation()

    # ---------------------------------------------------------
    def _play_random_animation(self):
        """随机播放一个动画（A 指令）。

        动画编号范围在 Arduino animations.ino 的 switch case 里定义。
        仓库内置 2 个动画（case 0/1/2），取 0..2 随机。
        """
        # 无法从 Arduino 端读取动画数量，硬编码已知范围
        import random as _r
        self._send(f"A{_r.randint(0, 2)}")

    # ---------------------------------------------------------
    def _play_random_sound(self):
        """随机播放一个声音文件。

        复用 config.SOUND_FOLDER 扫描 *.wav，用 AUDIOPLAYER_CMD 播放。
        与 app.py /audio 路由逻辑一致。
        """
        import random as _r
        import subprocess
        sound_folder = self.config.get("SOUND_FOLDER")
        fmt = self.config.get("SOUND_FORMAT", "wav")
        if not sound_folder:
            return
        try:
            import os as _os
            files = [f for f in sorted(_os.listdir(sound_folder)) if f.endswith(f".{fmt}")]
            if not files:
                return
            clip = _r.choice(files)
            path = _os.path.join(sound_folder, clip)
            player = self.config.get("AUDIOPLAYER_CMD", ["aplay"])
            subprocess.Popen(player + [path],
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        except Exception as ex:
            logging.error(f"Gamepad random sound error: {repr(ex)}")
