#!/usr/bin/env python3
"""generate_pcb.py — Wall-E 主板 PCB (walle-mainboard.kicad_pcb) 生成器。

直接写出 KiCad 10 (version 20260206) 格式板文件：80x63mm 双层板。
X 向右 0..80，Y 向下 0..63。

用法：
    python3 generate_pcb.py            # 生成 + 固定骨架/焊盘间距自检 + A* 布线
    python3 generate_pcb.py --check    # 重新解析产出文件，逐盘网络 diff + 连通性检查
    <KiCad python> generate_pcb.py --fill   # pcbnew ZONE_FILLER 回填铺铜（见文件尾）

坐标变换（KiCad 10 实测，F/B 两侧一致，镜像只体现在 footprint layer 上）：
    board_x = ax + lx*cos(t) + ly*sin(t)
    board_y = ay - lx*sin(t) + ly*cos(t)
焊盘 at 为封装局部坐标（未旋转），加载器按上式展开。

路由器：RES=0.127mm 网格 A*（8 邻接、禁切角、拐弯罚、换层过孔），
net-aware 间距由 scipy distance_transform_edt 提供（最近铜皮网络判别）。
"""
import math
import os
import sys
import uuid
import heapq

import numpy as np
from scipy.ndimage import distance_transform_edt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_schematic as gs

HERE = os.path.dirname(os.path.abspath(__file__))
PCB_PATH = os.path.join(HERE, "walle-mainboard.kicad_pcb")

BOARD_W, BOARD_H = 80.0, 63.0
RES = 0.127                       # router grid (mm); 2.54mm = 20 cells
GW, GH = int(math.ceil(BOARD_W / RES)) + 1, int(math.ceil(BOARD_H / RES)) + 1
CLEAR = 0.15                      # netclass clearance (mm); 0.15 opens the
                                  # 0.76mm ESP32 socket pin gaps (0.2 left no room)
VIA_DIA, VIA_DRILL = 0.8, 0.4
VIA_COST = 8.0                    # grid cells (~1mm) penalty per layer switch
DIJK_BEND = 0.3                   # per-direction-change penalty (congestion router)
DIJK_CONG = 0.12                  # per-cell multiplier on clearance-tightness
DIJK_SOFT = 5.0                   # cells of "soft" congestion margin beyond need
BEND_COST = 2.0

# antenna keepout (tracks/vias/pour forbidden; pads allowed).
# ESP32 module mirrored 180° so its antenna faces WEST (x 0..8); the east
# corridor (x > 55.4) is now free as the main routing channel.
KEEPOUT = (0.0, 7.2, 8.0, 28.0)   # x0, y0, x1, y1

# +5V_SERVO B.Cu pour rects (stamped as net copper before routing).
# Disabled: the band under the LU9685 blocked all southbound signal routing;
# servo current rides the 4mm B run + 2.5mm F track + off-board DC-DC->LU9685
# terminal backup wire (see 主板设计说明.md §1 J2).
SERVO_POUR_RECTS = []

EDGE_MARGIN = 0.35                # router keeps copper this far from board edge


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def fmt(v):
    """mm float -> KiCad token (up to 4 decimals, trailing zeros stripped)."""
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def uid():
    return str(uuid.uuid4())


def strip_pwr(n):
    if n == "NC":
        return None
    return n[4:] if n.startswith("PWR:") else n


def rot(lx, ly, deg):
    """Verified KiCad-10 file transform (same for F and B footprints)."""
    t = math.radians(deg)
    return (lx * math.cos(t) + ly * math.sin(t),
            -lx * math.sin(t) + ly * math.cos(t))


# ---------------------------------------------------------------------------
# footprint types
#   pads: (num, lx, ly, sx, sy, drill, shape)  shape in {circle, rect}
#   kind: 'pth' | 'smd' | 'npth'
# ---------------------------------------------------------------------------

def _row(n, pitch=2.54, dia=1.7, drill=1.0):
    return [(str(i + 1), 0.0, pitch * i, dia, dia, drill,
             "rect" if i == 0 else "circle") for i in range(n)]


FP_TYPES = {
    "sock22": dict(kind="pth", pads=_row(22)),
    "sock8":  dict(kind="pth", pads=_row(8)),
    "sock7":  dict(kind="pth", pads=_row(7)),
    "sock6":  dict(kind="pth", pads=_row(6)),
    "hdr8":   dict(kind="pth", pads=_row(8)),
    "xh2":    dict(kind="pth", pads=[("1", 0, 0, 1.7, 1.7, 1.0, "rect"),
                                     ("2", 2.5, 0, 1.7, 1.7, 1.0, "circle")]),
    "xh4":    dict(kind="pth", pads=[(str(i + 1), 2.5 * i, 0, 1.7, 1.7, 0.95,
                                      "rect" if i == 0 else "circle")
                                     for i in range(4)]),
    "p600":   dict(kind="pth", pads=[("1", 0, 0, 3.2, 3.2, 1.6, "rect"),
                                     ("2", 12.7, 0, 3.2, 3.2, 1.6, "circle")]),
    "rax":    dict(kind="pth", pads=[("1", 0, 0, 1.6, 1.6, 0.8, "rect"),
                                     ("2", 7.62, 0, 1.6, 1.6, 0.8, "circle")]),
    "cp8":    dict(kind="pth", pads=[("1", 0, 0, 1.8, 1.8, 0.9, "rect"),
                                     ("2", 3.5, 0, 1.8, 1.8, 0.9, "circle")]),
    "cdisc5": dict(kind="pth", pads=[("1", 0, 0, 1.6, 1.6, 0.8, "rect"),
                                     ("2", 5.0, 0, 1.6, 1.6, 0.8, "circle")]),
    "led3":   dict(kind="pth", pads=[("1", 0, 0, 1.8, 1.8, 0.9, "rect"),
                                     ("2", 2.54, 0, 1.8, 1.8, 0.9, "circle")]),
    "j6":     dict(kind="pth", pads=[("1", 0, 0, 3.2, 3.2, 2.0, "rect"),
                                     ("2", 0, 18.0, 3.2, 3.2, 2.0, "circle")]),
    "jbat":   dict(kind="pth", pads=[("1", 0, 0, 3.2, 3.2, 2.0, "rect"),
                                     ("2", 0, 5.08, 3.2, 3.2, 2.0, "circle")]),
    "jdbg":   dict(kind="pth", pads=[("1", 0, 0, 2.0, 2.0, 1.0, "rect"),
                                     ("2", 2.54, 0, 2.0, 2.0, 1.0, "circle"),
                                     ("3", 5.08, 0, 2.0, 2.0, 1.0, "circle")]),
    # PCM5102 module (32x18): 6 pins on the LEFT (top->bottom: SCK BCK DIN
    # LCK GND VIN) + 9 pins on the TOP (left->right: FLT DEMP XSMT FMT A3V3
    # AGND ROUT AGND LROUT).  Top pin1 is 6.3mm right of / 1.3mm above left pin1.
    "pcm5102": dict(kind="pth",
                    pads=[(str(i + 1), 0.0, -2.54 * i, 1.7, 1.7, 1.0,
                           "rect" if i == 0 else "circle") for i in range(6)] +
                         [(str(i + 7), 6.3 + 2.54 * i, 1.3, 1.7, 1.7, 1.0,
                           "circle") for i in range(9)]),
    # FFC 22P 1.0mm (JUSHUO AFA07-S22FCA-00 like), local coords chosen so that
    # theta=90 lands pin k (1-based) at board (1.125, 36.5+k-1) for anchor (4.5,47)
    "ffc22":  dict(kind="smd",
                   pads=[(str(k + 1), 10.5 - k, -3.375, 0.6, 1.8, None, "rect")
                         for k in range(22)] +
                        [("MP1", 12.85, -1.205, 2.6, 3.0, None, "rect"),
                         ("MP2", -12.85, -1.205, 2.6, 3.0, None, "rect")]),
}

# ---------------------------------------------------------------------------
# placements: (ref, type, value, x, y, theta_deg, side, pin_nets)
#   side 'F' or 'B'; pin_nets aligned to pad order of FP_TYPES
# ---------------------------------------------------------------------------

def _nets(table):
    return [strip_pwr(n) for n in table]


PLACEMENTS = [
    # ---- F.Cu ----
    ("J1A", "sock22", "ESP32-S3 L", 2.032, 6.604, 90, "F", _nets(gs.J1A)),
    ("J1B", "sock22", "ESP32-S3 R", 2.032, 32.004, 90, "F", _nets(gs.J1B)),
    ("J2A", "sock7", "LU9685 A", 7.62, 54.24, 180, "F", _nets(gs.J2)),
    ("J2B", "sock7", "LU9685 B", 68.62, 54.24, 180, "F", _nets(gs.J2)),
    ("J-4GA", "sock6", "ML307R L", 66.5, 14.7, 180, "F", _nets(gs.J4GL)),
    ("J-4GB", "sock6", "ML307R R", 69.04, 14.7, 180, "F", _nets(gs.J4GR)),
    ("J-CAM", "xh4", "CAM XHB4", 76.2, 32.004, 270, "F", _nets(gs.JCAM)),
    ("J-MOTA", "xh2", "MOT-A XH2", 3.2, 54.7, 0, "F", _nets(gs.JMOTA)),
    ("J-MOTB", "xh2", "MOT-B XH2", 77.0, 48.0, 270, "F", _nets(gs.JMOTB)),
    ("J-BAT", "jbat", "12V IN", 63.5, 2.3, 0, "F", _nets(gs.JBAT)),
    # ---- B.Cu ----
    ("J6A", "j6", "DCDC IN", 18.0, 10.5, 0, "B", _nets(gs.J6A)),
    ("J6B", "j6", "DCDC OUT", 56.0, 10.5, 0, "B", _nets(gs.J6B)),
    ("J5", "hdr8", "PAM8406", 72.5, 12.5, 0, "B", _nets(gs.J5)),
    ("J4", "pcm5102", "PCM5102", 10.0, 55.0, 0, "B", _nets(gs.J4)),
    ("J3A", "sock8", "TB6612 L", 46.0, 60.0, 180, "B", _nets(gs.J3A)),
    ("J3B", "sock8", "TB6612 R", 62.0, 60.0, 180, "B", _nets(gs.J3B)),
    ("J-PANEL", "ffc22", "FFC22", 4.5, 47.3, 90, "B", _nets(gs.JPANEL)),
    ("D1", "p600", "P600", 60.0, 8.0, 270, "B", ["+12V", "+12V_RAW"]),
    ("C1", "cp8", "470uF", 37.5, 16.0, 270, "B", ["+5V_SERVO", "GND"]),
    ("C2", "cdisc5", "100nF", 33.0, 24.0, 270, "B", ["+5V_SERVO", "GND"]),
    ("C4G", "cp8", "470uF", 66.0, 28.0, 180, "B", ["+5V_SERVO", "GND"]),
    ("R0", "rax", "0R", 30.0, 12.0, 180, "B", ["+5V_SERVO", "+5V"]),
    ("R_EN", "rax", "100k", 71.0, 40.0, 0, "B", ["+5V_SERVO", "4G_EN"]),
    ("R1", "rax", "100k", 50.0, 58.0, 0, "B", ["+12V_RAW", "BAT_ADC"]),
    ("R2", "rax", "20k", 66.0, 58.0, 90, "B", ["BAT_ADC", "GND"]),
    ("R3", "rax", "1k", 64.0, 60.0, 180, "B", ["+5V", "Net-(R3-D2)"]),
    ("D2", "led3", "LED", 76.5, 53.5, 270, "B", ["GND", "Net-(R3-D2)"]),
    ("J-DBG", "jdbg", "DBG", 28.0, 61.0, 0, "B", _nets(gs.JDBG)),
]

MOUNTING_HOLES = [(4.0, 4.0), (76.0, 4.0), (14.0, 59.0), (76.0, 59.0)]

# ---------------------------------------------------------------------------
# fixed routing skeleton (net, layer, width, [polyline points])
# hand-verified against pads/keepout; generator re-validates clearances
# ---------------------------------------------------------------------------

FIXED = [
    ("+5V_SERVO", "B", 4.0, [(56, 10.5), (56, 12), (47.7, 12), (25.5, 12)]),
    ("+5V_SERVO", "B", 4.0, [(56, 10.5), (56, 16.5), (68.9, 16.5), (68.9, 30.5)]),
    ("+5V_SERVO", "B", 1.5, [(37.5, 12), (37.5, 16)]),
    ("+5V_SERVO", "B", 1.5, [(33, 12), (33, 24)]),
    ("+5V_SERVO", "B", 1.5, [(66.5, 14.7), (66.5, 34)]),
    ("+5V_SERVO", "B", 1.5, [(69.04, 14.7), (69.04, 34)]),
    ("+5V_SERVO", "F", 2.5, [(7.62, 34.6), (72.6, 34.6)]),
    ("+5V_SERVO", "F", 1.5, [(7.62, 34.6), (7.62, 39.0)]),     # -> J2A V+ (top)
    ("+5V_SERVO", "F", 1.5, [(68.62, 34.6), (68.62, 39.0)]),   # -> J2B V+ (top)
    ("+5V_SERVO", "F", 1.5, [(72.6, 34.6), (72.6, 38.5)]),
    ("+5V_SERVO", "F", 1.5, [(71, 35.85), (71, 40)]),   # R_EN.1 tap
]

FIXED_VIAS = [  # (net, x, y)
    ("+5V_SERVO", 66.5, 34.0),
    ("+5V_SERVO", 69.04, 34.0),
]

# ---------------------------------------------------------------------------
# nets left for the A* router: (net, width) in routing order
# (GND intentionally absent: handled by F+B copper pours, verified post-fill)
# ---------------------------------------------------------------------------

ROUTE_NETS = [
    # thin signal nets FIRST — they need the tight pin-gap corridors; fat
    # power nets are inner-plane zones (In2), not routed here.
    ("CAM_TX", 0.4), ("CAM_RX", 0.4),
    ("4G_RX", 0.4), ("4G_TX", 0.4),
    ("UART0_TX", 0.4), ("UART0_RX", 0.4),
    ("TFT_SCL", 0.4), ("TFT_SDA", 0.4), ("TFT_RES", 0.4), ("TFT_DC", 0.4),
    ("TFT_BLK", 0.4),
    ("BTN_RST", 0.4), ("BTN_VDN", 0.4), ("BTN_VUP", 0.4), ("BTN_BOOT", 0.4),
    ("MIC_SCK", 0.4), ("MIC_WS", 0.4), ("MIC_SD", 0.4),
    ("I2C_SDA", 0.4), ("I2C_SCL", 0.4),
    ("MOT_PWMA", 0.4), ("MOT_AIN1", 0.4), ("MOT_AIN2", 0.4),
    ("MOT_PWMB", 0.4), ("MOT_BIN1", 0.4), ("MOT_BIN2", 0.4),
    ("DAC_BCK", 0.4), ("DAC_DIN", 0.4), ("DAC_LCK", 0.4),
    ("BAT_ADC", 0.4), ("4G_EN", 0.4), ("Net-(R3-D2)", 0.4),
    ("SPK_L+", 0.8), ("SPK_L-", 0.8), ("SPK_R+", 0.8), ("SPK_R-", 0.8),
    ("DAC_L", 0.8), ("DAC_R", 0.8),
    ("MOTA_1", 1.5), ("MOTA_2", 1.5), ("MOTB_1", 1.5), ("MOTB_2", 1.5),
]

# ---------------------------------------------------------------------------
# silkscreen labels
# ---------------------------------------------------------------------------

SILK_F = [
    ("J1 ESP32-S3", 28.0, 18.0, 0),
    ("J2 LU9685 pitch 60.96 TBC", 38.0, 47.0, 0),
    ("ANT KEEPOUT", 54.0, 17.0, 90),
    ("J-CAM: UART cross on CAM board", 74.0, 44.0, 90),
]
SILK_B = [
    ("J3 TB6612 pitch 15.24 TBC", 52.0, 58.0, 0),
    ("J4 PCM5102", 19.0, 44.0, 0),
    ("J5 PAM8406", 60.0, 14.0, 0),
    ("J6 DC-DC", 26.0, 17.0, 0),
]


# ---------------------------------------------------------------------------
# net registry + board model
# ---------------------------------------------------------------------------

class NetDB:
    def __init__(self):
        self.names = []          # index -> name
        self.ids = {}            # name -> index

    def get(self, name):
        if name is None:
            return -2            # generic obstacle / no-net copper
        if name not in self.ids:
            self.ids[name] = len(self.names)
            self.names.append(name)
        return self.ids[name]


NETS = NetDB()

# pads: list of dict(ref, num, net, layer('F'/'B'/'P'), x, y, sx, sy, shape)
PADS = []


def build_pads():
    """Expand placements to absolute pad positions (verified transform)."""
    for ref, ftype, _val, ax, ay, th, side, nets in PLACEMENTS:
        spec = FP_TYPES[ftype]
        if len(nets) < len(spec["pads"]):
            nets = list(nets) + [None] * (len(spec["pads"]) - len(nets))  # MP pads
        assert len(spec["pads"]) == len(nets), (ref, ftype)
        for (num, lx, ly, sx, sy, drill, shape), net in zip(spec["pads"], nets):
            dx, dy = rot(lx, ly, th)
            if spec["kind"] == "pth":
                layer = "P"          # plated through: copper on both layers
            elif spec["kind"] == "npth":
                layer = "N"
            else:
                layer = side         # SMD: one side only
            PADS.append(dict(ref=ref, num=num, net=net, layer=layer,
                             x=ax + dx, y=ay + dy, sx=sx, sy=sy, shape=shape,
                             rot=th % 180 != 0))  # rot90: swap sx/sy at stamp


# ---------------------------------------------------------------------------
# grid stamping
# ---------------------------------------------------------------------------

FREE, OBST = -1, -2


class Board:
    def __init__(self):
        self.netmap = [np.full((GH, GW), FREE, dtype=np.int32) for _ in "FB"]
        # per-net pad cells for connectivity tracking: net -> list of masks
        self.pad_cells = {}        # nid -> list of (layer_idx, np.bool_ mask)

    # -- coordinate helpers
    @staticmethod
    def gx(x):
        return int(round(x / RES))

    @staticmethod
    def gy(y):
        return int(round(y / RES))

    def _stamp_rect(self, li, cx, cy, hx, hy, val):
        x0 = max(0, self.gx(cx - hx) - 1)
        x1 = min(GW - 1, self.gx(cx + hx) + 1)
        y0 = max(0, self.gy(cy - hy) - 1)
        y1 = min(GH - 1, self.gy(cy + hy) + 1)
        if x1 < x0 or y1 < y0:
            return None
        xs = np.arange(x0, x1 + 1) * RES
        ys = np.arange(y0, y1 + 1) * RES
        inside = ((np.abs(xs[None, :] - cx) <= hx + 1e-9) &
                  (np.abs(ys[:, None] - cy) <= hy + 1e-9))
        sub = self.netmap[li][y0:y1 + 1, x0:x1 + 1]
        sub[inside] = val
        m = np.zeros((GH, GW), dtype=bool)
        m[y0:y1 + 1, x0:x1 + 1] = inside
        return m

    def _stamp_disk(self, li, cx, cy, r, val):
        x0 = max(0, self.gx(cx - r) - 1)
        x1 = min(GW - 1, self.gx(cx + r) + 1)
        y0 = max(0, self.gy(cy - r) - 1)
        y1 = min(GH - 1, self.gy(cy + r) + 1)
        xs = np.arange(x0, x1 + 1) * RES
        ys = np.arange(y0, y1 + 1) * RES
        inside = ((xs[None, :] - cx) ** 2 + (ys[:, None] - cy) ** 2
                  <= (r + 1e-9) ** 2)
        sub = self.netmap[li][y0:y1 + 1, x0:x1 + 1]
        sub[inside] = val
        m = np.zeros((GH, GW), dtype=bool)
        m[y0:y1 + 1, x0:x1 + 1] = inside
        return m

    def stamp_pad(self, p):
        nid = NETS.get(p["net"])
        layers = (0, 1) if p["layer"] == "P" else \
                 ((0,) if p["layer"] == "F" else (1,))
        hx, hy = p["sx"] / 2, p["sy"] / 2
        if p["rot"] and p["shape"] == "rect" and p["sx"] != p["sy"]:
            hx, hy = hy, hx
        masks = {}
        for li in layers:
            if p["shape"] == "circle":
                m = self._stamp_disk(li, p["x"], p["y"], hx, nid)
            else:
                m = self._stamp_rect(li, p["x"], p["y"], hx, hy, nid)
            masks[li] = m
        if nid >= 0:
            # PTH pads are barrel-connected: group their two layer masks under
            # the same (ref,num) so the router treats them as one physical pad.
            self.pad_cells.setdefault(nid, []).append((p["ref"], p["num"], masks))
        return nid

    def stamp_segment(self, li, x1, y1, x2, y2, w, nid):
        """capsule: disks of radius w/2 along the centerline at 0.4-cell pitch"""
        dist = math.hypot(x2 - x1, y2 - y1)
        steps = max(1, int(dist / (RES * 0.4)) + 1)
        r = w / 2
        for i in range(steps + 1):
            t = i / steps
            self._stamp_disk(li, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, r, nid)

    def stamp_via(self, x, y, nid):
        for li in (0, 1):
            self._stamp_disk(li, x, y, VIA_DIA / 2, nid)

    # -- obstacles
    def stamp_obstacles(self):
        # outside-board / edge margin
        for li in (0, 1):
            nm = self.netmap[li]
            xs = np.arange(GW) * RES
            ys = np.arange(GH) * RES
            outside = ((xs[None, :] < EDGE_MARGIN) | (xs[None, :] > BOARD_W - EDGE_MARGIN) |
                       (ys[:, None] < EDGE_MARGIN) | (ys[:, None] > BOARD_H - EDGE_MARGIN))
            nm[outside] = OBST
            # antenna keepout (tracks/vias; pads will overwrite where allowed)
            x0, y0, x1, y1 = KEEPOUT
            inko = ((xs[None, :] >= x0 - 1e-9) & (xs[None, :] <= x1 + 1e-9) &
                    (ys[:, None] >= y0 - 1e-9) & (ys[:, None] <= y1 + 1e-9))
            nm[inko] = OBST
            # mounting holes: drill 3.2, keep copper CLEAR+0.3 away from hole edge
            for hx, hy_ in MOUNTING_HOLES:
                self._stamp_disk(li, hx, hy_, 1.6 + 0.3, OBST)

    # -- queries used by router / validator
    def other_mask(self, li, nid):
        nm = self.netmap[li]
        return (nm != FREE) & (nm != nid)


def fixed_skeleton_check(board):
    """Stamp everything; validate clearance of each fixed element against
    copper already on the map.  Returns list of violation strings."""
    errors = []

    def other_copper(li, nid):
        return board.other_mask(li, nid)

    def check_capsule(li, x1, y1, x2, y2, w, nid, label):
        other = other_copper(li, nid)
        if not other.any():
            return
        dt = distance_transform_edt(~other)          # cells to nearest other
        dist = math.hypot(x2 - x1, y2 - y1)
        steps = max(1, int(dist / (RES * 0.5)) + 1)
        need = (w / 2 + CLEAR) / RES - 0.26          # matches router slack
        for i in range(steps + 1):
            t = i / steps
            cx, cy = x1 + (x2 - x1) * t, y1 + (y2 - y1) * t
            gx, gy = board.gx(cx), board.gy(cy)
            if 0 <= gx < GW and 0 <= gy < GH and dt[gy, gx] < need:
                errors.append(f"{label}: clearance {dt[gy, gx] * RES:.2f}mm "
                              f"< {w / 2 + CLEAR:.2f}mm at ({cx:.2f},{cy:.2f}) layer "
                              f"{'F' if li == 0 else 'B'}")
                return

    # 1. obstacles first
    board.stamp_obstacles()
    # 2. pads (pad-vs-pad clearance checked pairwise by geometry)
    for i, p in enumerate(PADS):
        for q in PADS[i + 1:]:
            if p["net"] and p["net"] == q["net"]:
                continue
            # same-layer overlap test
            shared = ("P" in (p["layer"], q["layer"]) or
                      p["layer"] == q["layer"])
            if not shared:
                continue
            phx, phy = p["sx"] / 2, p["sy"] / 2
            qhx, qhy = q["sx"] / 2, q["sy"] / 2
            if p["rot"] and p["sx"] != p["sy"]:
                phx, phy = phy, phx
            if q["rot"] and q["sx"] != q["sy"]:
                qhx, qhy = qhy, qhx
            dx = abs(p["x"] - q["x"]) - (phx + qhx)
            dy = abs(p["y"] - q["y"]) - (phy + qhy)
            # crude rect-rect clearance (exact for axis-aligned rects)
            gap = max(dx, dy) if (dx > 0 or dy > 0) else max(dx, dy)
            if gap < CLEAR - 1e-6:
                errors.append(f"pad clearance {p['ref']}.{p['num']}({p['net']}) vs "
                              f"{q['ref']}.{q['num']}({q['net']}): gap {gap:.2f}mm")
    for p in PADS:
        board.stamp_pad(p)
    # 2b. +5V_SERVO pour rects count as that net's copper for routing clearance
    servo_id = NETS.get("+5V_SERVO")
    for x0, y0, x1, y1 in SERVO_POUR_RECTS:
        board._stamp_rect(1, (x0 + x1) / 2, (y0 + y1) / 2,
                          (x1 - x0) / 2, (y1 - y0) / 2, servo_id)
    # 3. fixed segments + vias with clearance validation
    for net, layer, w, pts in FIXED:
        nid = NETS.get(net)
        li = 0 if layer == "F" else 1
        for a, b in zip(pts, pts[1:]):
            check_capsule(li, a[0], a[1], b[0], b[1], w, nid,
                          f"{net} {layer} seg {a}->{b}")
            board.stamp_segment(li, a[0], a[1], b[0], b[1], w, nid)
    for net, x, y in FIXED_VIAS:
        nid = NETS.get(net)
        for li in (0, 1):
            check_capsule(li, x, y, x, y, VIA_DIA, nid, f"{net} via ({x},{y})")
        board.stamp_via(x, y, nid)
    return errors



# ---------------------------------------------------------------------------
# A* grid router
# ---------------------------------------------------------------------------

DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
DCOST = [1.0, math.sqrt(2), 1.0, math.sqrt(2), 1.0, math.sqrt(2), 1.0, math.sqrt(2)]


def _bbox(mask):
    ys, xs = np.nonzero(mask)
    return xs.min(), ys.min(), xs.max(), ys.max()


class Router:
    def __init__(self, board):
        self.b = board
        self.failed = []
        self.segments = []       # (net, layer 'F'/'B', width, [(x,y)...])
        self.vias = []           # (net, x, y)

    # -- per-net masks
    def _blocked(self, nid, w):
        # centreline must keep w/2 + CLEAR from other copper.  dt() measures
        # to the nearest other-copper CELL CENTRE, which is up to half a cell
        # inside the copper, so add one full cell of margin (no more "-0.26"
        # slack — that let real <0.15mm gaps through).
        need = (w / 2 + CLEAR) / RES - 0.26
        blocked, dts = [], []
        for li in (0, 1):
            other = self.b.other_mask(li, nid)
            dt = distance_transform_edt(~other)
            blocked.append(dt < need)
            dts.append(dt)
        return blocked, dts

    def _via_ok(self, nid):
        # via centre must keep the full annular-ring radius (VIA_DIA/2) plus
        # CLEAR from other copper, else tracks thread through the ring.
        need = (VIA_DIA / 2 + CLEAR) / RES - 0.26
        ok = np.ones((GH, GW), dtype=bool)
        for li in (0, 1):
            other = self.b.other_mask(li, nid)
            ok &= distance_transform_edt(~other) >= need
        # keep vias inside board with edge margin
        xs = np.arange(GW) * RES
        ys = np.arange(GH) * RES
        edge = ((xs[None, :] < 0.6) | (xs[None, :] > BOARD_W - 0.6) |
                (ys[:, None] < 0.6) | (ys[:, None] > BOARD_H - 0.6))
        ok &= ~edge
        return ok

    def _dijkstra(self, blocked, via_ok, dts, w, src_masks, target):
        """Congestion-aware path search (Dijkstra, no heuristic).

        Costs beyond the base 1-per-cell:
          * bend penalty      — prefer straight runs (less snakey maze-hugging)
          * congestion field  — prefer cells far from other copper, so early
            nets spread out instead of clumping and walling off the corridor
            that later nets need (this was the FFC-west failure mode).
          * via penalty       — prefer staying on one layer.
        Keeps the old budget bound; returns a grid path or None.
        """
        import heapq
        tmask = [target.get(li, np.zeros((GH, GW), dtype=bool)) for li in (0, 1)]
        need = (w / 2 + CLEAR) / RES - 0.26
        cong = [np.clip(need + DIJK_SOFT - dts[li], 0.0, None) for li in (0, 1)]
        dist = [np.full((GH, GW), 1e18) for _ in (0, 1)]
        par = [np.full((GH, GW), -1, np.int32) for _ in (0, 1)]
        pdir = [np.full((GH, GW), -1, np.int8) for _ in (0, 1)]
        heap = []

        def enc(li, x, y):
            return ((li & 1) << 23) | (x << 13) | (y << 4)

        def dec(c):
            return ((c >> 23) & 1, (c >> 13) & 0x3FF, (c >> 4) & 0x1FF)

        for li in (0, 1):
            m = src_masks[li] & ~blocked[li]
            ys, xs = np.nonzero(m)
            for x, y in zip(xs, ys):
                dist[li][y, x] = 0.0
                heapq.heappush(heap, (0.0, li, x, y))
        budget = 1_500_000
        expansions = 0
        found = None
        while heap:
            d, li, x, y = heapq.heappop(heap)
            if d > dist[li][y, x] + 1e-9:
                continue
            if tmask[li][y, x]:
                found = (li, x, y)
                break
            if expansions > budget:
                break
            expansions += 1
            last_nd = pdir[li][y, x]
            for nd in range(8):
                dx, dy = DIRS[nd]
                nx, ny = x + dx, y + dy
                if not (0 <= nx < GW and 0 <= ny < GH):
                    continue
                if blocked[li][ny, nx]:
                    continue
                if nd % 2 == 1 and (blocked[li][y, nx] or blocked[li][ny, x]):
                    continue
                step = 1.0 + DIJK_CONG * cong[li][ny, nx]
                if last_nd != nd:
                    step += DIJK_BEND
                ndist = d + step
                if ndist < dist[li][ny, nx] - 1e-9:
                    dist[li][ny, nx] = ndist
                    par[li][ny, nx] = enc(li, x, y)
                    pdir[li][ny, nx] = nd
                    heapq.heappush(heap, (ndist, li, nx, ny))
            if via_ok[y, x] and not blocked[1 - li][y, x]:
                ndist = d + VIA_COST
                if ndist < dist[1 - li][y, x] - 1e-9:
                    dist[1 - li][y, x] = ndist
                    par[1 - li][y, x] = enc(li, x, y)
                    pdir[1 - li][y, x] = -2
                    heapq.heappush(heap, (ndist, 1 - li, x, y))
        if found is None:
            return None
        path = []
        li, x, y = found
        while True:
            path.append((li, x, y))
            p = par[li][y, x]
            if p < 0:
                break
            li, x, y = dec(p)
        path.reverse()
        return path

    def route_net(self, name, w):
        b = self.b
        nid = NETS.get(name)
        pad_groups = b.pad_cells.get(nid, [])   # (ref, num, {li: mask})
        if not pad_groups:
            return
        blocked, dts = self._blocked(nid, w)
        via_ok = self._via_ok(nid)
        # drop pad cells from the connected base so "pad touches only itself"
        # is not mistaken for connectivity
        pad_union = [np.zeros((GH, GW), dtype=bool) for _ in (0, 1)]
        for _ref, _num, masks in pad_groups:
            for li, m in masks.items():
                pad_union[li] |= m
        # initial connected set: fixed copper of this net ONLY (tracks/vias/
        # pour).  All pads are stamped in netmap, so subtract the pad union —
        # otherwise every pad is its own source and A* returns 1-cell paths.
        connected = [(b.netmap[li] == nid) & ~pad_union[li] for li in (0, 1)]
        has_fixed = any(m.any() for m in connected)
        if not has_fixed:
            # seed a single physical pad as the starting tree
            li0 = next(iter(pad_groups[0][2].keys()))
            m0 = pad_groups[0][2][li0]
            connected = [np.zeros((GH, GW), dtype=bool) for _ in (0, 1)]
            connected[li0] |= m0

        def swept(target_masks):
            """target_masks: {li: mask} of one physical pad; fill any layer
            that touches connected (barrel joins all other layers too)."""
            touched = False
            for li, m in target_masks.items():
                # require TRUE 8-neighbour contact between pad surface and
                # connected copper (edt ~1.0 = orthogonal neighbour).  The old
                # <=1.2 (~0.15 mm) accepted near-touches and skipped routing
                # to the pad, leaving a visible gap (BAT_ADC R2.1, I2C J2A.4).
                around = distance_transform_edt(~m) <= 1.0
                if (around & (connected[li] & ~m)).any():
                    touched = True
            if touched:
                for li, m in target_masks.items():
                    connected[li] |= m
            return touched

        remaining = []
        for _ref, _num, masks in pad_groups:
            if not has_fixed and masks is pad_groups[0][2]:
                continue
            if not swept(masks):
                remaining.append(masks)
        for target in remaining:
            if swept(target):
                continue
            path = self._dijkstra(blocked, via_ok, dts, w, connected, target)
            if path is None:
                self.failed.append(name)
                continue
            self._commit(name, nid, w, path, dts, connected)
            # sweep: pads now touching connected copper join
            for _ref, _num, masks in pad_groups:
                swept(masks)

    def _bfs(self, blocked, via_ok, src_masks, target):
        """Multi-source BFS over both layers (8-neighbour + via layer-jump),
        tracking parents for path reconstruction.  Uniform hop cost — fast
        enough to thread this congested board, vs. the pure-Python A* that
        explored the whole grid and stalled.  Shortest-hop path; _commit's
        string-pulling straightens it and adds vias at layer changes."""
        from collections import deque
        tmask = [target.get(li, np.zeros((GH, GW), dtype=bool))
                 for li in (0, 1)]
        dist = [np.full((GH, GW), -1, np.int32) for _ in (0, 1)]
        par = [np.full((GH, GW), -1, np.int32) for _ in (0, 1)]
        q = deque()

        def enc(li, x, y):
            return ((li & 1) << 23) | (x << 13) | (y << 4)

        for li in (0, 1):
            m = src_masks[li] & ~blocked[li]
            ys, xs = np.nonzero(m)
            for x, y in zip(xs, ys):
                if dist[li][y, x] < 0:
                    dist[li][y, x] = 0
                    q.append((li, x, y))
        budget = 8_000_000
        expansions = 0
        found = None
        while q:
            li, x, y = q.popleft()
            if tmask[li][y, x]:
                found = (li, x, y)
                break
            if expansions > budget:
                break
            expansions += 1
            for nd in range(8):
                dx, dy = DIRS[nd]
                nx, ny = x + dx, y + dy
                if not (0 <= nx < GW and 0 <= ny < GH):
                    continue
                if blocked[li][ny, nx]:
                    continue
                if nd % 2 == 1 and (blocked[li][y, nx] or blocked[li][ny, x]):
                    continue
                if dist[li][ny, nx] < 0:
                    dist[li][ny, nx] = dist[li][y, x] + 1
                    par[li][ny, nx] = enc(li, x, y)
                    q.append((li, nx, ny))
            if via_ok[y, x] and not blocked[1 - li][y, x]:
                if dist[1 - li][y, x] < 0:
                    dist[1 - li][y, x] = dist[li][y, x] + 1
                    par[1 - li][y, x] = enc(li, x, y)
                    q.append((1 - li, x, y))
        if found is None:
            return None
        path = []
        li, x, y = found
        while True:
            path.append((li, x, y))
            p = par[li][y, x]
            if p < 0:
                break
            li = (p >> 23) & 1
            x = (p >> 13) & 0x3FF
            y = (p >> 4) & 0x1FF
        path.reverse()
        return path

    def _line_ok(self, li, ax, ay, bx, by, w, dts):
        """straight centerline (grid cells) keeps w/2+CLEAR to other copper"""
        need = (w + CLEAR) / RES - 0.26
        steps = max(1, int(math.hypot(bx - ax, by - ay) / 0.5) + 1)
        dt = dts[li]
        for i in range(steps + 1):
            t = i / steps
            x = int(round(ax + (bx - ax) * t))
            y = int(round(ay + (by - ay) * t))
            if not (0 <= x < GW and 0 <= y < GH) or dt[y, x] < need:
                return False
        return True

    def _commit(self, name, nid, w, path, dts, connected):
        # split into per-layer runs
        runs = []
        cur = [path[0]]
        for cell in path[1:]:
            if cell[0] != cur[-1][0]:
                runs.append(cur)
                cur = [cell]
            else:
                cur.append(cell)
        runs.append(cur)
        for run in runs:
            li = run[0][0]
            pts = [(x, y) for _, x, y in run]
            # string pulling
            out = [pts[0]]
            i = 0
            while i < len(pts) - 1:
                j = len(pts) - 1
                while j > i + 1 and not self._line_ok(li, *pts[i], *pts[j], w, dts):
                    j -= 1
                out.append(pts[j])
                i = j
            mm = [(x * RES, y * RES) for x, y in out]
            self.segments.append((name, "F" if li == 0 else "B", w, mm))
            for (x1, y1), (x2, y2) in zip(mm, mm[1:]):
                self.b.stamp_segment(li, x1, y1, x2, y2, w, nid)
                m = self.b._stamp_disk(li, x2, y2, w / 2, nid)
            # mark connected
            for (x, y) in pts:
                connected[li][y, x] = True
        # vias at run transitions
        for a, b in zip(runs, runs[1:]):
            _, x, y = b[0]
            self.vias.append((name, x * RES, y * RES))
            self.b.stamp_via(x * RES, y * RES, nid)
            connected[0][y, x] = connected[1][y, x] = True

    def route_all(self):
        for name, w in ROUTE_NETS:
            self.route_net(name, w)


# ---------------------------------------------------------------------------
# emitter
# ---------------------------------------------------------------------------

HEADER = """(kicad_pcb
	(version 20260206)
	(generator "pcbnew")
	(generator_version "10.0")
	(general
		(thickness 1.6)
		(legacy_teardrops no)
	)
	(paper "A3")
	(title_block
		(title "WALLE Mainboard (module carrier, 63x80mm, 4 layers)")
		(date "2026-08")
		(rev "Rev A")
	)
	(layers
		(0 "F.Cu" signal)
		(1 "In1.Cu" signal "GND")
		(2 "In2.Cu" signal "+5V")
		(31 "B.Cu" signal)
		(9 "F.Adhes" user "F.Adhesive")
		(11 "B.Adhes" user "B.Adhesive")
		(13 "F.Paste" user)
		(15 "B.Paste" user)
		(5 "F.SilkS" user "F.Silkscreen")
		(7 "B.SilkS" user "B.Silkscreen")
		(1 "F.Mask" user)
		(3 "B.Mask" user)
		(17 "Dwgs.User" user "User.Drawings")
		(19 "Cmts.User" user "User.Comments")
		(21 "Eco1.User" user "User.Eco1")
		(23 "Eco2.User" user "User.Eco2")
		(25 "Edge.Cuts" user)
		(27 "Margin" user)
		(31 "F.CrtYd" user "F.Courtyard")
		(29 "B.CrtYd" user "B.Courtyard")
		(35 "F.Fab" user)
		(33 "B.Fab" user)
		(39 "User.1" user)
		(41 "User.2" user)
		(43 "User.3" user)
		(45 "User.4" user)
	)
	(setup
		(pad_to_mask_clearance 0)
		(allow_soldermask_bridges_in_footprints no)
		(tenting
			(front yes)
			(back yes)
		)
		(covering
			(front no)
			(back no)
		)
		(plugging
			(front no)
			(back no)
		)
		(capping no)
		(filling no)
	)
"""


def q(s):
    return '"%s"' % s


def emit_footprint(out, ref, ftype, value, ax, ay, th, side, nets):
    spec = FP_TYPES[ftype]
    flayer = "F.Cu" if side == "F" else "B.Cu"
    silk = "F.SilkS" if side == "F" else "B.SilkS"
    fab = "F.Fab" if side == "F" else "B.Fab"
    mirror = "\n\t\t\t\t(justify mirror)" if side == "B" else ""
    at = f"(at {fmt(ax)} {fmt(ay)}" + (f" {fmt(th)}" if th else "") + ")"
    out.append(f'\t(footprint {q("WalleMainboard:" + ftype)}')
    out.append(f'\t\t(layer {q(flayer)})')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append(f'\t\t{at}')
    out.append(f'\t\t(property "Reference" {q(ref)}')
    out.append('\t\t\t(at 0 -2.2 0)')
    out.append(f'\t\t\t(layer {q(silk)})')
    out.append(f'\t\t\t(uuid {q(uid())})')
    out.append('\t\t\t(effects')
    out.append('\t\t\t\t(font (size 1 1) (thickness 0.15))' + mirror.replace("\t", ""))
    out.append('\t\t\t)')
    out.append('\t\t)')
    out.append(f'\t\t(property "Value" {q(value)}')
    out.append('\t\t\t(at 0 2.2 0)')
    out.append(f'\t\t\t(layer {q(fab)})')
    out.append(f'\t\t\t(uuid {q(uid())})')
    out.append('\t\t\t(effects')
    out.append('\t\t\t\t(font (size 1 1) (thickness 0.15))' + ("(justify mirror)" if side == "B" else ""))
    out.append('\t\t\t)')
    out.append('\t\t)')
    out.append('\t\t(duplicate_pad_numbers_are_jumpers no)')
    if len(nets) < len(spec["pads"]):
        nets = list(nets) + [None] * (len(spec["pads"]) - len(nets))
    for (num, lx, ly, sx, sy, drill, shape), net in zip(spec["pads"], nets):
        kind = spec["kind"]
        if kind == "pth":
            attr, layers = "thru_hole", '"*.Cu" "*.Mask"'
        elif kind == "npth":
            attr, layers = "np_thru_hole", '"*.Cu" "*.Mask"'
        else:
            attr = "smd"
            layers = ('"F.Cu" "F.Mask" "F.Paste"' if side == "F"
                      else '"B.Cu" "B.Mask" "B.Paste"')
        lines = [f'\t\t(pad {q(num)} {attr} {shape}',
                 f'\t\t\t(at {fmt(lx)} {fmt(ly)})',
                 f'\t\t\t(size {fmt(sx)} {fmt(sy)})']
        if drill:
            lines.append(f'\t\t\t(drill {fmt(drill)})')
        lines.append(f'\t\t\t(layers {layers})')
        if kind == "pth":
            lines.append('\t\t\t(remove_unused_layers no)')
        if net:
            lines.append(f'\t\t\t(net {q(net)})')
        if kind != "npth":
            lines.append('\t\t\t(thermal_bridge_angle 45)')
        lines.append(f'\t\t\t(uuid {q(uid())})')
        lines.append('\t\t)')
        out.extend(lines)
    out.append('\t)')


def emit_mounting_hole(out, idx, x, y):
    out.append('\t(footprint "MountingHole:MountingHole_3.2mm_M3"')
    out.append('\t\t(layer "F.Cu")')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append(f'\t\t(at {fmt(x)} {fmt(y)})')
    out.append(f'\t\t(property "Reference" "H{idx}"')
    out.append('\t\t\t(at 0 -4.15 0)')
    out.append('\t\t\t(layer "F.SilkS")')
    out.append(f'\t\t\t(uuid {q(uid())})')
    out.append('\t\t\t(effects (font (size 1 1) (thickness 0.15)))')
    out.append('\t\t)')
    out.append('\t\t(property "Value" "MountingHole_3.2mm_M3"')
    out.append('\t\t\t(at 0 4.15 0)')
    out.append('\t\t\t(layer "F.Fab")')
    out.append(f'\t\t\t(uuid {q(uid())})')
    out.append('\t\t\t(effects (font (size 1 1) (thickness 0.15)))')
    out.append('\t\t)')
    out.append('\t\t(attr exclude_from_pos_files exclude_from_bom)')
    out.append('\t\t(duplicate_pad_numbers_are_jumpers no)')
    out.append('\t\t(pad "" np_thru_hole circle')
    out.append('\t\t\t(at 0 0)')
    out.append('\t\t\t(size 3.2 3.2)')
    out.append('\t\t\t(drill 3.2)')
    out.append('\t\t\t(layers "*.Cu" "*.Mask")')
    out.append(f'\t\t\t(uuid {q(uid())})')
    out.append('\t\t)')
    out.append('\t)')


def emit_segment(out, net, layer, w, pts):
    for a, b in zip(pts, pts[1:]):
        if math.hypot(b[0] - a[0], b[1] - a[1]) < 1e-6:
            continue
        out.append('\t(segment')
        out.append(f'\t\t(start {fmt(a[0])} {fmt(a[1])})')
        out.append(f'\t\t(end {fmt(b[0])} {fmt(b[1])})')
        out.append(f'\t\t(width {fmt(w)})')
        out.append(f'\t\t(layer "{layer}.Cu")')
        out.append(f'\t\t(net {q(net)})')
        out.append(f'\t\t(uuid {q(uid())})')
        out.append('\t)')


def emit_via(out, net, x, y):
    out.append('\t(via')
    out.append(f'\t\t(at {fmt(x)} {fmt(y)})')
    out.append(f'\t\t(size {fmt(VIA_DIA)})')
    out.append(f'\t\t(drill {fmt(VIA_DRILL)})')
    out.append('\t\t(layers "F.Cu" "B.Cu")')
    out.append(f'\t\t(net {q(net)})')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append('\t)')


def emit_zone(out, net, layer, pts, priority=0):
    out.append('\t(zone')
    out.append(f'\t\t(net {q(net)})')
    out.append(f'\t\t(layer {q(layer)})')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append('\t\t(hatch edge 0.5)')
    if priority:
        out.append(f'\t\t(priority {priority})')
    out.append('\t\t(connect_pads yes')
    out.append('\t\t\t(clearance 0.25)')
    out.append('\t\t)')
    out.append('\t\t(min_thickness 0.2)')
    out.append('\t\t(fill yes')
    out.append('\t\t\t(thermal_gap 0.5)')
    out.append('\t\t\t(thermal_bridge_width 0.5)')
    out.append('\t\t\t(island_removal_mode 0)')
    out.append('\t\t)')
    out.append('\t\t(polygon')
    out.append('\t\t\t(pts')
    out.append('\t\t\t\t' + ' '.join(f'(xy {fmt(x)} {fmt(y)})' for x, y in pts))
    out.append('\t\t\t)')
    out.append('\t\t)')
    out.append('\t)')


def emit_keepout(out):
    x0, y0, x1, y1 = KEEPOUT
    out.append('\t(zone')
    out.append('\t\t(layers "F.Cu" "B.Cu")')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append('\t\t(name "ANT_KEEPOUT")')
    out.append('\t\t(hatch edge 0.5)')
    out.append('\t\t(connect_pads')
    out.append('\t\t\t(clearance 0)')
    out.append('\t\t)')
    out.append('\t\t(min_thickness 0.25)')
    out.append('\t\t(keepout')
    out.append('\t\t\t(tracks not_allowed)')
    out.append('\t\t\t(vias not_allowed)')
    out.append('\t\t\t(pads allowed)')
    out.append('\t\t\t(copperpour not_allowed)')
    out.append('\t\t\t(footprints allowed)')
    out.append('\t\t)')
    out.append('\t\t(placement')
    out.append('\t\t\t(enabled no)')
    out.append('\t\t\t(sheetname "")')
    out.append('\t\t)')
    out.append('\t\t(fill')
    out.append('\t\t\t(thermal_gap 0.5)')
    out.append('\t\t\t(thermal_bridge_width 0.5)')
    out.append('\t\t\t(island_removal_mode 0)')
    out.append('\t\t)')
    out.append('\t\t(polygon')
    out.append('\t\t\t(pts')
    out.append(f'\t\t\t\t(xy {fmt(x0)} {fmt(y0)}) (xy {fmt(x1)} {fmt(y0)}) '
               f'(xy {fmt(x1)} {fmt(y1)}) (xy {fmt(x0)} {fmt(y1)})')
    out.append('\t\t\t)')
    out.append('\t\t)')
    out.append('\t)')


# power nets carried as inner-plane zones on In2 (In1 = GND).  Only the
# current-heavy +5V_SERVO stays as fat F/B tracks (kept in FIXED).
POWER_ZONES = {
    "+5V":        [(0.4, 0.4), (BOARD_W - 0.4, 0.4), (BOARD_W - 0.4, BOARD_H - 0.4),
                   (0.4, BOARD_H - 0.4)],
    "+3V3":       [(0.4, 5.0), (6.0, 5.0), (6.0, BOARD_H - 0.4), (0.4, BOARD_H - 0.4)],
    "+12V":       [(10.0, 5.0), (65.0, 5.0), (65.0, 45.0), (10.0, 45.0)],
    "+12V_RAW":   [(55.0, 2.0), (66.0, 2.0), (66.0, BOARD_H - 0.4),
                   (55.0, BOARD_H - 0.4)],
}


def plane_vias():
    """Through-vias to drop SMD pads of inner-plane nets onto their plane.
    PTH pads already barrel through and connect.  The inner zones auto-clear
    other nets, so through-vias never short across planes."""
    vias = []
    for p in PADS:
        if p["net"] in POWER_ZONES and p["layer"] in ("F", "B"):
            vias.append((p["net"], p["x"], p["y"]))
    return vias


def emit_gr_line(out, x1, y1, x2, y2, layer, w=0.1):
    out.append('\t(gr_line')
    out.append(f'\t\t(start {fmt(x1)} {fmt(y1)})')
    out.append(f'\t\t(end {fmt(x2)} {fmt(y2)})')
    out.append(f'\t\t(stroke (width {fmt(w)}) (type default))')
    out.append(f'\t\t(layer {q(layer)})')
    out.append(f'\t\t(uuid {q(uid())})')
    out.append('\t)')


def emit_gr_text(out, text, x, y, layer, rot_=0, mirror=False):
    out.append(f'\t(gr_text {q(text)}')
    out.append(f'\t\t(at {fmt(x)} {fmt(y)}' + (f' {fmt(rot_)}' if rot_ else '') + ')')
    out.append(f'\t\t(layer {q(layer)})')
    out.append(f'\t\t(uuid {q(uid())})')
    eff = '\t\t(effects (font (size 1.2 1.2) (thickness 0.2))' + \
          (' (justify mirror))' if mirror else ')')
    out.append(eff)
    out.append('\t)')


def build_board_file(routed_segments, routed_vias):
    out = [HEADER]
    for i, (x, y) in enumerate(MOUNTING_HOLES, 1):
        emit_mounting_hole(out, i, x, y)
    for ref, ftype, value, ax, ay, th, side, nets in PLACEMENTS:
        emit_footprint(out, ref, ftype, value, ax, ay, th, side, nets)
    # board outline
    emit_gr_line(out, 0, 0, BOARD_W, 0, "Edge.Cuts")
    emit_gr_line(out, BOARD_W, 0, BOARD_W, BOARD_H, "Edge.Cuts")
    emit_gr_line(out, BOARD_W, BOARD_H, 0, BOARD_H, "Edge.Cuts")
    emit_gr_line(out, 0, BOARD_H, 0, 0, "Edge.Cuts")
    # keepout indicator on silk
    x0, y0, x1, y1 = KEEPOUT
    emit_gr_line(out, x0, y0, x1, y0, "F.SilkS", 0.15)
    emit_gr_line(out, x1, y0, x1, y1, "F.SilkS", 0.15)
    emit_gr_line(out, x1, y1, x0, y1, "F.SilkS", 0.15)
    emit_gr_line(out, x0, y1, x0, y0, "F.SilkS", 0.15)
    for text, x, y, r in SILK_F:
        emit_gr_text(out, text, x, y, "F.SilkS", r)
    for text, x, y, r in SILK_B:
        emit_gr_text(out, text, x, y, "B.SilkS", r, mirror=True)
    # copper: fixed skeleton
    for net, layer, w, pts in FIXED:
        emit_segment(out, net, layer, w, pts)
    for net, x, y in FIXED_VIAS:
        emit_via(out, net, x, y)
    # copper: routed
    for net, layer, w, pts in routed_segments:
        emit_segment(out, net, layer, w, pts)
    for net, x, y in routed_vias:
        emit_via(out, net, x, y)
    # 4-layer planes: GND on In1 (continuous), +5V on In2.  SMD pads of these
    # nets need a via to the inner plane; PTH barrels pass through and connect.
    for net, x, y in plane_vias():
        emit_via(out, net, x, y)
    # zones
    inset = 0.5
    rect = [(inset, inset), (BOARD_W - inset, inset),
            (BOARD_W - inset, BOARD_H - inset), (inset, BOARD_H - inset)]
    emit_zone(out, "GND", "In1.Cu", rect)
    for net, poly in POWER_ZONES.items():
        emit_zone(out, net, "In2.Cu", poly)
    for x0, y0, x1, y1 in SERVO_POUR_RECTS:
        emit_zone(out, "+5V_SERVO", "B.Cu",
                  [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], priority=1)
    emit_keepout(out)
    out.append('\t(embedded_fonts no)')
    out.append(')')
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------------------
# --check: re-parse emitted file, pad-net diff + connectivity
# ---------------------------------------------------------------------------

def _tokenize(s):
    import re
    return re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+', s)


def _parse(toks):
    pos = 0

    def rec():
        nonlocal pos
        if toks[pos] == '(':
            pos += 1
            node = []
            while toks[pos] != ')':
                node.append(rec())
            pos += 1
            return node
        t = toks[pos]
        pos += 1
        return t.strip('"')

    tree = rec()
    return tree


def _child(node, tag):
    return [c for c in node[1:] if isinstance(c, list) and c and c[0] == tag]


def check():
    with open(PCB_PATH) as f:
        tree = _parse(_tokenize(f.read()))
    problems = []

    # --- pad net table diff
    expected = {}
    for p in PADS:
        if p["num"] in ("MP1", "MP2", ""):
            continue
        expected[(p["ref"], p["num"])] = p["net"]
    got = {}
    for fp in _child(tree, "footprint"):
        refp = [c for c in _child(fp, "property") if len(c) > 2 and c[1] == "Reference"]
        if not refp:
            continue
        ref = refp[0][2]
        for pad in _child(fp, "pad"):
            num = pad[1]
            if num in ("MP1", "MP2", ""):
                continue
            netc = _child(pad, "net")
            got[(ref, num)] = netc[0][1] if netc else None
    mism = 0
    for k in sorted(set(expected) | set(got)):
        if expected.get(k) != got.get(k):
            problems.append(f"pad net mismatch {k}: expected {expected.get(k)} "
                            f"got {got.get(k)}")
            mism += 1
    print(f"pad-net diff: {len(expected)} pads, {mism} mismatches")

    # --- connectivity (excl. GND, which relies on poured zones)
    segs = []   # (net, layer, x1,y1,x2,y2,w)
    vias = []   # (net, x, y)
    for seg in _child(tree, "segment"):
        st = _child(seg, "start")[0]
        en = _child(seg, "end")[0]
        w = float(_child(seg, "width")[0][1])
        layer = _child(seg, "layer")[0][1]
        net = _child(seg, "net")[0][1]
        segs.append((net, layer, float(st[1]), float(st[2]),
                     float(en[1]), float(en[2]), w))
    for via in _child(tree, "via"):
        at = _child(via, "at")[0]
        netc = _child(via, "net")
        vias.append((netc[0][1] if netc else None, float(at[1]), float(at[2])))

    pads = [p for p in PADS if p["net"]]

    def pad_rect(p):
        hx, hy = p["sx"] / 2, p["sy"] / 2
        if p["rot"] and p["sx"] != p["sy"]:
            hx, hy = hy, hx
        return hx, hy

    def pt_rect_dist(px, py, cx, cy, hx, hy):
        dx = max(abs(px - cx) - hx, 0.0)
        dy = max(abs(py - cy) - hy, 0.0)
        return math.hypot(dx, dy)

    def seg_pad_touch(seg, p):
        _n, _l, x1, y1, x2, y2, w = seg
        hx, hy = pad_rect(p)
        d = math.hypot(x2 - x1, y2 - y1)
        steps = max(1, int(d / (RES * 0.5)) + 1)
        for i in range(steps + 1):
            t = i / steps
            if pt_rect_dist(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t,
                            p["x"], p["y"], hx, hy) <= w / 2 + 0.03:
                return True
        return False

    def seg_pt_touch(seg, x, y, r):
        _n, _l, x1, y1, x2, y2, w = seg
        d = math.hypot(x2 - x1, y2 - y1)
        steps = max(1, int(d / (RES * 0.5)) + 1)
        for i in range(steps + 1):
            t = i / steps
            if math.hypot(x1 + (x2 - x1) * t - x, y1 + (y2 - y1) * t - y) \
                    <= w / 2 + r + 0.03:
                return True
        return False

    def seg_seg_touch(a, b):
        _na, la, ax1, ay1, ax2, ay2, wa = a
        _nb, lb, bx1, by1, bx2, by2, wb = b
        if la != lb:
            return False
        d = math.hypot(ax2 - ax1, ay2 - ay1)
        steps = max(1, int(d / (RES * 0.5)) + 1)
        for i in range(steps + 1):
            t = i / steps
            if seg_pt_touch(b, ax1 + (ax2 - ax1) * t, ay1 + (ay2 - ay1) * t,
                            wa / 2):
                return True
        return False

    nets_all = sorted({p["net"] for p in pads})
    bad_nets = []
    for net in nets_all:
        # inner-plane nets (In1 GND, In2 POWER_ZONES) — not track-routed
        if net in ("GND",) or net in POWER_ZONES:
            continue
        items = []  # ('p', pad) | ('s', seg) | ('v', via)
        for p in pads:
            if p["net"] == net:
                items.append(("p", p))
        for s in segs:
            if s[0] == net:
                items.append(("s", s))
        for v in vias:
            if v[0] == net:
                items.append(("v", v))
        if len(items) < 2:
            continue
        parent_ = list(range(len(items)))

        def find(a):
            while parent_[a] != a:
                parent_[a] = parent_[parent_[a]]
                a = parent_[a]
            return a

        def union(a, b):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent_[ra] = rb

        def layers_of(it):
            k, o = it
            if k == "v":
                return ("F.Cu", "B.Cu")
            if k == "s":
                return (o[1],)
            return ("F.Cu", "B.Cu") if o["layer"] == "P" else (o["layer"] + ".Cu",)

        def touch(ia, ib):
            ka, oa = ia
            kb, ob = ib
            shared = set(layers_of(ia)) & set(layers_of(ib))
            if not shared:
                return False
            if ka == "p" and kb == "p":
                hx1, hy1 = pad_rect(oa)
                hx2, hy2 = pad_rect(ob)
                return pt_rect_dist(oa["x"], oa["y"], ob["x"], ob["y"],
                                    hx2, hy2) <= hx1 + 0.03 or \
                    pt_rect_dist(ob["x"], ob["y"], oa["x"], oa["y"],
                                 hx1, hy1) <= hx2 + 0.03
            if ka == "s" and kb == "s":
                return seg_seg_touch(oa, ob)
            if ka == "s" and kb == "p":
                return seg_pad_touch(oa, ob)
            if ka == "p" and kb == "s":
                return seg_pad_touch(ob, oa)
            if ka == "v" and kb == "s":
                return seg_pt_touch(ob, oa[1], oa[2], VIA_DIA / 2)
            if ka == "s" and kb == "v":
                return seg_pt_touch(oa, ob[1], ob[2], VIA_DIA / 2)
            if ka == "v" and kb == "p":
                hx, hy = pad_rect(ob)
                return pt_rect_dist(oa[1], oa[2], ob["x"], ob["y"], hx, hy) \
                    <= VIA_DIA / 2 + 0.03
            if ka == "p" and kb == "v":
                hx, hy = pad_rect(oa)
                return pt_rect_dist(ob[1], ob[2], oa["x"], oa["y"], hx, hy) \
                    <= VIA_DIA / 2 + 0.03
            if ka == "v" and kb == "v":
                return math.hypot(oa[1] - ob[1], oa[2] - ob[2]) <= VIA_DIA + 0.03
            return False

        for i in range(len(items)):
            for j in range(i + 1, len(items)):
                if touch(items[i], items[j]):
                    union(i, j)
        ncomp = len({find(i) for i in range(len(items))})
        if ncomp > 1:
            comps = {}
            for i, it in enumerate(items):
                comps.setdefault(find(i), []).append(it)
            desc = []
            for comp in comps.values():
                ps = [f"{o['ref']}.{o['num']}" for k, o in comp if k == "p"]
                desc.append(f"[{len(comp)} items: {' '.join(ps[:6])}]")
            problems.append(f"net {net}: {ncomp} disjoint islands: "
                            + " ".join(desc))
            bad_nets.append(net)
        else:
            npads = sum(1 for k, _ in items if k == "p")
            print(f"net {net}: connected ({npads} pads, "
                  f"{sum(1 for k, _ in items if k == 's')} segs, "
                  f"{sum(1 for k, _ in items if k == 'v')} vias)")
    gnd_pads = sum(1 for p in pads if p["net"] == "GND")
    print(f"GND: {gnd_pads} pads — connectivity via F/B pours "
          f"(verify with kicad-cli DRC after --fill)")
    print(f"check: {len(problems)} problem(s)")
    for p in problems:
        print("  !!", p)
    return not problems


# ---------------------------------------------------------------------------
# --fill: pour zones via pcbnew (run with KiCad's bundled python)
# ---------------------------------------------------------------------------

def fill():
    import pcbnew
    b = pcbnew.LoadBoard(PCB_PATH)
    filler = pcbnew.ZONE_FILLER(b)
    zones = list(b.Zones())
    filler.Fill(zones, True)
    pcbnew.SaveBoard(PCB_PATH, b)
    print(f"filled {len(zones)} zones -> {PCB_PATH}")


# ---------------------------------------------------------------------------

def main():
    build_pads()
    board = Board()
    errors = fixed_skeleton_check(board)
    if errors:
        print(f"fixed skeleton: {len(errors)} clearance violation(s):")
        for e in errors:
            print("  !!", e)
    else:
        print("fixed skeleton: clearance self-check PASS")
    router = Router(board)
    router.route_all()
    if router.failed:
        print(f"router: {len(router.failed)} FAILED target(s): "
              + ", ".join(router.failed))
    else:
        print(f"router: all nets routed ({len(router.segments)} polylines, "
              f"{len(router.vias)} vias)")
    text = build_board_file(router.segments, router.vias)
    with open(PCB_PATH, "w") as f:
        f.write(text)
    print(f"wrote {PCB_PATH} ({len(text)} bytes)")
    ok = check()
    return 0 if (ok and not errors and not router.failed) else 1


if __name__ == "__main__":
    if "--fill" in sys.argv:
        fill()
    elif "--check" in sys.argv:
        build_pads()
        sys.exit(0 if check() else 1)
    else:
        sys.exit(main())
