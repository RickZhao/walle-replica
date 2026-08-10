/**
 * WALL-E MCP TOOLS
 *
 * @file    walle_mcp_tools.cc
 * @brief   Registers the self.walle.* / self.battery.* tools with the
 *          McpServer, exposing Wall-E's movements to the cloud LLM
 *          (function calling). Pattern follows otto-robot's
 *          OttoController::RegisterMcpTools().
 *
 * All motion tools dispatch through WalleMotion::EvaluateCommand(), so voice
 * commands behave exactly like the serial / web control paths. Camera and
 * eye-display actions dispatch through WalleCamLink (CAM_PROTOCOL v1 over
 * UART1, with HTTP fallback to the cam module, see docs/CAM_PROTOCOL.md).
 */

#include "walle_motion.h"
#include "walle_cam_link.h"
#include "walle_face_tracker.h"
#include "config.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <string>

#define TAG "WalleMcpTools"


// Neck position (0..200) -> neck top/bottom servo segment mapping
// (same mapping as web_interface/gamepad.py and bt_gamepad.cpp)
static void SendNeck(int n) {
    auto& motion = WalleMotion::GetInstance();
    if (n < 100) {
        motion.EvaluateCommand('T', n);
        motion.EvaluateCommand('B', 0);
    } else if (n < 160) {
        motion.EvaluateCommand('T', 200 - n);
        motion.EvaluateCommand('B', n - 100);
    } else {
        motion.EvaluateCommand('T', n - 110);
        motion.EvaluateCommand('B', 60);
    }
}


// -------------------------------------------------------------------
/// Camera module access goes through WalleCamLink (UART first, HTTP
/// fallback) - no local HTTP helpers needed here anymore.
// -------------------------------------------------------------------


void WalleMotion::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    ESP_LOGI(TAG, "Registering Wall-E MCP tools...");

    // -- Movement ----------------------------------------------------
    mcp.AddTool("self.walle.move",
        "让 Wall-E 机器人移动。direction: 方向，forward=前进、backward=后退、left=原地左转、right=原地右转；"
        "speed: 速度百分比 0-100；duration_ms: 持续毫秒数，到时间自动停止，0 表示持续移动直到调用 stop",
        PropertyList({
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 60, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 1000, 0, 10000)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string dir = properties["direction"].value<std::string>();
            int speed = properties["speed"].value<int>();
            int duration = properties["duration_ms"].value<int>();

            int move = 0, turn = 0;
            if (dir == "forward") move = speed;
            else if (dir == "backward") move = -speed;
            else if (dir == "left") turn = -speed;
            else if (dir == "right") turn = speed;
            else return std::string("unknown direction, use forward/backward/left/right");

            StartTimedMove(move, turn, duration);
            return true;
        });

    mcp.AddTool("self.walle.stop",
        "立即停止 Wall-E 的移动",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            StartTimedMove(0, 0, 0);
            return true;
        });

    // -- Head --------------------------------------------------------
    mcp.AddTool("self.walle.head",
        "控制 Wall-E 的头部姿态。rotation: 头部左右旋转 0-100（0=最左，50=正中，100=最右）；"
        "neck: 颈部位置 0-200（0=低头收回，125=自然居中，200=抬头前伸），-1=保持当前位置",
        PropertyList({
            Property("rotation", kPropertyTypeInteger, 50, 0, 100),
            Property("neck", kPropertyTypeInteger, -1, -1, 200)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            EvaluateCommand('G', properties["rotation"].value<int>());
            int neck = properties["neck"].value<int>();
            if (neck >= 0) SendNeck(neck);
            return true;
        });

    // -- Arms --------------------------------------------------------
    mcp.AddTool("self.walle.arms",
        "控制 Wall-E 的手臂。left / right: 手臂位置 0-100（0=放下，100=举起），-1=保持不动",
        PropertyList({
            Property("left", kPropertyTypeInteger, -1, -1, 100),
            Property("right", kPropertyTypeInteger, -1, -1, 100)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int left = properties["left"].value<int>();
            int right = properties["right"].value<int>();
            if (left >= 0) EvaluateCommand('L', left);
            if (right >= 0) EvaluateCommand('R', right);
            return true;
        });

    // -- Eye expressions ---------------------------------------------
    mcp.AddTool("self.walle.eyes",
        "控制 Wall-E 的眼部表情（机械眼舵机和摄像头模块上的眼睛屏会同步更新）。"
        "expression: neutral=中性、sad=难过/委屈、left=向左倾（疑惑）、right=向右倾（疑惑）",
        PropertyList({
            Property("expression", kPropertyTypeString, "neutral")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string expr = properties["expression"].value<std::string>();
            if (expr == "neutral") EvaluateCommand('k', 0);
            else if (expr == "sad") EvaluateCommand('i', 0);
            else if (expr == "left") EvaluateCommand('j', 0);
            else if (expr == "right") EvaluateCommand('l', 0);
            else return std::string("unknown expression, use neutral/sad/left/right");
            // Mirror the expression to the cam module's eye screens
            // (CAM_PROTOCOL §11; best effort, the mechanical eyes lead).
            WalleCamLink::GetInstance().SetEyes(expr);
            return true;
        });

    // -- Animations --------------------------------------------------
    mcp.AddTool("self.walle.play_animation",
        "播放 Wall-E 的预置动作动画。id: 0=复位回位、1=开机眨眼序列、2=好奇观察序列",
        PropertyList({
            Property("id", kPropertyTypeInteger, 2, 0, 2)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            EvaluateCommand('A', properties["id"].value<int>());
            return true;
        });

    // -- Autonomous mode ---------------------------------------------
    mcp.AddTool("self.walle.set_auto_mode",
        "开关 Wall-E 的自主动作模式（随机眨眼、转头等拟人小动作）。on: true=开启，false=关闭",
        PropertyList({
            Property("on", kPropertyTypeBoolean, false)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            EvaluateCommand('M', properties["on"].value<bool>() ? 1 : 0);
            return true;
        });

    // -- Illumination LED ---------------------------------------------
    mcp.AddTool("self.walle.light",
        "控制 Wall-E 的照明灯。brightness: 亮度百分比 0-100，0=关灯，100=最亮",
        PropertyList({
            Property("brightness", kPropertyTypeInteger, 100, 0, 100)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            EvaluateCommand('V', properties["brightness"].value<int>());
            return true;
        });

    // -- Camera module (photo / video recording on its SD card) -------
    mcp.AddTool("self.walle.camera",
        "控制 Wall-E 的摄像头模块（照片和录像保存在摄像头模块的 SD 卡上，预览和回放显示在 Wall-E 的眼睛屏幕上）。"
        "action: photo=拍一张照片（拍摄前摄像头会在眼睛屏做 3、2、1 倒计时，调用后你应立即同步对用户口播"
        "\"3、2、1，茄子\"；拍完照片自动在眼睛屏回放几秒）、record_start=开始录像、record_stop=停止录像、"
        "preview=在眼睛屏回放最新照片、replay=回放最新录像（眼睛屏暂不支持录像回放，工具会返回替代指引）、"
        "stop=停止眼睛屏上的回放并恢复表情",
        PropertyList({
            Property("action", kPropertyTypeString, "photo")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string action = properties["action"].value<std::string>();
            auto& cam = WalleCamLink::GetInstance();
            if (action == "photo") {
                std::string path;
                if (!cam.TakePhoto(path)) {
                    std::string err = cam.last_error();
                    return std::string("拍照失败") + (err.empty() ? "：摄像头模块无响应" : ("：" + err));
                }
                // CAM_PROTOCOL §4: prompt the LLM to voice the countdown
                return std::string("已拍照，照片保存在摄像头模块 ") + path +
                    "（眼睛屏正在回放）。请对用户口播：3、2、1，茄子！";
            }
            if (action == "record_start") {
                std::string path;
                if (!cam.RecordStart(path)) {
                    std::string err = cam.last_error();
                    return std::string("开始录像失败") + (err.empty() ? "：摄像头模块无响应" : ("：" + err));
                }
                return std::string("开始录像，文件 ") + path;
            }
            if (action == "record_stop") {
                std::string path;
                int frames = 0;
                if (!cam.RecordStop(path, frames)) {
                    std::string err = cam.last_error();
                    return std::string("停止录像失败") + (err.empty() ? "：摄像头模块无响应" : ("：" + err));
                }
                return std::string("录像已保存 ") + path + "（" + std::to_string(frames) + " 帧）";
            }
            if (action == "preview") {
                std::string path;
                if (!cam.ShowLatest(path)) {
                    return std::string("眼睛屏预览不可用（需要摄像头 UART 链路在线）");
                }
                return std::string("正在眼睛屏回放最新照片 ") + path;
            }
            if (action == "replay") {
                // CAM_PROTOCOL §11: eye-display video replay is v2; guide
                // the user to the cam module's browser page instead.
                return std::string("眼睛屏暂不支持录像回放。请在浏览器打开摄像头模块页面 ") +
                    CAM_MODULE_URL + " 下载录像文件播放。";
            }
            if (action == "stop") {
                if (!cam.Abort()) {
                    return std::string("停止回放失败（需要摄像头 UART 链路在线）");
                }
                return true;
            }
            return std::string("unknown action, use photo/record_start/record_stop/preview/replay/stop");
        });

    // -- Face Follow -------------------------------------------------
    mcp.AddTool("self.walle.follow_me",
        "启动人脸跟随：锁定摄像头画面中最大的人脸，Wall-E 会转头和移动身体来跟踪这个人。"
        "如果目标转身背对摄像头，会切换到人体检测模式继续跟随（通过衣服颜色匹配确认是同一个人）。"
        "说'跟着我'或'follow me'来触发。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            auto& cam = WalleCamLink::GetInstance();
            if (!cam.LockPerson()) {
                return std::string("人脸登记失败：摄像头 UART 链路离线，或当前画面中没有检测到人脸");
            }
            auto& tracker = WalleFaceTracker::GetInstance();
            tracker.StartFollow();
            return true;
        });

    mcp.AddTool("self.walle.stop_follow",
        "停止人脸跟随，机器人原地停止，解除人脸锁定。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            auto& tracker = WalleFaceTracker::GetInstance();
            tracker.StopFollow();
            auto& cam = WalleCamLink::GetInstance();
            cam.UnlockPerson();
            return true;
        });

    // -- Battery -----------------------------------------------------
    mcp.AddTool("self.battery.get_level",
        "查询 Wall-E 的电池电量百分比",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            int level = battery_level();
            if (level == -999) return std::string("battery level unavailable");
            return level;
        });

    ESP_LOGI(TAG, "Wall-E MCP tools registered");
}
