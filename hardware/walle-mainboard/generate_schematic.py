#!/usr/bin/env python3
"""Generate walle-mainboard KiCad 10 schematic + project file.

Design input: hardware/主板设计说明.md (per-pin connector tables) — see the
NET tables below, one entry per connector pin.

The PCB (walle-mainboard.kicad_pcb) is generated separately by
generate_pcb.py, which imports the pin->net tables from this module.

Usage:
    python3 generate_schematic.py           # write .kicad_sch + .kicad_pro
    python3 generate_schematic.py --check   # re-parse generated .kicad_sch and
                                            # diff connector pin->net vs spec
"""

import json
import math
import re
import sys
import uuid as uuidlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SYM_DIR = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")

SCH_PATH = HERE / "walle-mainboard.kicad_sch"
PRO_PATH = HERE / "walle-mainboard.kicad_pro"

SHEET_UUID = "7a1e2c10-5b6d-4e8f-9a0b-walle0000001".replace("walle", "1c2d")  # fixed
SHEET_UUID = "7a1e2c10-5b6d-4e8f-9a0b-1c2d00000001"
PROJECT = "walle-mainboard"


GRID = 1.27  # 50 mil connection grid


def g(v):
    return round(round(float(v) / GRID) * GRID, 2)


def uid():
    return str(uuidlib.uuid4())


def r2(v):
    v = round(float(v), 2)
    if v == int(v):
        return str(int(v))
    return f"{v:.2f}".rstrip("0").rstrip(".")


# ---------------------------------------------------------------------------
# Standard library symbol extraction (verbatim copy into lib_symbols)
# ---------------------------------------------------------------------------

def extract_symbol(libfile, name):
    """Return the verbatim text of `(symbol "name" ...)` from a .kicad_sym."""
    text = (SYM_DIR / libfile).read_text(encoding="utf-8")
    pat = re.compile(r'^\t\(symbol "' + re.escape(name) + r'"', re.M)
    m = pat.search(text)
    if not m:
        raise SystemExit(f"symbol {name} not found in {libfile}")
    i = m.start()
    depth = 0
    j = i
    while True:
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                break
        j += 1
    return text[i : j + 1]


def parse_symbol_geometry(symtext, name):
    """Parse pin number -> (x, y, name) and property positions from a lib symbol."""
    pins = {}
    pin_re = re.compile(
        r'\(pin\s+(\S+)\s+(\S+)\s+\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)'
        r'(.*?)\(number\s+"([^"]+)"',
        re.S,
    )
    for m in pin_re.finditer(symtext):
        num = m.group(7)
        pname_m = re.search(r'\(name\s+"([^"]*)"', m.group(6))
        pins[num] = (
            float(m.group(3)),
            float(m.group(4)),
            pname_m.group(1) if pname_m else "",
        )
    props = {}
    prop_re = re.compile(
        r'\(property\s+"(Reference|Value)"\s+"[^"]*"\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)'
    )
    for m in prop_re.finditer(symtext):
        props[m.group(1)] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
    return pins, props


SYMBOLS = {}  # nick:Name -> dict(text, pins, props)


def load_symbol(libfile, name):
    nick = libfile.replace(".kicad_sym", "")
    full = f"{nick}:{name}"
    text = extract_symbol(libfile, name)
    pins, props = parse_symbol_geometry(text, name)
    SYMBOLS[full] = {"text": text, "pins": pins, "props": props, "name": name, "nick": nick}
    return full


for lib, names in {
    "Connector_Generic.kicad_sym": [
        "Conn_01x02", "Conn_01x03", "Conn_01x04", "Conn_01x06", "Conn_01x07", "Conn_01x08", "Conn_01x22",
    ],
    "Device.kicad_sym": ["R", "C", "C_Polarized", "D", "LED"],
    "power.kicad_sym": ["+12V", "+5V", "+3V3", "GND", "PWR_FLAG"],
}.items():
    for n in names:
        load_symbol(lib, n)

# PCM5102 is an L-shaped 15-pin module; synthesize a generic 15-pin socket
# symbol (no Conn_01x15 in the KiCad lib) so the schematic netlist matches and
# the pins render in eeschema.  Body/pins modeled on the real Conn_01x08.
def _conn15_text():
    L = ['\t(symbol "Connector_Generic:Conn_01x15"',
         '\t\t(pin_names',
         '\t\t\t(offset 1.016)',
         '\t\t\t(hide yes)',
         '\t\t)',
         '\t\t(exclude_from_sim no)',
         '\t\t(in_bom yes)',
         '\t\t(on_board yes)',
         '\t\t(in_pos_files yes)',
         '\t\t(duplicate_pin_numbers_are_jumpers no)']
    for pn, pv, px, py in (("Reference", "J", 0, 10.16),
                           ("Value", "Conn_01x15", 0, -17.78)):
        L += [f'\t\t(property "{pn}" "{pv}"',
              f'\t\t\t(at {px} {py} 0)',
              '\t\t\t(show_name no)',
              '\t\t\t(do_not_autoplace no)',
              '\t\t\t(effects',
              '\t\t\t\t(font',
              '\t\t\t\t\t(size 1.27 1.27)',
              '\t\t\t\t)',
              '\t\t\t)',
              '\t\t)']
    L += ['\t\t(symbol "Conn_01x15_1_1"',
          '\t\t\t(rectangle',
          '\t\t\t\t(start -1.27 8.89)',
          '\t\t\t\t(end 1.27 -27.94)',
          '\t\t\t\t(stroke',
          '\t\t\t\t\t(width 0.254)',
          '\t\t\t\t\t(type default)',
          '\t\t\t\t)',
          '\t\t\t\t(fill',
          '\t\t\t\t\t(type background)',
          '\t\t\t\t)',
          '\t\t\t)']
    for i in range(1, 16):
        y = 7.62 - 2.54 * (i - 1)
        L += [f'\t\t\t(pin passive line',
              f'\t\t\t\t(at -5.08 {y} 0)',
              f'\t\t\t\t(length 3.81)',
              f'\t\t\t\t(name "Pin_{i}"',
              '\t\t\t\t\t(effects',
              '\t\t\t\t\t\t(font',
              '\t\t\t\t\t\t\t(size 1.27 1.27)',
              '\t\t\t\t\t\t)',
              '\t\t\t\t\t)',
              '\t\t\t\t)',
              f'\t\t\t\t(number "{i}"',
              '\t\t\t\t\t(effects',
              '\t\t\t\t\t\t(font',
              '\t\t\t\t\t\t\t(size 1.27 1.27)',
              '\t\t\t\t\t\t)',
              '\t\t\t\t\t)',
              '\t\t\t\t)',
              '\t\t\t)']
    L += ['\t\t)',
          '\t\t(embedded_fonts no)',
          '\t)']
    return "\n".join(L)


SYMBOLS["Connector_Generic:Conn_01x15"] = {
    "text": _conn15_text(),
    "pins": {str(i): (-5.08, 7.62 - 2.54 * (i - 1), "") for i in range(1, 16)},
    "props": {"Reference": (0, 10.16, 0), "Value": (0, -17.78, 0)},
    "name": "Conn_01x15", "nick": "Connector_Generic",
}

# sanity checks on parsed geometry
assert SYMBOLS["Connector_Generic:Conn_01x22"]["pins"].keys() == {str(i) for i in range(1, 23)}
assert SYMBOLS["power:PWR_FLAG"]["pins"], "PWR_FLAG pin parse failed"
for s in ("Device:R", "Device:C", "Device:C_Polarized"):
    assert set(SYMBOLS[s]["pins"]) == {"1", "2"}, s


# ---------------------------------------------------------------------------
# Net assignment tables (authoritative: hardware/主板设计说明.md §1/§2)
#   entry: net name string, or "NC", power rails prefixed with "PWR:"
# ---------------------------------------------------------------------------

def P(net):
    return "PWR:" + net


J1A = ["PWR:+3V3", "NC", "NC", "MIC_WS", "MIC_SCK", "MIC_SD", "DAC_DIN", "DAC_BCK",
       "DAC_LCK", "MOT_AIN1", "MOT_AIN2", "NC", "4G_RX", "NC", "CAM_TX", "CAM_RX",
       "MOT_PWMB", "MOT_BIN1", "MOT_BIN2", "TFT_SCL", "PWR:+5V", "PWR:GND"]
J1B = ["PWR:GND", "UART0_TX", "UART0_RX", "BAT_ADC", "4G_TX", "TFT_BLK", "BTN_RST",
       "TFT_DC", "BTN_VDN", "BTN_VUP", "NC", "NC", "NC", "BTN_BOOT", "TFT_RES",
       "NC", "TFT_SDA", "I2C_SCL", "I2C_SDA", "MOT_PWMA", "NC", "NC"]
# 针序按实物实测（2026-08）：LU9685 两排7P 排距61, 由下至上 GND TXD RXD SCL SDA DVCC V+；
# TB6612 两排8P 排距16, 左排由下至上 GND PWMB BIN2 BIN1 STBY AIN1 AIN2 PWMA,
#   右排由下至上 GND BO1 BO2 AO2 AO1 GND VCC VM；
# PCM5102 L型, 左排自上而下 SCK BCK DIN LCK GND VIN, 上排自左而右 FLT DEMP XSMT FMT A3V3 AGND ROUT AGND LROUT。
J2 = ["PWR:GND", "NC", "NC", "I2C_SCL", "I2C_SDA", "PWR:+5V", "+5V_SERVO"]
J3A = ["PWR:GND", "MOT_PWMB", "MOT_BIN2", "MOT_BIN1", "PWR:+5V", "MOT_AIN1", "MOT_AIN2", "MOT_PWMA"]
J3B = ["PWR:GND", "MOTB_1", "MOTB_2", "MOTA_2", "MOTA_1", "PWR:GND", "PWR:+5V", "PWR:+12V"]
J4 = ["NC", "DAC_BCK", "DAC_DIN", "DAC_LCK", "PWR:GND", "PWR:+5V",
      "NC", "NC", "NC", "NC", "PWR:+3V3", "PWR:GND", "DAC_R", "PWR:GND", "DAC_L"]
J5 = ["PWR:+5V", "PWR:GND", "DAC_L", "DAC_R", "SPK_L+", "SPK_L-", "SPK_R+", "SPK_R-"]
J6A = ["PWR:+12V", "PWR:GND"]
J6B = ["+5V_SERVO", "PWR:GND"]
JBAT = ["+12V_RAW", "PWR:GND"]
JMOTA = ["MOTA_1", "MOTA_2"]
JMOTB = ["MOTB_1", "MOTB_2"]
JPANEL = ["PWR:+5V", "PWR:+3V3", "PWR:GND", "PWR:GND", "SPK_L+", "SPK_L-", "SPK_R+",
          "SPK_R-", "MIC_SCK", "MIC_WS", "MIC_SD", "BTN_BOOT", "BTN_VUP", "BTN_VDN",
          "BTN_RST", "TFT_SCL", "TFT_SDA", "TFT_RES", "TFT_DC", "TFT_BLK",
          "PWR:GND", "PWR:+3V3"]
JCAM = ["PWR:+5V", "PWR:GND", "CAM_TX", "CAM_RX"]
JDBG = ["UART0_TX", "UART0_RX", "PWR:GND"]
# ML307R-DL mini 核心板针序（2026-08 用户实测）：左右两排 6P，各排从下往上为脚 1→6
# 左排：+5V GND TXD RXD EN BAT；右排：+5V GND USB_DP USB_DN NET BOT
J4GL = ["+5V_SERVO", "PWR:GND", "4G_RX", "4G_TX", "4G_EN", "NC"]   # TXD→4G_RX(GPIO3) RXD→4G_TX(GPIO2); BAT(3.7V)不用
J4GR = ["+5V_SERVO", "PWR:GND", "NC", "NC", "4G_NET", "NC"]       # USB_DP/DN 不用; BOT 悬空

PS = "Connector_PinSocket_2.54mm:PinSocket_1x{:02d}_P2.54mm_Vertical"
PH = "Connector_PinHeader_2.54mm:PinHeader_1x{:02d}_P2.54mm_Vertical"

# (ref, lib_symbol, value, footprint, x, y, pins[list], note)
# 分区分列:  ESP32(左) | LU9685+TB6612 | J-PANEL+音频 | 电源+外设 | 4G+调试
CONNECTORS = [
    ("J1A", "Connector_Generic:Conn_01x22", "ESP32-S3 主控插座 左排(天线端→USB端)",
     PS.format(22), 58, 50, J1A, "J1: ESP32-S3主控, 排针顺序=天线端→USB端\nJ1B 21/22为模块第2/3个GND, 底板不接\n排母2×22P: 首尾53.34mm, 排距25.40mm(布局保证)"),
    ("J1B", "Connector_Generic:Conn_01x22", "ESP32-S3 主控插座 右排(天线端→USB端)",
     PS.format(22), 58, 160, J1B, None),
    ("J2A", "Connector_Generic:Conn_01x07", "LU9685 舵机驱动 A排",
     PS.format(7), 135, 50, J2, "J2: LU9685, 两排7P网络并联(分摊舵机电流); V+大电流轨,另备短线直连模块2P端子"),
    ("J2B", "Connector_Generic:Conn_01x07", "LU9685 舵机驱动 B排",
     PS.format(7), 135, 95, J2, None),
    ("J3A", "Connector_Generic:Conn_01x08", "TB6612FNG 左排(信号/输出)",
     PS.format(8), 135, 150, J3A, "J3: TB6612FNG 两排8P排距16; 左排由下至上 GND PWMB BIN2 BIN1 STBY AIN1 AIN2 PWMA"),
    ("J3B", "Connector_Generic:Conn_01x08", "TB6612FNG 右排(电源/输出)",
     PS.format(8), 135, 195, J3B, "右排由下至上 GND BO1 BO2 AO2 AO1 GND VCC VM; STBY接+5V常开"),
    ("J-PANEL", "Connector_Generic:Conn_01x22", "FPC 22P 1.0mm 下接座→副板",
     "Connector_FFC-FPC:JUSHUO_AFA07-S22FCA-00_1x22-1MP_P1.0mm_Horizontal",
     230, 50, JPANEL, "J-PANEL: 反向FPC 1↔22镜像,按丝印对插\n喇叭/麦/按键/1.3寸屏全部经此口到副板"),
    ("J4", "Connector_Generic:Conn_01x15", "GY-PCM5102 DAC (L形两排)",
     "PCM5102", 230, 155, J4, "J4: GY-PCM5102 32x18; 左排SCK BCK DIN LCK GND VIN, 上排FLT..LROUT"),
    ("J5", "Connector_Generic:Conn_01x08", "PAM8406 功放",
     PH.format(8), 230, 210, J5, "J5: PAM8406; LIN/RIN←PCM5102, 差分输出经J-PANEL到副板喇叭"),
    ("J-BAT", "Connector_Generic:Conn_01x02", "12V电池输入 (占位)",
     PH.format(2), 305, 50, JBAT, "J-BAT: Ø2mm焊盘×2, 开关/保险丝不上板"),
    ("J6A", "Connector_Generic:Conn_01x02", "DC-DC 降压 IN (占位)",
     PH.format(2), 305, 80, J6A, "J6: DC-DC 15A持续版(8A禁舵机轨); 焊盘间距18mm"),
    ("J6B", "Connector_Generic:Conn_01x02", "DC-DC 降压 OUT (占位)",
     PH.format(2), 305, 110, J6B, None),
    ("J-MOTA", "Connector_Generic:Conn_01x02", "左电机 XH2P",
     "Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical", 305, 160, JMOTA, None),
    ("J-MOTB", "Connector_Generic:Conn_01x02", "右电机 XH2P",
     "Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical", 305, 188, JMOTB,
     "J-MOTA/B: XHB 2P 左右板边镜像"),
    ("J-CAM", "Connector_Generic:Conn_01x04", "CAM模组 XHB4P",
     "Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical", 305, 216, JCAM,
     "J-CAM: UART交叉在背板完成"),
    ("J-DBG", "Connector_Generic:Conn_01x03", "调试串口 焊盘(占位)",
     PH.format(3), 395, 50, JDBG,
     "J-DBG: 底层焊盘, 调试日志口"),
    ("J-4GA", "Connector_Generic:Conn_01x06", "ML307R-DL核心板 左排(脚1=最下)",
     PS.format(6), 395, 95, J4GL,
     "J-4G: 2×6P排母,针序实测(脚1=最下)\n左排: +5V/GND/TXD/RXD/EN/BAT; BAT不用→NC\nEN经R_EN上拉; 线束30cm双头同序"),
    ("J-4GB", "Connector_Generic:Conn_01x06", "ML307R-DL核心板 右排(脚1=最下)",
     PS.format(6), 395, 155, J4GR,
     "右排: +5V/GND/USB_DP/DN/NET/BOT; USB不用→NC\n两排+5V并联2A,旁加C4G 470µF"),
]

# ---------------------------------------------------------------------------
# Schematic item emitters
# ---------------------------------------------------------------------------

ITEMS = []  # text chunks


def emit(s):
    ITEMS.append(s)


def rot_point(lx, ly, deg):
    """Library coords (y-up) -> canvas offset (y-down) for CCW rotation deg."""
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return (lx * c - ly * s, -lx * s - ly * c)


def fmt_at(x, y, a=None):
    return f"(at {r2(x)} {r2(y)}" + (f" {int(a)}" if a is not None else "") + ")"


def emit_symbol(ref, lib, value, footprint, x, y, rot=0, extra_props=None,
                ref_hide=False, power=False, value_hide=False):
    x, y = g(x), g(y)
    sym = SYMBOLS[lib]
    u = uid()

    def prop_pos(pname, fallback_dy):
        if pname in sym["props"]:
            lx, ly, la = sym["props"][pname]
        else:
            lx, ly, la = 0, fallback_dy, 0
        dx, dy = rot_point(lx, ly, rot)
        return x + dx, y + dy, (la + rot) % 360

    rx, ry, ra = prop_pos("Reference", -6)
    vx, vy, va = prop_pos("Value", 6)

    lines = []
    lines.append("\t(symbol")
    lines.append(f'\t\t(lib_id "{lib}")')
    lines.append(f"\t\t{fmt_at(x, y, rot)}")
    lines.append("\t\t(unit 1)")
    lines.append("\t\t(body_style 1)")
    lines.append("\t\t(exclude_from_sim no)")
    if power:
        lines.append("\t\t(in_bom no)")
        lines.append("\t\t(on_board no)")
        lines.append("\t\t(in_pos_files no)")
    else:
        lines.append("\t\t(in_bom yes)")
        lines.append("\t\t(on_board yes)")
        lines.append("\t\t(in_pos_files yes)")
    lines.append("\t\t(dnp no)")
    lines.append(f'\t\t(uuid "{u}")')

    def prop(name, val, px, py, pa, hide=False):
        lines.append(f'\t\t(property "{name}" "{val}"')
        lines.append(f"\t\t\t{fmt_at(px, py, pa)}")
        if hide:
            lines.append("\t\t\t(hide yes)")
        lines.append("\t\t\t(show_name no)")
        lines.append("\t\t\t(do_not_autoplace no)")
        lines.append("\t\t\t(effects")
        lines.append("\t\t\t\t(font")
        lines.append("\t\t\t\t\t(size 1.27 1.27)")
        lines.append("\t\t\t\t)")
        if ref_hide and name == "Reference":
            lines.append("\t\t\t\t(hide yes)")
        if value_hide and name == "Value":
            lines.append("\t\t\t\t(hide yes)")
        lines.append("\t\t\t)")
        lines.append("\t\t)")

    prop("Reference", ref, rx, ry, ra)
    prop("Value", value, vx, vy, va)
    prop("Footprint", footprint, x, y, 0, hide=True)
    prop("Datasheet", "", x, y, 0, hide=True)
    prop("Description", "", x, y, 0, hide=True)
    for pn, pv in (extra_props or []):
        prop(pn, pv, x, y, 0, hide=True)
    for num in sorted(sym["pins"], key=lambda s: int(s) if s.isdigit() else 0):
        lines.append(f'\t\t(pin "{num}"')
        lines.append(f'\t\t\t(uuid "{uid()}")')
        lines.append("\t\t)")
    lines.append("\t\t(instances")
    lines.append(f'\t\t\t(project "{PROJECT}"')
    lines.append(f'\t\t\t\t(path "/{SHEET_UUID}"')
    lines.append(f'\t\t\t\t\t(reference "{ref}")')
    lines.append("\t\t\t\t\t(unit 1)")
    lines.append("\t\t\t\t)")
    lines.append("\t\t\t)")
    lines.append("\t\t)")
    lines.append("\t)")
    emit("\n".join(lines))
    return u


def pin_endpoint(lib, num, x, y, rot=0):
    lx, ly, _ = SYMBOLS[lib]["pins"][num]
    dx, dy = rot_point(lx, ly, rot)
    return (round(x + dx, 2), round(y + dy, 2))


def emit_wire(x1, y1, x2, y2):
    x1, y1, x2, y2 = g(x1), g(y1), g(x2), g(y2)
    emit("\t(wire\n"
         "\t\t(pts\n"
         f"\t\t\t(xy {r2(x1)} {r2(y1)}) (xy {r2(x2)} {r2(y2)})\n"
         "\t\t)\n"
         "\t\t(stroke\n"
         "\t\t\t(width 0)\n"
         "\t\t\t(type default)\n"
         "\t\t)\n"
         f'\t\t(uuid "{uid()}")\n'
         "\t)")


def emit_junction(x, y):
    x, y = g(x), g(y)
    emit("\t(junction\n"
         f"\t\t{fmt_at(x, y)}\n"
         "\t\t(diameter 0)\n"
         "\t\t(color 0 0 0 0)\n"
         f'\t\t(uuid "{uid()}")\n'
         "\t)")


def emit_no_connect(x, y):
    x, y = g(x), g(y)
    emit("\t(no_connect\n"
         f"\t\t{fmt_at(x, y)}\n"
         f'\t\t(uuid "{uid()}")\n'
         "\t)")


def emit_label(name, x, y, angle=0):
    x, y = g(x), g(y)
    # 锚点在标签尖端：angle=0（朝右）文字向右伸；angle=180（朝左，挂在符号
    # 左侧线桩上）文字向左伸（justify right），否则会压到引脚编号。
    justify = "left" if angle == 0 else "right"
    emit(f'\t(global_label "{name}"\n'
         "\t\t(shape bidirectional)\n"
         f"\t\t{fmt_at(x, y, angle)}\n"
         "\t\t(effects\n"
         "\t\t\t(font\n"
         "\t\t\t\t(size 1.27 1.27)\n"
         "\t\t\t)\n"
         f"\t\t\t(justify {justify} bottom)\n"
         "\t\t)\n"
         f'\t\t(uuid "{uid()}")\n'
         '\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"\n'
         f"\t\t\t{fmt_at(x, y, 0)}\n"
         "\t\t\t(effects\n"
         "\t\t\t\t(font\n"
         "\t\t\t\t\t(size 1.27 1.27)\n"
         "\t\t\t\t)\n"
         "\t\t\t\t(hide yes)\n"
         "\t\t\t)\n"
         "\t\t)\n"
         "\t)")


PWR_COUNT = {"n": 0}


def emit_power(net, x, y, rot=0):
    """Place a power symbol (+5V/GND/...) connecting at its anchor (x,y)."""
    PWR_COUNT["n"] += 1
    ref = f"#PWR0{PWR_COUNT['n']:02d}"
    emit_symbol(ref, f"power:{net}", net, "", x, y, rot=rot,
                ref_hide=True, power=True)


def emit_pwr_flag(x, y, rot=0):
    PWR_COUNT["n"] += 1
    ref = f"#FLG0{PWR_COUNT['n']:02d}"
    emit_symbol(ref, "power:PWR_FLAG", "PWR_FLAG", "", x, y, rot=rot,
                ref_hide=True, power=True, value_hide=True)


def emit_text(txt, x, y, size=1.27, justify="left"):
    x, y = g(x), g(y)
    txt = txt.replace("\\", "\\\\").replace('"', '\\"')
    emit(f'\t(text "{txt}"\n'
         "\t\t(exclude_from_sim no)\n"
         f"\t\t{fmt_at(x, y, 0)}\n"
         "\t\t(effects\n"
         "\t\t\t(font\n"
         f"\t\t\t\t(size {size} {size})\n"
         "\t\t\t)\n"
         f"\t\t\t(justify {justify} bottom)\n"
         "\t\t)\n"
         f'\t\t(uuid "{uid()}")\n'
         "\t)")


# ---------------------------------------------------------------------------
# Build schematic items
# ---------------------------------------------------------------------------

# Header notes
HEADER = [
    "WALLE Mainboard — 模块插接底板(方案A), 板框63×80mm, 2层板, 全直插; 本页仅原理图, 布局布线另作。",
    "顶层: ESP32-S3(J1) + LU9685(J2) 长边沿80mm方向并排; 底层: DC-DC(J6) + TB6612(J3) + PCM5102(J4) + PAM8406(J5) + 小元件(R*/C*/D*)。",
    "GPIO0/45/46 为 strapping 引脚, 注意默认电平; 打样前按《主板设计说明.md》§7 清单卡尺复核各模块排距。",
    "电源链: J-BAT(+12V_RAW)→D1→+12V→J6 IN; DC-DC OUT→+5V_SERVO→R0(0Ω)→+5V; +3V3来自主控板载LDO; +5V_SERVO峰值4-6A,铺铜≥4mm。",
]
for i, line in enumerate(HEADER):
    emit_text(line, 12, 14 + i * 4)

# Connectors — 分区分列，注释收集到底部「模块说明」图例，不占符号区
CONN_NOTES = []
for ref, lib, value, fp, x, y, pins, note in CONNECTORS:
    emit_symbol(ref, lib, value, fp, x, y,
                extra_props=[("Assembly", "顶层插接" if ref in ("J1A", "J1B", "J2A", "J2B") else "底层插接")])
    sym = SYMBOLS[lib]
    pwr_run = 0  # stagger consecutive power symbols so their graphics don't overlap
    for idx, net in enumerate(pins):
        num = str(idx + 1)
        ex, ey = pin_endpoint(lib, num, x, y)
        lx = sym["pins"][num][0]
        # stub direction: pins point outward on -x side for these symbols
        sdir = -1 if lx < 0 else 1
        if net.startswith("PWR:"):
            # alternate 5.08 / 12.7 / 20.32 so stacked power pins' symbols
            # (each ~5-6 units tall, pins 2.54 apart) don't overlap
            sx = sdir * (5.08 + 7.62 * (pwr_run % 3))
            pwr_run += 1
        else:
            sx = sdir * 5.08
            pwr_run = 0
        wx, wy = round(ex + sx, 2), ey
        if net == "NC":
            emit_no_connect(ex, ey)
        else:
            emit_wire(ex, ey, wx, wy)
            if net.startswith("PWR:"):
                emit_power(net[4:], wx, wy, rot=270 if sx < 0 else 90)
            else:
                emit_label(net, wx, wy, angle=180 if sx < 0 else 0)
    if note:
        CONN_NOTES.append((ref, note))

# --- Discrete section (all bottom-side parts) ---
# 离散元件整理成两列整块，放在 col2(135) 与 col3(230) 之间的空隙 (x≈155-220)。
R_FP = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal"
emit_text("小元件 C1/C2/C4G/R1/R2/R3/R_EN/R0/D1/D2 全部装底层", 155, 30)

# C1 / C2 / C4G rail caps to GND
def cap(ref, value, fp, x, y, rail_label=True, rail="+5V_SERVO"):
    emit_symbol(ref, "Device:C_Polarized" if "CP_Radial" in fp else "Device:C",
                value, fp, x, y, extra_props=[("Assembly", "装底层")])
    lib = "Device:C_Polarized" if "CP_Radial" in fp else "Device:C"
    t = pin_endpoint(lib, "1", x, y)
    b = pin_endpoint(lib, "2", x, y)
    emit_wire(t[0], t[1], t[0], t[1] - 7.62)
    emit_wire(t[0], t[1] - 7.62, t[0] + 5.08, t[1] - 7.62)
    emit_label(rail, t[0] + 5.08, t[1] - 7.62, angle=0)
    emit_wire(b[0], b[1], b[0], b[1] + 7.62)
    emit_power("GND", b[0], b[1] + 7.62, rot=0)

# 左列 x=160
cap("C1", "470µF/25V", "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm", 160, 55)
cap("C4G", "470µF/10V", "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm", 160, 90)

# D1 reverse-polarity protection: anode=+12V_RAW, cathode=+12V
emit_symbol("D1", "Device:D", "10A10", "Diode_THT:D_P600_R-6_P12.70mm_Horizontal",
            160, 123, extra_props=[("Assembly", "装底层")])
d1k = pin_endpoint("Device:D", "1", 160, 123)   # K
d1a = pin_endpoint("Device:D", "2", 160, 123)   # A
emit_wire(d1k[0], d1k[1], d1k[0] - 7.62, d1k[1])
emit_power("+12V", d1k[0] - 7.62, d1k[1], rot=270)
emit_wire(d1a[0], d1a[1], d1a[0] + 7.62, d1a[1])
emit_label("+12V_RAW", d1a[0] + 7.62, d1a[1], angle=0)

# R1/R2 battery divider: +12V_RAW -> R1 -> BAT_ADC -> R2 -> GND
emit_symbol("R1", "Device:R", "100k", R_FP, 160, 148, extra_props=[("Assembly", "装底层")])
emit_symbol("R2", "Device:R", "33k", R_FP, 160, 168, extra_props=[("Assembly", "装底层")])
r1t = pin_endpoint("Device:R", "1", 160, 148)
r1b = pin_endpoint("Device:R", "2", 160, 148)
r2t = pin_endpoint("Device:R", "1", 160, 168)
r2b = pin_endpoint("Device:R", "2", 160, 168)
emit_wire(r1t[0], r1t[1], r1t[0], r1t[1] - 7.62)
emit_wire(r1t[0], r1t[1] - 7.62, r1t[0] + 5.08, r1t[1] - 7.62)
emit_label("+12V_RAW", r1t[0] + 5.08, r1t[1] - 7.62, angle=0)
emit_wire(r1b[0], r1b[1], r2t[0], r2t[1])
midy = round((r1b[1] + r2t[1]) / 2 / 1.27) * 1.27
emit_junction(r1b[0], midy)
emit_wire(r1b[0], midy, r1b[0] + 7.62, midy)
emit_label("BAT_ADC", r1b[0] + 7.62, midy, angle=0)
emit_wire(r2b[0], r2b[1], r2b[0], r2b[1] + 7.62)
emit_power("GND", r2b[0], r2b[1] + 7.62, rot=0)

# 右列 x=200
cap("C2", "100nF", "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm", 200, 55)

# R3 + D2 power indicator: +5V -> R3 -> D2(A) -> D2(K) -> GND
emit_symbol("R3", "Device:R", "1k", R_FP, 200, 95, extra_props=[("Assembly", "装底层")])
emit_symbol("D2", "Device:LED", "LED 3mm", "LED_THT:LED_D3.0mm", 200, 115,
            rot=90, extra_props=[("Assembly", "装底层")])
r3t = pin_endpoint("Device:R", "1", 200, 95)
r3b = pin_endpoint("Device:R", "2", 200, 95)
d2a = pin_endpoint("Device:LED", "2", 200, 115, rot=90)  # A on top after rot90
d2k = pin_endpoint("Device:LED", "1", 200, 115, rot=90)
emit_wire(r3t[0], r3t[1], r3t[0], r3t[1] - 7.62)
emit_power("+5V", r3t[0], r3t[1] - 7.62, rot=0)
emit_wire(r3b[0], r3b[1], d2a[0], d2a[1])
emit_wire(d2k[0], d2k[1], d2k[0], d2k[1] + 7.62)
emit_power("GND", d2k[0], d2k[1] + 7.62, rot=0)

# R_EN pull-up: +5V_SERVO -> R_EN -> 4G_EN
emit_symbol("R_EN", "Device:R", "10k", R_FP, 200, 148, extra_props=[("Assembly", "装底层")])
rent = pin_endpoint("Device:R", "1", 200, 148)
renb = pin_endpoint("Device:R", "2", 200, 148)
emit_wire(rent[0], rent[1], rent[0], rent[1] - 7.62)
emit_wire(rent[0], rent[1] - 7.62, rent[0] + 5.08, rent[1] - 7.62)
emit_label("+5V_SERVO", rent[0] + 5.08, rent[1] - 7.62, angle=0)
emit_wire(renb[0], renb[1], renb[0], renb[1] + 7.62)
emit_wire(renb[0], renb[1] + 7.62, renb[0] + 5.08, renb[1] + 7.62)
emit_label("4G_EN", renb[0] + 5.08, renb[1] + 7.62, angle=0)

# R0 star point: +5V_SERVO -> R0 -> +5V
emit_symbol("R0", "Device:R", "0Ω", R_FP, 200, 178, extra_props=[("Assembly", "装底层")])
r0t = pin_endpoint("Device:R", "1", 200, 178)
r0b = pin_endpoint("Device:R", "2", 200, 178)
emit_wire(r0t[0], r0t[1], r0t[0], r0t[1] - 7.62)
emit_wire(r0t[0], r0t[1] - 7.62, r0t[0] + 5.08, r0t[1] - 7.62)
emit_label("+5V_SERVO", r0t[0] + 5.08, r0t[1] - 7.62, angle=0)
emit_wire(r0b[0], r0b[1], r0b[0], r0b[1] + 7.62)
emit_power("+5V", r0b[0], r0b[1] + 7.62, rot=180)

# PWR_FLAGs, one per rail — 底部
emit_text("电源轨说明: +12V/+5V/+3V3/GND 驱动来自外部模块; +12V_RAW/+5V_SERVO 经 D1/R0 产生", 45, 230)
for rail, fx in (("+12V", 45), ("+5V", 68), ("+3V3", 91)):
    emit_power(rail, fx, 216, rot=0)
    emit_wire(fx, 216, fx, 221.08)
    emit_pwr_flag(fx, 221.08, rot=0)
emit_pwr_flag(118, 216, rot=0)
emit_wire(118, 216, 118, 221.08)
emit_power("GND", 118, 221.08, rot=0)
for rail, fx in (("+12V_RAW", 145), ("+5V_SERVO", 172)):
    emit_pwr_flag(fx, 216, rot=0)
    emit_wire(fx, 216, fx, 221.08)
    emit_wire(fx, 221.08, fx + 5.08, 221.08)
    emit_label(rail, fx + 5.08, 221.08, angle=0)

# --- 模块说明图例（左下角，标题栏左侧空区） ---
emit_text("模块说明:", 15, 230, size=1.0)
ly = 233.4
for ref, note in CONN_NOTES:
    txt = "; ".join(note.split("\n"))
    emit_text(f"{ref}: {txt}", 15, ly, size=1.0)
    ly += 2.54

# ---------------------------------------------------------------------------
# Assemble schematic file
# ---------------------------------------------------------------------------

def build_schematic():
    out = []
    out.append("(kicad_sch")
    out.append("\t(version 20260101)")
    out.append('\t(generator "eeschema")')
    out.append('\t(generator_version "10.0")')
    out.append(f'\t(uuid "{SHEET_UUID}")')
    out.append('\t(paper "A3")')
    out.append("\t(title_block")
    out.append('\t\t(title "WALLE Mainboard (模块插接底板, 63×80mm, 2层板)")')
    out.append('\t\t(date "2026-08")')
    out.append('\t\t(rev "Rev A")')
    out.append('\t\t(company "walle-replica")')
    out.append("\t)")
    out.append("\t(lib_symbols")
    for full in SYMBOLS:
        sym = SYMBOLS[full]
        text = sym["text"]
        # prefix top-level symbol name with library nickname
        text = text.replace(f'(symbol "{sym["name"]}"', f'(symbol "{full}"', 1)
        # Library symbols already end with '(embedded_fonts no)' and a closing
        # ')', so reindent the whole block verbatim one tab.  The synthetic
        # single-line symbol (Conn_01x15) has no body — finish it by hand.
        lines = text.split("\n")
        if len(lines) == 1:
            # single-line synthetic symbol '(symbol "X")': the inline ')' closes
            # it immediately, so strip it and rebuild a proper 3-line block.
            name = lines[0].rstrip()
            if name.endswith(")"):
                name = name.rstrip(")").rstrip()
            out.append("\t\t" + name)
            out.append("\t\t\t(embedded_fonts no)")
            out.append("\t\t)")
        else:
            out.append("\n".join("\t" + ln for ln in lines))
    out.append("\t)")
    out.extend(ITEMS)
    out.append("\t(sheet_instances")
    out.append('\t\t(path "/"')
    out.append('\t\t\t(page "1")')
    out.append("\t\t)")
    out.append("\t)")
    out.append(")")
    return "\n".join(out) + "\n"




def build_pro():
    # Minimal KiCad 10 project file, modeled on the (since removed)
    # hardware/walle-shield/walle-shield.kicad_pro: same ERC rule severities
    # and pin map, trimmed to the sections that matter for this board.
    pro = {
        "board": {
            "3dviewports": [],
            "design_settings": {
                "defaults": {
                    "board_outline_line_width": 0.05,
                    "copper_line_width": 0.2,
                    "copper_text_size_h": 1.5,
                    "copper_text_size_v": 1.5,
                    "copper_text_thickness": 0.3,
                    "courtyard_line_width": 0.05,
                    "dimension_precision": 4,
                    "dimension_units": 3,
                    "fab_line_width": 0.1,
                    "fab_text_size_h": 1.0,
                    "fab_text_size_v": 1.0,
                    "fab_text_thickness": 0.15,
                    "other_line_width": 0.1,
                    "other_text_size_h": 1.0,
                    "other_text_size_v": 1.0,
                    "other_text_thickness": 0.15,
                    "pads": {"drill": 0.8, "height": 1.27, "width": 1.27},
                    "silk_line_width": 0.1,
                    "silk_text_size_h": 1.0,
                    "silk_text_size_v": 1.0,
                    "silk_text_thickness": 0.1,
                },
                "diff_pair_dimensions": [],
                "drc_exclusions": [],
                "meta": {"version": 2},
                "rules": {
                    "min_clearance": 0.2,
                    "min_copper_edge_clearance": 0.5,
                    "min_hole_clearance": 0.25,
                    "min_hole_to_hole": 0.25,
                    "min_silk_clearance": 0.0,
                    "min_text_height": 0.8,
                    "min_text_thickness": 0.08,
                    "min_through_hole_diameter": 0.3,
                    "min_track_width": 0.25,
                    "min_via_annular_width": 0.1,
                    "min_via_diameter": 0.6,
                    "solder_mask_to_copper_clearance": 0.0,
                },
                "track_widths": [],
                "via_dimensions": [],
            },
            "ipc2581": {"bom_rev": "", "dist": "", "distpn": "",
                        "internal_id": "", "mfg": "", "mpn": "", "sch_revision": ""},
            "layer_pairs": [],
            "layer_presets": [],
            "viewports": [],
        },
        "boards": [],
        "erc": {
            "erc_exclusions": [],
            "meta": {"version": 0},
            "pin_map": [
                [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2],
                [0, 2, 0, 1, 0, 0, 1, 0, 2, 2, 2, 2],
                [0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 2],
                [0, 1, 0, 0, 0, 0, 1, 1, 2, 1, 1, 2],
                [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2],
                [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2],
                [1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 2],
                [0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 2],
                [0, 2, 1, 2, 0, 0, 1, 0, 2, 2, 2, 2],
                [0, 2, 0, 1, 0, 0, 1, 0, 2, 0, 0, 2],
                [0, 2, 1, 1, 0, 0, 1, 0, 2, 0, 0, 2],
                [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2],
            ],
            "rule_severities": {
                "bus_definition_conflict": "error", "bus_entry_needed": "error",
                "bus_to_bus_conflict": "error", "bus_to_net_conflict": "error",
                "different_unit_footprint": "error", "different_unit_net": "error",
                "duplicate_reference": "error", "duplicate_sheet_names": "error",
                "endpoint_off_grid": "warning", "extra_units": "error",
                "field_name_whitespace": "warning", "footprint_filter": "ignore",
                "footprint_link_issues": "warning", "four_way_junction": "ignore",
                "ground_pin_not_ground": "warning", "hier_label_mismatch": "error",
                "isolated_pin_label": "warning", "label_dangling": "error",
                "label_multiple_wires": "warning", "lib_symbol_issues": "warning",
                "lib_symbol_mismatch": "warning", "missing_bidi_pin": "warning",
                "missing_input_pin": "warning", "missing_power_pin": "error",
                "missing_unit": "warning", "multiple_net_names": "warning",
                "net_not_bus_member": "warning", "no_connect_connected": "warning",
                "no_connect_dangling": "warning", "pin_not_connected": "error",
                "pin_not_driven": "error", "pin_to_pin": "warning",
                "power_pin_not_driven": "error", "same_local_global_label": "warning",
                "similar_label_and_power": "warning", "similar_labels": "warning",
                "similar_power": "warning", "simulation_model_issue": "ignore",
                "single_global_label": "ignore", "stacked_pin_name": "warning",
                "unannotated": "error", "unconnected_wire_endpoint": "warning",
                "undefined_netclass": "error", "unit_value_mismatch": "error",
                "unresolved_variable": "error", "wire_dangling": "error",
            },
        },
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": "walle-mainboard.kicad_pro", "version": 3},
        "net_settings": {
            "classes": [{
                "bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25,
                "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Default",
                "pcb_color": "rgba(0, 0, 0, 0.000)", "priority": 2147483647,
                "schematic_color": "rgba(0, 0, 0, 0.000)", "track_width": 0.2,
                "tuning_profile": "", "via_diameter": 0.6, "via_drill": 0.3,
                "wire_width": 6,
            }],
            "meta": {"version": 5},
            "net_colors": None,
            "netclass_assignments": None,
            "netclass_patterns": [],
        },
        "pcbnew": {"last_paths": {"idf": "", "netlist": "", "plot": "",
                                  "specctra_dsn": "", "vrml": ""},
                   "page_layout_descr_file": ""},
        "schematic": {
            "annotate_start_num": 0,
            "annotation": {"method": 0, "sort_order": 0},
            "bom_export_filename": "${PROJECTNAME}.csv",
            "bus_aliases": {},
            "connection_grid_size": 50.0,
            "drawing": {
                "default_line_thickness": 6.0, "default_text_size": 50.0,
                "field_names": [], "intersheets_ref_own_page": False,
                "intersheets_ref_prefix": "", "intersheets_ref_short": False,
                "intersheets_ref_show": False, "intersheets_ref_suffix": "",
                "junction_size_choice": 3, "label_size_ratio": 0.375,
                "pin_symbol_size": 25.0, "text_offset_ratio": 0.15,
            },
            "legacy_lib_dir": "",
            "legacy_lib_list": [],
            "meta": {"version": 1},
            "page_layout_descr_file": "",
            "plot_directory": "",
            "reuse_designators": True,
            "subpart_first_id": 65,
            "subpart_id_separator": 0,
            "top_level_sheets": [{
                "filename": "walle-mainboard.kicad_sch",
                "name": "walle-mainboard",
                "uuid": SHEET_UUID,
            }],
            "used_designators": "",
            "variants": [],
        },
        "sheets": [],
        "text_variables": {},
    }
    return json.dumps(pro, indent=2, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------------------
# Connectivity check: re-parse generated schematic, diff against spec tables
# ---------------------------------------------------------------------------

# Independently hand-typed expected tables (do NOT reuse the generation
# constants, so transcription slips in either are caught by the diff).
# "." = no-connect, power rails written plainly.
EXPECTED_TEXT = {
    "J1A": "+3V3 . . MIC_WS MIC_SCK MIC_SD DAC_DIN DAC_BCK DAC_LCK MOT_AIN1 MOT_AIN2 . 4G_RX . CAM_TX CAM_RX MOT_PWMB MOT_BIN1 MOT_BIN2 TFT_SCL +5V GND",
    "J1B": "GND UART0_TX UART0_RX BAT_ADC 4G_TX TFT_BLK BTN_RST TFT_DC BTN_VDN BTN_VUP . . . BTN_BOOT TFT_RES . TFT_SDA I2C_SCL I2C_SDA MOT_PWMA . .",
    "J2A": "GND . . I2C_SCL I2C_SDA +5V +5V_SERVO",
    "J2B": "GND . . I2C_SCL I2C_SDA +5V +5V_SERVO",
    "J3A": "GND MOT_PWMB MOT_BIN2 MOT_BIN1 +5V MOT_AIN1 MOT_AIN2 MOT_PWMA",
    "J3B": "GND MOTB_1 MOTB_2 MOTA_2 MOTA_1 GND +5V +12V",
    "J4": ". DAC_BCK DAC_DIN DAC_LCK GND +5V . . . . +3V3 GND DAC_R GND DAC_L",
    "J5": "+5V GND DAC_L DAC_R SPK_L+ SPK_L- SPK_R+ SPK_R-",
    "J6A": "+12V GND",
    "J6B": "+5V_SERVO GND",
    "J-BAT": "+12V_RAW GND",
    "J-MOTA": "MOTA_1 MOTA_2",
    "J-MOTB": "MOTB_1 MOTB_2",
    "J-PANEL": "+5V +3V3 GND GND SPK_L+ SPK_L- SPK_R+ SPK_R- MIC_SCK MIC_WS MIC_SD BTN_BOOT BTN_VUP BTN_VDN BTN_RST TFT_SCL TFT_SDA TFT_RES TFT_DC TFT_BLK GND +3V3",
    "J-CAM": "+5V GND CAM_TX CAM_RX",
    "J-DBG": "UART0_TX UART0_RX GND",
    "J-4GA": "+5V_SERVO GND 4G_RX 4G_TX 4G_EN .",
    "J-4GB": "+5V_SERVO GND . . 4G_NET .",
}
EXPECTED = {}
for _ref, _row in EXPECTED_TEXT.items():
    _nets = []
    for _tok in _row.split():
        if _tok == ".":
            _nets.append("NC")
        elif _tok in ("+12V", "+5V", "+3V3", "GND"):
            _nets.append("PWR:" + _tok)
        else:
            _nets.append(_tok)
    EXPECTED[_ref] = _nets

# cross-check generation tables against the independent literals
for _ref, _nets in EXPECTED.items():
    _gen = {
        "J1A": J1A, "J1B": J1B, "J2A": J2, "J2B": J2, "J3A": J3A, "J3B": J3B,
        "J4": J4, "J5": J5, "J6A": J6A, "J6B": J6B, "J-BAT": JBAT,
        "J-MOTA": JMOTA, "J-MOTB": JMOTB, "J-PANEL": JPANEL, "J-CAM": JCAM,
        "J-DBG": JDBG, "J-4GA": J4GL, "J-4GB": J4GR,
    }[_ref]
    assert _gen == _nets, f"table mismatch between generator and checker for {_ref}: {_gen} vs {_nets}"


def check():
    text = SCH_PATH.read_text(encoding="utf-8")

    def pt(x, y):
        # quantize to 0.01mm as ints; all generated coordinates are 2-decimal
        return (int(round(float(x) * 100)), int(round(float(y) * 100)))

    # embedded lib pin geometry
    libpins = {}
    for m in re.finditer(r'\(symbol "((?:Connector_Generic|Device|power):[^"]+)"(.*?)\n\t\t\t\(embedded_fonts', text, re.S):
        full, body = m.group(1), m.group(2)
        pins, _ = parse_symbol_geometry(body, full.split(":", 1)[1])
        libpins[full] = pins

    # instances: lib_id, at, rotation, reference, value
    insts = []
    for m in re.finditer(
        r'\t\(symbol\n\t\t\(lib_id "([^"]+)"\)\n\t\t\(at ([-\d.]+) ([-\d.]+) (\d+)\)(.*?)\n\t\)\n', text, re.S):
        lib, x, y, rot, body = m.group(1), float(m.group(2)), float(m.group(3)), int(m.group(4)), m.group(5)
        ref = re.search(r'\(property "Reference" "([^"]*)"', body).group(1)
        val = re.search(r'\(property "Value" "([^"]*)"', body).group(1)
        insts.append({"lib": lib, "x": x, "y": y, "rot": rot, "ref": ref, "val": val})

    wires = []
    for m in re.finditer(r'\(wire\n\t\t\(pts\n\t\t\t\(xy ([-\d.]+) ([-\d.]+)\) \(xy ([-\d.]+) ([-\d.]+)\)', text):
        wires.append((pt(m.group(1), m.group(2)), pt(m.group(3), m.group(4))))

    ncs = {pt(m.group(1), m.group(2)) for m in re.finditer(r'\(no_connect\n\t\t\(at ([-\d.]+) ([-\d.]+)\)', text)}
    labels = {}
    for m in re.finditer(r'\(global_label "([^"]+)"\n\t\t\(shape \S+\)\n\t\t\(at ([-\d.]+) ([-\d.]+) (\d+)\)', text):
        labels.setdefault(pt(m.group(2), m.group(3)), m.group(1))

    # union-find over points
    parent = {}

    def find(p):
        parent.setdefault(p, p)
        while parent[p] != p:
            parent[p] = parent[parent[p]]
            p = parent[p]
        return p

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    def on_segment(p, a, b):
        if a[0] == b[0]:
            return p[0] == a[0] and min(a[1], b[1]) <= p[1] <= max(a[1], b[1])
        if a[1] == b[1]:
            return p[1] == a[1] and min(a[0], b[0]) <= p[0] <= max(a[0], b[0])
        return False

    # collect all element points
    elem_points = set(labels) | ncs
    pin_pts = {}   # point -> (ref, pinnum)
    power_pts = {}  # point -> net name
    for inst in insts:
        pins = libpins[inst["lib"]]
        for num, (lx, ly, _pn) in pins.items():
            dx, dy = rot_point(lx, ly, inst["rot"])
            p = pt(inst["x"] + dx, inst["y"] + dy)
            elem_points.add(p)
            if inst["lib"].startswith("power:"):
                power_pts[p] = inst["val"] if inst["val"] != "PWR_FLAG" else "PWR_FLAG"
            else:
                pin_pts[p] = (inst["ref"], num)

    for a, b in wires:
        union(a, b)
    for p in elem_points:
        for a, b in wires:
            if on_segment(p, a, b):
                union(p, a)

    # net name per component
    netname = {}
    for p, name in labels.items():
        netname.setdefault(find(p), name)
    for p, name in power_pts.items():
        if name == "PWR_FLAG":
            continue
        netname.setdefault(find(p), name)

    # build report
    errors = 0
    report = {}
    for ref, table in EXPECTED.items():
        got = {}
        for p, (r, num) in pin_pts.items():
            if r != ref:
                continue
            if p in ncs:
                got[int(num)] = "NC"
            else:
                got[int(num)] = netname.get(find(p), "???")
        exp = {}
        for i, e in enumerate(table):
            exp[i + 1] = "NC" if e == "NC" else (e[4:] if e.startswith("PWR:") else e)
        report[ref] = got
        for n in sorted(exp):
            if got.get(n) != exp[n]:
                print(f"MISMATCH {ref} pin {n}: expected {exp[n]}, got {got.get(n)}")
                errors += 1
    for ref in EXPECTED:
        got = report[ref]
        print(f'{ref}: ' + ", ".join(f"{n}={got[n]}" for n in sorted(got)))
    print(f"\ncheck: {'FAIL' if errors else 'PASS'} ({errors} mismatches)")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    if "--check" in sys.argv:
        sys.exit(check())
    SCH_PATH.write_text(build_schematic(), encoding="utf-8")
    PRO_PATH.write_text(build_pro(), encoding="utf-8")
    print(f"wrote {SCH_PATH.name}, {PRO_PATH.name} (PCB: run generate_pcb.py)")
