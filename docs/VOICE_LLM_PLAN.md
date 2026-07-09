# 接入语音大模型 实施方案

## 目标与范围

把 `voice_agent.py` 现有的「关键词匹配」升级为「唤醒词 → ASR → 火山方舟豆包 LLM（函数调用）→ TTS」的完整语音对话管线，让 Wall-E 能用中文自然对话并按语义直接控制动作。

**本期范围内（Phase 1）**
- 唤醒词常驻监听（openWakeWord + sounddevice 统一音频路径）
- 火山方舟 Ark API（OpenAI 兼容）+ 函数调用，控制动画/头/眼/臂/直行/停止
- 多轮对话上下文（有界历史 + 空闲超时清空）
- 接入 `app.py`：后台线程生命周期、状态/开关路由、最小 Web UI
- 说话时抑制唤醒（避免自激）、状态机并发控制

**本期范围外（Phase 2，单独任务）**
- 把 `vision_tracker` + `follow_controller` + `robot_brain` 接入 `app.py`，让 `follow` 真正驱动行走。本期 `follow` 工具只写共享行为状态 `FOLLOW`，供 Phase 2 消费。
- 自定义中文唤醒词「嗨 Wall-E」模型训练（本期用内置英文模型打底）。
- 流式 ASR/TTS 降延迟、多模态（摄像头喂 LLM 视觉）。

## 架构与数据流

```
[sounddevice 16kHz 单声道流] ──chunk──▶ 唤醒检测(openWakeWord)
                                          │ 命中
                                          ▼
                              状态机: IDLE→LISTENING
                                          │ 录音 N 秒 → PCM
                                          ▼
                              ASR(google/whisper) → 文本
                                          │
                                          ▼
                  火山方舟 Ark chat.completions(tools=…, history=…)
                                          │
                            ┌─────────────┴──────────────┐
                            ▼                            ▼
                     tool_calls(函数调用)          纯文本回复
                            │                            │
                            ▼                            ▼
                  ToolDispatcher → arduino            TTS(edge-tts)
                  .send_command(...)                  │ + 抑制唤醒
                  + 写共享 behavior 状态              ▼
                            │                    回到 IDLE 监听
                            └─── 把 tool 结果回灌 LLM，再循环 ──▶
```

## 新增 / 修改文件

| 文件 | 动作 | 说明 |
|------|------|------|
| `web_interface/voice_llm.py` | 新增 | `VoiceLLMAgent`：唤醒线程 + 对话状态机 + LLM 客户端 + 工具循环 + 生命周期 |
| `web_interface/robot_tools.py` | 新增 | 工具 schema（OpenAI function 格式）+ `ToolDispatcher`：工具名/参数 → `arduino.send_command` + 共享 behavior |
| `web_interface/voice_agent.py` | 修改 | 抽出 `transcribe_pcm(pcm_bytes, sample_rate)` 接受原始 PCM 做 ASR（不再独占麦克风）；保留 `listen()`/`speak()` 供 push-to-talk 回退路径 |
| `web_interface/config.py` | 修改 | 新增 `LLM_*` / `WAKE_WORD_*` 段（全大写键名 + 注释，沿用现有风格） |
| `web_interface/app.py` | 修改 | 实例化 `VoiceLLMAgent`、启动/关闭后台线程、新增 `/voiceStatus` `/voiceToggle` 路由 |
| `web_interface/templates/index.html` + `static/js/main.js` | 修改（最小） | 设置页加「语音对话」开关 + 状态点 |
| `raspi-setup.sh` | 修改 | `pip install openai openwakeword sounddevice numpy`（与现有 mediapipe 等一起注明） |

## 配置新增（config.py）

```python
# LLM 配置（火山方舟 Ark，OpenAI 兼容）
LLM_ENABLED = True
LLM_PROVIDER = "volcengine"
LLM_ARK_API_KEY = ""                         # 火山方舟 API Key（写 local_config.py）
LLM_ARK_BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
LLM_ARK_MODEL = ""                           # Ark 推理接入点 ID，如 ep-2024xxxx
LLM_SYSTEM_PROMPT = (                        # Wall-E 人设
    "你是 Wall-E，一台可爱、好奇、黏人的垃圾清理机器人。"
    "用简短、口语化的中文回答（最多两三句），会偶尔提到 Eva。"
    "可以通过调用工具控制自己的动作。"
)
LLM_MAX_HISTORY = 8                          # 保留最近几轮对话
LLM_TEMPERATURE = 0.7
LLM_IDLE_RESET_SECONDS = 60                  # 空闲多久后清空历史

# 唤醒词配置
WAKE_WORD_ENABLED = True
WAKE_WORD_MODEL = "hey_jarvis"               # 内置模型打底；自定义换 "models/hey_walle.onnx"
WAKE_WORD_SENSITIVITY = 0.5
WAKE_WORD_CHUNK_MS = 80                      # openWakeWord 标准 80ms 帧
```

## 组件设计

### 1. 统一音频路径（关键，解决麦克风争用）
单一 `sounddevice.InputStream`（16kHz、mono、int16、chunk 1280 样本=80ms）：
- **唤醒线程**：每帧喂 `owwModel.predict(chunk)`，任一模型得分 ≥ `WAKE_WORD_SENSITIVITY` → 触发 `wake_event`。
- **ASR 阶段**：命中后切到 `LISTENING`，暂停喂唤醒，攒 `VOICE_RECORD_SECONDS` 秒 PCM → `sr.AudioData(pcm_bytes, 16000, 2)` → 调 `VoiceAgent.transcribe_pcm()` → google/whisper。
- 用 sounddevice 同时供唤醒+ASR，**避免 `speech_recognition.Microphone` 独占设备冲突**。

### 2. 对话状态机（`voice_llm.py`）
状态枚举 `IDLE / LISTENING / THINKING / SPEAKING`，`threading.Lock` 保护：
- `IDLE`：唤醒监听中。
- `LISTENING`：唤醒命中 → 录音 → ASR。
- `THINKING`：调 LLM；若返回 `tool_calls` → `ToolDispatcher.execute()` → 把结果以 `role=tool` 回灌 → 再调 LLM（循环直至纯文本）。
- `SPEAKING`：`VoiceAgent.speak(text)`；期间 `is_speaking=True` 抑制唤醒。
- 空闲超过 `LLM_IDLE_RESET_SECONDS` 清空历史。
- 非法并发（说话时唤醒、思考时再唤醒）由状态锁丢弃。

### 3. LLM 客户端（火山方舟 Ark）
用 `openai` SDK（Ark 兼容）：
```python
from openai import OpenAI
client = OpenAI(api_key=LLM_ARK_API_KEY, base_url=LLM_ARK_BASE_URL)
resp = client.chat.completions.create(
    model=LLM_ARK_MODEL,
    messages=[{"role":"system","content":LLM_SYSTEM_PROMPT}, *history],
    tools=TOOL_SCHEMAS, tool_choice="auto", temperature=LLM_TEMPERATURE,
)
```
- 处理 `resp.choices[0].message.tool_calls` → 逐个执行 → 追加 `tool` 消息 → 重调。
- 终态文本 → `speak()`。
- 失败回退：TTS 一句「我有点没听清，再说一次」。
- **关于「agent plan」**：火山方舟的智能体（bot）走 `/api/v3/bots/chat/completions` + `bot_id`，但其插件是服务端 HTTP，不适合实时本地控机器人。故本期用**模型接入点 + 内联 tools**（Doubao 支持 OpenAI 风格 function calling），人设走 system prompt。bot 模式作为 config 注释留口，不在本期实现。

### 4. 工具集与 Dispatcher（`robot_tools.py`）
工具 schema 用 OpenAI function 格式；`ToolDispatcher` 持有 `arduino` 与共享 `behavior` 状态：

| 工具 | 参数 | 动作 |
|------|------|------|
| `stop_move` | — | `X0`+`Y0`；behavior=IDLE |
| `follow_me` | — | behavior=FOLLOW（Phase 2 消费） |
| `play_animation` | `clip: 0\|1\|2` | `A{clip}` |
| `move_head` | `rotation: 0-100` | `G{n}` |
| `tilt_neck` | `position: 0-100` | `T{n}`（上）/`B{n}`（下），按值选 |
| `move_eyes` | `left:0-100, right:0-100` | `E{}`/`U{}` |
| `wave_arm` | `side: left\|right, position:0-100` | `L{}`/`R{}` |
| `drive` | `x:-100..100, y:-100..100, duration_ms:int` | `X{}`+`Y{}`，睡 duration 后自动 `X0Y0`（安全自停） |
| `set_behavior` | `state: idle\|follow\|chat` | 写共享状态 |

- `arduino.is_connected()` 为假时：工具跳过下发，LLM 被告知「机器人离线」。
- 每个 `send_command` 复用现有 Queue，天然线程安全。
- `drive` 用独立计时线程自停，避免 LLM 失控长驱。

### 5. app.py 接入与生命周期
- 第 241 行 `arduino = ArduinoDevice()` 之后：实例化 `voice_llm = VoiceLLMAgent(app.config, arduino)`。
- `if __name__ == '__main__'` 内、`serve()`/`app.run()` 前：`voice_llm.start()`；`atexit`/`finally` 里 `voice_llm.stop()`。
- 新路由（复用 `session.get('active')` 登录守卫）：
  - `/voiceStatus`：返回 `{wake_enabled, state, arduino_connected}`。
  - `/voiceToggle`：POST 切换唤醒开关。

### 6. 最小 Web UI
设置页加：开关 checkbox + 状态点（IDLE/LISTENING/THINKING/SPEAKING 四色）。JS 每 1.5s 轮询 `/voiceStatus`。唤醒是主交互，UI 仅作开关与可视化。

## 依赖

```
pip install openai openwakeword sounddevice numpy
```
（`mediapipe`/`opencv-python`/`edge-tts`/`pygame`/`speech_recognition` 按现有 CLAUDE.md 已需另装。）
API Key、接入点 ID 写 `local_config.py`（已 gitignore），不进 `config.py`。

## 并发与边界

- **自激**：`SPEAKING` 时唤醒线程丢帧不判断。
- **并发唤醒**：非 `IDLE` 状态的唤醒事件直接丢弃。
- **LLM 失败/超时**：回退 TTS + 回 `IDLE`，不清历史。
- **Arduino 断连**：工具 no-op，LLM 回复中说明离线。
- **drive 超时**：独立看门狗线程强制 `X0Y0`。
- **历史膨胀**：`LLM_MAX_HISTORY` 截断 + 空闲超时清空。

## 测试（仓库无测试套件，手动冒烟）

1. `python3 web_interface/voice_llm.py` 自检模式：跳过唤醒，直接文本输入 → LLM → 工具执行 → 打印指令。
2. 接 Arduino：喊唤醒词 → 说「挥挥手」→ 观察 `wave_arm` 下发 `L80`/`R80` + TTS 回复。
3. 说「停下来」→ `stop_move`。
4. 说「看左边」→ `move_head`。
5. 断开 Arduino 重复 → 工具 no-op、LLM 回复离线提示。
6. `/voiceStatus` 轮询状态切换正确。

## 风险

- **火山方舟 function calling 兼容**：需选支持 tools 的 Doubao 模型/接入点（如 doubao-1.5-pro-32k 或 functioncall 专用版），接入点 ID 配错会报错——文档里标注。
- **中文唤醒词**：内置英文模型打底可用但名不副实；自定义「嗨 Wall-E」需训练，Phase 2。
- **端到端延迟**：唤醒+ASR+LLM+TTS 约 3–7s。流式优化放 Phase 2。
- **sounddevice 与 Pi 音频子系统**：需 `apt install libportaudio2`；raspi-setup.sh 一并加。

## 实施顺序

1. `config.py` 加配置段 → 2. `robot_tools.py` 工具+dispatcher → 3. `voice_agent.py` 抽 `transcribe_pcm` → 4. `voice_llm.py` 主体 → 5. `app.py` 接入+路由+生命周期 → 6. 前端最小 UI → 7. `raspi-setup.sh` 加依赖 → 8. 手动冒烟测试。
