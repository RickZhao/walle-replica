/**
 * WALL-E MCP TOOLS
 *
 * @file    walle_mcp_tools.cc
 * @brief   Registers the self.walle.* / self.battery.* tools with the
 *          McpServer, exposing Wall-E's movements to the cloud LLM
 *          (function calling). Pattern follows otto-robot's
 *          OttoController::RegisterMcpTools().
 *
 * All tools dispatch through WalleMotion::EvaluateCommand(), so voice
 * commands behave exactly like the serial / web control paths.
 */

#include "walle_motion.h"
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
        "控制 Wall-E 的眼部表情。expression: neutral=中性、sad=难过/委屈、left=向左倾（疑惑）、right=向右倾（疑惑）",
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
