# 目标再识别 (Re-ID) 跟随方案

## 目标与范围

把 `vision_tracker.py`（计划中）的「人脸**检测**」升级为「目标**再识别 (Re-ID)**」：让 Wall-E 锁定**一个**人后，在目标转头、被遮挡、短暂消失后仍能继续跟**同一个**人。属于 `docs/VOICE_LLM_PLAN.md` 里 **Phase 2 视觉接入**的设计细化，本期不写代码，仅定方案。

**本期范围内**
- 人脸 embedding + 锁定模板 + 余弦匹配（主锁）
- 身体 Re-ID：BlazePose 骨架 + 躯干 HSV 颜色直方图 + 肢长比 + 姿态构型（丢脸回退）
- 步态加成：步频/步幅/手臂摆幅/头部起伏 时序签名（置信度加成）
- 轨迹跟踪摊销 embedding 算力
- `RobotBrain` 融合状态机（SEARCH/IDLE 超时、重识别窗口）

**本期范围外（单独任务）**
- 上学习型步态模型（GaitSet 等），本期用手工特征 + 模板最近邻
- Coral Edge TPU 适配（需 `edgetpu_compiler` 重编，见 `docs/HARDWARE.md` 既有提醒）
- `VL53L1X` 测距接入（`AVOID` 碰撞保护依赖它，未装时 `FOLLOW` 无防撞）

**设计预算**：Raspberry Pi 4B，单线程含 Python 开销，目标 ~12–15 fps。

## 架构与数据流

```
[Pi Camera CSI] ──帧──▶ FaceDetector ──▶ N 个人脸框
                        │                 │
                        ▼ (按轨迹)        ▼ (需验身时)
                   BboxTracker ────────▶ FaceLandmarker(5点)
                        │                 │ 仿射对齐
                        │                 ▼
                        │            MobileFaceNet ──embedding──┐
                        │                                          │ 余弦相似
                        │              ┌──── reference_embedding ◀┘ (锁定时存)
                        │              ▼
                        ▼         命中? ──是──▶ 用人脸框 (置信最高)
                PoseLandmarker                    │
                   33 关键点                       │ 否(无人脸/不命中)
                   │                               ▼
        ┌───────────┼───────────┐          身体 Re-ID:
        ▼           ▼           ▼          躯干HSV直方图 + 肢长比 + 姿态构型
     躯干ROI     肢长比       步态窗口        │   + 步态加成(窗口满时)
        │         (肩宽归一)  (滑动N帧)       │       │
        ▼                                       ▼       ▼
     HSV直方图 ──────────────────────────▶ 加权距离 ──▶ 命中? ──是──▶ 用身体框
                                                          │
                                                          ▼ 否
                                                   丢 > T1 -> SEARCH
                                                   丢 > T2 -> IDLE(清 ref)

锁定目标的 (offset_x, area_ratio) ──▶ FollowController ──▶ Arduino X/Y
```

Re-ID 层包装在 `VisionTracker` 上（或新增 `ReIDTracker`），**对下游接口不变**：仍吐出锁定目标的 `(offset_x, area_ratio, confidence, source)`，`FollowController` 无需改动。

## 新增 / 修改文件

| 文件 | 动作 | 说明 |
|------|------|------|
| `web_interface/reid_tracker.py` | 新增 | `ReIDTracker`：bbox 轨迹 + 人脸匹配 + 身体匹配 + 融合决策，包装 `VisionTracker` |
| `web_interface/gait_features.py` | 新增 | 步态特征提取（步频/步幅/摆幅/起伏）+ 模板最近邻 |
| `web_interface/vision_tracker.py` | 修改（计划中模块） | 暴露检测/姿态原始结果供 Re-ID 复用；`track()` 增 `track_id/confidence/source` |
| `web_interface/config.py` | 修改 | 新增 `VISION_REID_*` / `VISION_GAIT_*` 段（见下） |
| `web_interface/robot_brain.py` | 修改（计划中模块） | `SEARCH` 加超时重识别窗口、`T1/T2` 转 `IDLE` 清 reference |
| `models/mobilefacenet.tflite` | 新增 | 人脸 embedding 模型（需另行获取，见风险） |

## 配置新增（config.py）

```python
# 人脸 Re-ID
VISION_REID_ENABLED = True
VISION_FACE_RECOG_MODEL = "models/mobilefacenet.tflite"   # 人脸 embedding，MediaPipe 不自带
VISION_FACE_EMBED_DIM = 128
VISION_FACE_MATCH_THRESHOLD = 0.55        # ArcFace/MobileFaceNet 经验值，需现场标定
VISION_FACE_ALIGN = True                  # 5 点仿射对齐（侧脸/低光可关）
VISION_REID_INTERVAL = 8                  # 每 K 帧重算一次 embedding 验身
VISION_LOCK_STRATEGY = "largest"          # 锁定时取最大脸 / "center" 最居中

# 身体 Re-ID（路线 C：姿态+外观为主，步态加成）
VISION_BODY_REID_METHOD = "pose_color_gait"
VISION_BODY_MATCH_THRESHOLD = 0.45
VISION_BODY_WEIGHTS = {"color": 0.5, "limb": 0.3, "pose": 0.2}  # 加权和
VISION_TORSO_HIST_BINS = [8, 8]           # HSV H×S 2D 直方图
VISION_LIMB_BASIS = "shoulder_width"      # 肢长归一化基准（抗透视）

# 步态加成
VISION_GAIT_WINDOW_FRAMES = 30           # ~2s 窗口
VISION_GAIT_FREQ_RANGE = [0.8, 2.5]       # 有效步频 Hz，窗口外不参与
VISION_GAIT_BOOST_WEIGHT = 0.15           # 仅在窗口满且人行走时叠加

# 重识别状态机
VISION_TRACK_MAX_LOST_SEC = 1.5           # T1：丢脸 -> SEARCH
VISION_GIVEUP_SEC = 12                    # T2：SEARCH 仍无 -> IDLE 清 reference
```

## 组件设计

### 1. 人脸 embedding + 锁定（主锁）

- **模型**：MobileFaceNet TFLite（~4–5MB，112×112 RGB -> 128-d），L2 归一化。MediaPipe Tasks **不提供**人脸识别模型，须另配。
- **对齐**：用 `FaceLandmarker` 的 5 点（双眼角、鼻尖、双嘴角）仿射到标准 112×112，显著拉开正负样本余弦差。侧脸/低光 landmark 不准时降级为不对齐、阈值放宽。
- **锁定 (lock)**：语音 `follow_me`（`voice_llm.py` 工具，已计划设 `behavior=FOLLOW`）或 UI 按钮触发 -> 取当前帧最大/居中脸 -> 算 embedding 存 `reference_embedding`，同时采身体 reference。
- **匹配**：每张脸 embedding 与 reference 余弦相似度 > `VISION_FACE_MATCH_THRESHOLD` 即视为同一人；多张脸时选相似度最高且过阈者。

### 2. 身体 Re-ID（丢脸回退，路线 C 主体）

- **姿态**：`PoseLandmarker`（BlazePose lite，33 关键点 + visibility），任意角度可用（含背对）。
- **外观特征（最便宜最稳）**：取躯干 ROI（肩–髋四点凸包内缩，`pose` 关键点 mask 排背景）的 **HSV 颜色直方图**（H×S 2D，`[8,8]` bin），L2 归一化，巴氏系数/相交比对。
- **肢长比（固定身体特征）**：上臂/前臂/大腿/小腿/躯干长，全部按**肩宽**归一化（抗透视）。换装不变，是「固定身体特征」的几何体现。背对时部分点不可见 -> 只用可见子集、权重重归一化。
- **姿态构型**：关键点减质心后 flatten，权重小，作补充。
- **匹配**：`Σ wᵢ·(1−simᵢ)`，阈值 `VISION_BODY_MATCH_THRESHOLD`。

### 3. 步态加成（身体运动特征的核心）

- 维护 `PoseLandmarker` 输出的滑动窗口（`VISION_GAIT_WINDOW_FRAMES`，~2s）。
- 特征：左右踝 x 过零频率（步频）、连续同侧步 x 跨度/肩宽（步幅）、腕相对躯干振幅（摆臂）、鼻尖 y 振幅（头部起伏）。
- **仅当窗口填满且步频落在 `[0.8, 2.5] Hz`** 才输出步态距离、按 `VISION_GAIT_BOOST_WEIGHT` 叠加；否则置 0 不参与（不误判静止/非行走帧）。
- 这是真正依赖**时序运动**的身份签名，对换装/相似衣着鲁棒。

### 4. 轨迹跟踪摊销（省算力的关键）

- 轻量 IoU + 中心距关联维护 `tracks: {id, bbox, last_embedding, last_seen, confirmed}`。
- 新 track：跑一次 embedding 与 reference 比对 -> 标 `confirmed`/`rejected`。
- confirmed track：bbox 逐帧跟随，每 `VISION_REID_INTERVAL` 帧重 embed 验身。
- embedding 是最贵一步，按轨迹摊销到每 K 帧 -> 均摊 ~3ms。
- 消失超时 -> 删 track；confirmed 丢失触发 `SEARCH`。

### 5. 融合与状态机（接入 RobotBrain）

```
锁定事件 -> 采 reference(人脸 embedding + 身体外观/肢长/步态) -> FOLLOW

FOLLOW 帧:
  有人脸且 embedding 命中        -> 用人脸框 (最高置信), 刷 last_bbox
  无人脸但身体特征命中           -> 用身体框
  两者都无, 但在 last_bbox 邻域且 < T1 -> 预测外推继续跟
  丢失 > T1 (1.5s)              -> SEARCH (转头/小幅搜索轨迹重获)
  SEARCH 中再次命中 reference    -> 回 FOLLOW
  丢失 > T2 (12s)               -> IDLE (放弃), 清 reference
```

`AVOID`（碰撞）由 `FOLLOW_SAFETY_DISTANCE` + VL53L1X 触发，与 Re-ID 正交。

### 6. 算力预算（Pi 4B，单线程）

| 步骤 | 耗时 | 频率 |
|------|------|------|
| FaceDetector | ~10ms | 每帧 |
| PoseLandmarker (lite) | ~15–20ms | 每帧 |
| HSV 直方图 | <1ms | 每帧 |
| MobileFaceNet embedding | ~20ms | 每 K 帧摊销 ~3ms |
| 步态特征 | <2ms | 每帧（增量） |
| **合计** | ~30–35ms/帧 | → ~12–15 fps（含 Python 开销） |

Pi5 有余量可上更重 embedding/步态模型；Coral 须重编（见风险）。

## 依赖

```
# 均在 mediapipe 生态，无新增重依赖
# mobilefacenet.tflite 模型文件需另行获取（见风险）
```

`mediapipe` / `opencv-python` / `numpy` 已在既有计划依赖中。`PoseLandmarker`/`FaceLandmarker` 走 MediaPipe Tasks，与现有 `vision_tracker.py` 一致。

## 并发与边界

- **锁定竞态**：重复 `follow_me` -> 以最后一次为准，重置 reference（含身体+步态）。
- **换人穿越**：非目标脸 embedding 不过阈被拒；bbox 跟踪避免跳到穿场者；两人脸重叠时按 embedding 选 confirmed track。
- **短遮挡**：`< T1` 靠 last_bbox 预测外推；长遮挡进 `SEARCH`。
- **直方图漂移**：HSV 比 RGB 抗光照；可用 EMA 慢更新 reference 适应衣着/光照缓变，但需防被背景污染（ROI 用 pose mask）。
- **背对可见性**：肢长比算不全 -> 只用可见子集、权重重归一化；颜色直方图仍可用。
- **静止帧**：步态置 0 不参与，靠姿态+外观判定。
- **断连**：`arduino.is_connected()` 为假时 FollowController 不下发，Re-ID 仍可运行但无动作。

## 测试（仓库无测试套件，手动冒烟）

1. `python3 web_interface/reid_tracker.py --self-check`：喂一段视频/摄像头 -> 打印每帧决策（face/body/gait 命中、confidence、source）。
2. 单人锁定后转身背对：应继续跟随（body 命中）；转回正面应回 face 命中。
3. 锁定后第二人入场横穿：不换目标。
4. 目标走出画面 > T1：进 `SEARCH`；3s 内回画面：恢复 `FOLLOW`；> T2：回 `IDLE` 清 ref。
5. 相似衣着两人：步态/肢长辅助区分。
6. 调 `VISION_FACE_MATCH_THRESHOLD` / `VISION_BODY_MATCH_THRESHOLD`：记录 ROC 取工作点。

## 风险

- **MobileFaceNet 选型/许可**：需找可商用 TFLite；不同 export 的余弦分布不同，阈值须现场标定，不能照搬。
- **对齐失败**：侧脸/低光 landmark 不准 -> 降级不对齐、阈值放宽。
- **双模型时延**：embedding+pose 在 Pi4 可能掉帧 -> 限帧/隔帧跑 pose、embedding 仅按轨迹触发。
- **颜色歧义**：制服/深色衣着 -> 步态/肢长补充；极端情况置信度低 -> 倾向 `SEARCH` 而非乱跟。
- **Coral**：现有 `face_detector.tflite` 非 Edge-TPU 编译版（`docs/HARDWARE.md` 已提醒），embedding/pose 模型同理须 `edgetpu_compiler` 重编，否则白买加速棒。
- **步态窗口延迟**：首次锁定后需 ~2s 才有步态签名可用，前期靠 face+body。

## 实施顺序

1. `config.py` 加 `VISION_REID_*` / `VISION_GAIT_*` 段 -> 2. bbox 轨迹跟踪器 -> 3. 人脸 embedding + 对齐 + lock + 余弦匹配 -> 4. 身体 Re-ID（pose+color+limb） -> 5. `gait_features.py` 步态加成 -> 6. `ReIDTracker` 包装、统一输出接口 -> 7. `RobotBrain` 融合状态机（T1/T2/SEARCH/IDLE） -> 8. 接 `voice_llm` `follow_me` 触发 lock -> 9. 前端最小 UI（锁定/状态/置信度） -> 10. 手动冒烟 + 阈值标定。
