/**
 * WALL-E CAM LINK (main-controller side of CAM_PROTOCOL v1)
 *
 * @file    walle_cam_link.h
 * @brief   UART1 control channel between this main MCU and the
 *          ESP32-S3-CAM module (docs/CAM_PROTOCOL.md): line-based ASCII
 *          commands ("CMD\n" -> "OK ..." / "ERR ...") plus async events
 *          ("EVT ..."). All camera/eye-display actions from the MCP tools
 *          and the web panel dispatch through this layer.
 *
 * Behaviour (per the protocol spec):
 *   - One in-flight command at a time, serialised by a mutex (§8).
 *   - Per-command timeouts/retries from §7; a timeout counts as a link
 *     fault, 3 consecutive faults mark the link down.
 *   - While the link is down the semantic commands fall back to the CAM
 *     module's HTTP API (CAM_MODULE_URL, §10); SHOW/ABORT have no HTTP
 *     equivalent and fail instead.
 *   - A background task PINGs every 10s while down to detect recovery;
 *     an incoming "EVT BOOT" re-runs the HELLO handshake (§5).
 *
 * Start() is non-blocking: with no CAM module attached the link simply
 * stays offline and everything falls back to HTTP.
 */

#pragma once

#include <atomic>
#include <functional>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class WalleCamLink {
public:
    static WalleCamLink& GetInstance();

    /// Configure UART1 (CAM_UART_TX_PIN/RX_PIN @ CAM_UART_BAUD) and start
    /// the RX and link-health tasks. Safe to call with no CAM attached.
    void Start();

    /// True while the UART link to the CAM module is considered up.
    bool IsOnline() const { return online_.load(); }

    /// The CAM module's current IP address ("" when unknown).
    /// Updated after HELLO/STATUS and WIFI_CREDS responses.
    std::string GetCamIp() const { return cam_ip_; }

    /// True while the CAM module is recording (REC START .. REC STOP /
    /// EVT REC_DONE).
    bool IsRecording() const { return recording_.load(); }

    /// Error code from the last ERR response ("" when none / cleared).
    std::string last_error();

    /// Raw serialised command (§8). Sends "<cmd>\n" and collects the
    /// response line(s); LIST multi-line blocks are aggregated into one
    /// response. Returns true only for an "OK" response. On timeout
    /// *timed_out (if given) is set; on an ERR response it stays false
    /// and the code is available via last_error(). When the link is down
    /// the UART attempt is skipped and reported as a timeout so callers
    /// fall back to HTTP immediately.
    bool Command(const char* cmd, std::string& response, int timeout_ms,
                 int retries = 0, bool* timed_out = nullptr);

    // -- Semantic commands (§3). UART first, HTTP fallback when the link
    //    is down or the UART attempt timed out (§10). All file paths are
    //    normalised to full SD paths ("/photos/...", "/videos/...").

    /// HELLO handshake + STATUS refresh (§5). UART only (link management).
    bool Hello();

    /// STATUS snapshot ("sd=1 busy=0 rec=0 expr=... ..."). HTTP fallback
    /// queries /eyes (the only liveness endpoint) and returns its JSON.
    bool GetStatus(std::string& status_out);

    /// EYES <expr>; expr must be neutral/sad/left/right.
    bool SetEyes(const std::string& expr);

    /// PHOTO: full capture flow on the CAM side (~4.5s, §4), path_out
    /// receives e.g. "/photos/IMG_0007.jpg". 8s timeout, never retried.
    bool TakePhoto(std::string& path_out);

    /// REC START (1s timeout, 1 retry). Sets IsRecording() on success.
    bool RecordStart(std::string& path_out);

    /// REC STOP (2s timeout, never retried). Clears IsRecording().
    bool RecordStop(std::string& path_out, int& frames_out);

    /// SHOW LATEST: replay the newest photo on the CAM eye displays.
    /// UART only - returns false when the link is down.
    bool ShowLatest(std::string& path_out);

    /// SHOW <path>: replay a specific photo. UART only.
    bool Show(const std::string& path);

    /// ABORT: cancel an in-progress capture flow / replay. UART only.
    bool Abort();

    /// Sync WiFi credentials (SSID + password) to the CAM module so it can
    /// connect to the same network. Uses SsidManager to read saved creds.
    /// Returns true on success (CAM replied OK WIFI <ip>).
    bool SyncWifi();

    /// Lock onto the current largest face: CAM captures face + torso HSV
    /// colour histogram as the person reference for body-tracking fallback.
    /// Returns true on CAM "OK LOCK".
    bool LockPerson();

    /// Clear the locked reference. Returns true on CAM "OK UNLOCK".
    bool UnlockPerson();

    // -- EVT TRACK callback (face/body tracking events from CAM module) --
    struct TrackEvent {
        int type = 0;       // 0=no target, 1=face, 2=colour-matched body
        int x = 0, y = 0;   // bbox top-left (pixels, QVGA frame)
        int w = 0, h = 0;   // bbox size
        int conf = 0;       // confidence 0-100
    };
    using TrackCallback = std::function<void(const TrackEvent&)>;
    void SetTrackCallback(TrackCallback cb) { track_cb_ = std::move(cb); }

private:
    WalleCamLink() = default;
    WalleCamLink(const WalleCamLink&) = delete;
    WalleCamLink& operator=(const WalleCamLink&) = delete;

    enum class WaitResult { kOk, kErr, kTimeout };

    /// Command() without the link-down gate; used by the health task for
    /// recovery probes (PING/HELLO must be able to run while down).
    bool DoCommand(const char* cmd, std::string& response, int timeout_ms,
                   int retries, bool* timed_out);

    /// Collect response lines until a complete OK/ERR response arrives
    /// (aggregating LIST blocks when is_list) or the deadline passes.
    WaitResult WaitResponse(bool is_list, std::string& response, int timeout_ms);

    void HandleLine(const char* line);          // RX task context
    void HandleEvent(const char* line);         // RX task context

    static void RxTaskEntry(void* arg);
    void RxMain();
    static void HealthTaskEntry(void* arg);
    void HealthMain();

    bool started_ = false;
    std::atomic<bool> online_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> hello_pending_{false};
    std::atomic<int> consecutive_timeouts_{0};
    char last_error_[32] = {0};                 // written under cmd_mutex_
    std::string cam_ip_;                        // updated after STATUS / WIFI responses
    SemaphoreHandle_t cmd_mutex_ = nullptr;     // serialises §8 in-flight commands
    QueueHandle_t line_queue_ = nullptr;        // response lines (RX -> Command)
    TaskHandle_t health_task_ = nullptr;        // notified by EVT BOOT
    TrackCallback track_cb_;                    // EVT TRACK handler
    int last_track_type_ = -1;                   // for change-logging (avoid spam)
};
