/**
 * WALL-E CAM LINK (main-controller side of CAM_PROTOCOL v1)
 *
 * @file    walle_cam_link.cc
 * @brief   Implementation, see walle_cam_link.h. Protocol reference:
 *          docs/CAM_PROTOCOL.md (§3 commands, §5 events, §7 timeouts,
 *          §8 serialisation, §10 HTTP fallback).
 */

#include "walle_cam_link.h"
#include "config.h"
#include "ssid_manager.h"

#include <driver/uart.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "WalleCamLink"

// Timeouts/retries per protocol §7.
static constexpr int kControlTimeoutMs  = 500;   // HELLO/PING/STATUS/EYES/ABORT/LIST (1 retry)
static constexpr int kShowTimeoutMs     = 1000;  // SHOW (1 retry)
static constexpr int kRecStartTimeoutMs = 1000;  // REC START (1 retry)
static constexpr int kRecStopTimeoutMs  = 2000;  // REC STOP (no retry: flushing the AVI index)
static constexpr int kPhotoTimeoutMs    = 8000;  // PHOTO (no retry: avoid double captures)

static constexpr int kMaxConsecutiveTimeouts = 3;     // §7 link-down threshold
static constexpr int kHealthPeriodMs         = 10000; // background PING period
static constexpr size_t kMaxLineLen          = 256;   // §2 single-line limit
static constexpr UBaseType_t kLineQueueDepth = 6;

static constexpr uart_port_t kUartPort = UART_NUM_1;

/// Queue element for completed response lines (RX task -> Command()).
struct CamRxLine {
    char text[kMaxLineLen + 2];
};


WalleCamLink& WalleCamLink::GetInstance() {
    static WalleCamLink instance;
    return instance;
}


// -------------------------------------------------------------------
// HTTP fallback helpers (§10; same esp_http_client style as the rest of
// this board directory)
// -------------------------------------------------------------------

struct HttpResponse {
    std::string body;
    int status = 0;
};

static esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        auto* resp = static_cast<HttpResponse*>(evt->user_data);
        resp->body.append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

/// GET CAM_MODULE_URL + path; returns true on HTTP 200 with body_out filled.
static bool HttpGet(const std::string& path, std::string& body_out, int timeout_ms = 3000) {
    std::string url = std::string(CAM_MODULE_URL) + path;
    HttpResponse resp;

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.event_handler = HttpEventHandler;
    cfg.user_data = &resp;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) resp.status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP fallback %s failed: %s", url.c_str(), esp_err_to_name(err));
        return false;
    }
    if (resp.status != 200) {
        ESP_LOGW(TAG, "HTTP fallback %s -> HTTP %d", url.c_str(), resp.status);
        return false;
    }
    ESP_LOGI(TAG, "HTTP fallback %s -> %s", path.c_str(), resp.body.c_str());
    body_out = std::move(resp.body);
    return true;
}

/// Extract "key":"value" from the flat JSON the cam firmware sends.
static bool ExtractJsonString(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

/// Extract "key":<int>; returns -1 when absent.
static int ExtractJsonInt(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    return atoi(json.c_str() + pos + needle.size());
}

static bool HttpStatusOk(const std::string& body) {
    return body.find("\"status\":\"OK\"") != std::string::npos;
}

/// URL-encode a string into dst (caller provides buffer >= 3 * strlen(src) + 1).
static void UrlEncode(char *dst, const char *src) {
    while (*src) {
        unsigned char c = static_cast<unsigned char>(*src);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = static_cast<char>(c);
        } else {
            snprintf(dst, 4, "%%%02X", c);
            dst += 3;
        }
        src++;
    }
    *dst = '\0';
}

/// The HTTP API returns bare file names ("IMG_0007.jpg"), the UART
/// protocol full paths ("/photos/IMG_0007.jpg") - normalise to the latter.
static std::string FullSdPath(const std::string& name, const char* dir) {
    if (name.find('/') != std::string::npos) return name;
    return std::string(dir) + name;
}


// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

void WalleCamLink::Start() {
    if (started_) return;

    uart_config_t uart_config = {};
    uart_config.baud_rate = CAM_UART_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(kUartPort, 2048, 1024, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s - CAM link disabled (HTTP fallback only)",
                 esp_err_to_name(err));
        return;
    }
    err = uart_param_config(kUartPort, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(kUartPort, CAM_UART_TX_PIN, CAM_UART_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s - CAM link disabled (HTTP fallback only)",
                 esp_err_to_name(err));
        uart_driver_delete(kUartPort);
        return;
    }

    cmd_mutex_ = xSemaphoreCreateMutex();
    line_queue_ = xQueueCreate(kLineQueueDepth, sizeof(CamRxLine));
    if (!cmd_mutex_ || !line_queue_) {
        ESP_LOGE(TAG, "Out of memory - CAM link disabled");
        return;
    }

    xTaskCreate(RxTaskEntry, "cam_link_rx", 4096, this, 5, nullptr);
    xTaskCreate(HealthTaskEntry, "cam_link_hlth", 4096, this, 3, &health_task_);

    started_ = true;
    ESP_LOGI(TAG, "CAM UART link started: UART1 TX=GPIO%d RX=GPIO%d @ %d (CAM_PROTOCOL v1)",
             (int)CAM_UART_TX_PIN, (int)CAM_UART_RX_PIN, CAM_UART_BAUD);
}


// -------------------------------------------------------------------
// RX task: line assembly + event dispatch
// -------------------------------------------------------------------

void WalleCamLink::RxTaskEntry(void* arg) {
    auto* self = static_cast<WalleCamLink*>(arg);
    self->RxMain();
    vTaskDelete(nullptr);
}

void WalleCamLink::RxMain() {
    uint8_t chunk[64];
    std::string line;
    line.reserve(kMaxLineLen);

    while (true) {
        int n = uart_read_bytes(kUartPort, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            char c = static_cast<char>(chunk[i]);
            if (c == '\r') continue;                 // §2: \r is ignored
            if (c == '\n') {
                if (!line.empty()) {
                    HandleLine(line.c_str());
                    line.clear();
                }
                continue;
            }
            if (line.size() < kMaxLineLen) {
                line.push_back(c);
            } else {
                ESP_LOGW(TAG, "RX line over %d bytes - discarding", (int)kMaxLineLen);
                line.clear();
            }
        }
    }
}

void WalleCamLink::HandleLine(const char* line) {
    ESP_LOGD(TAG, "<< %s", line);
    if (strncmp(line, "EVT ", 4) == 0) {
        HandleEvent(line);
        return;
    }
    CamRxLine item;
    strlcpy(item.text, line, sizeof(item.text));
    if (xQueueSend(line_queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Response line dropped (queue full): %s", line);
    }
}

void WalleCamLink::HandleEvent(const char* line) {
    if (strncmp(line, "EVT BOOT", 8) == 0) {
        // §5: CAM (re)booted - mark the link up and answer with HELLO 1
        // from the health task (RX context must not block on the mutex).
        int proto = atoi(line + 8);
        ESP_LOGI(TAG, "CAM module booted (proto %d) - answering HELLO", proto);
        consecutive_timeouts_.store(0);
        online_.store(true);
        hello_pending_.store(true);
        if (health_task_) xTaskNotifyGive(health_task_);
    } else if (strncmp(line, "EVT REC_DONE", 12) == 0) {
        // §5: recording aborted on the CAM side (SD full / write failure)
        ESP_LOGW(TAG, "Recording aborted by CAM module:%s", line + 12);
        recording_.store(false);
    } else if (strncmp(line, "EVT TRACK", 9) == 0) {
        // §5: face/body tracking event — "EVT TRACK <type> <x> <y> <w> <h> <conf>"
        TrackEvent evt;
        int n = sscanf(line + 10, "%d %d %d %d %d %d",
                       &evt.type, &evt.x, &evt.y, &evt.w, &evt.h, &evt.conf);
        if (n >= 2 && evt.type >= 0 && evt.type <= 2) {
            if (evt.type != last_track_type_) {
                ESP_LOGI(TAG, "EVT TRACK type=%d (%d,%d %dx%d) conf=%d",
                         evt.type, evt.x, evt.y, evt.w, evt.h, evt.conf);
                last_track_type_ = evt.type;
            }
            if (track_cb_) track_cb_(evt);
        } else {
            ESP_LOGW(TAG, "Malformed EVT TRACK: %s", line);
        }
    } else if (strncmp(line, "EVT ERR", 7) == 0) {
        // §5: reserved in v1 - log only
        ESP_LOGW(TAG, "CAM module async error:%s", line + 7);
    } else {
        ESP_LOGW(TAG, "Unknown CAM event: %s", line);
    }
}


// -------------------------------------------------------------------
// Command transport (§7 timeouts/retries, §8 serialisation)
// -------------------------------------------------------------------

bool WalleCamLink::Command(const char* cmd, std::string& response, int timeout_ms,
                           int retries, bool* timed_out) {
    if (!online_.load()) {
        // Link down: skip UART, caller falls back to HTTP (§10).
        response.clear();
        if (timed_out) *timed_out = true;
        return false;
    }
    return DoCommand(cmd, response, timeout_ms, retries, timed_out);
}

bool WalleCamLink::DoCommand(const char* cmd, std::string& response, int timeout_ms,
                             int retries, bool* timed_out) {
    if (timed_out) *timed_out = false;
    response.clear();
    if (!started_ || !cmd_mutex_) {
        if (timed_out) *timed_out = true;
        return false;
    }

    xSemaphoreTake(cmd_mutex_, portMAX_DELAY);

    WaitResult result = WaitResult::kTimeout;
    const bool is_list = strncmp(cmd, "LIST", 4) == 0;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) ESP_LOGW(TAG, "Retrying '%s' (attempt %d/%d)", cmd, attempt + 1, retries + 1);
        // Drop stale lines (e.g. a late response to a previously timed-out
        // command). A late response can still race the next command - v1
        // has no sequence numbers, so this is best effort.
        xQueueReset(line_queue_);
        uart_write_bytes(kUartPort, cmd, strlen(cmd));
        uart_write_bytes(kUartPort, "\n", 1);
        ESP_LOGD(TAG, ">> %s", cmd);
        result = WaitResponse(is_list, response, timeout_ms);
        if (result != WaitResult::kTimeout) break;
    }

    if (result == WaitResult::kTimeout) {
        int faults = consecutive_timeouts_.fetch_add(1) + 1;
        ESP_LOGW(TAG, "'%s' timed out (%d consecutive)", cmd, faults);
        if (faults >= kMaxConsecutiveTimeouts && online_.load()) {
            online_.store(false);
            ESP_LOGW(TAG, "CAM link down - falling back to HTTP");
        }
        if (timed_out) *timed_out = true;
    } else {
        consecutive_timeouts_.store(0);
        if (!online_.load()) {
            online_.store(true);
            ESP_LOGI(TAG, "CAM link online");
        }
        if (result == WaitResult::kErr) {
            // "ERR <CODE> [msg]" - keep the code for the caller (§6)
            const char* p = response.c_str() + 3;
            while (*p == ' ') p++;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof(last_error_) - 1) {
                last_error_[i] = p[i];
                i++;
            }
            last_error_[i] = '\0';
        }
    }

    xSemaphoreGive(cmd_mutex_);
    return result == WaitResult::kOk;
}

WalleCamLink::WaitResult WalleCamLink::WaitResponse(bool is_list, std::string& response,
                                                    int timeout_ms) {
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool in_list_block = false;

    while (true) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return WaitResult::kTimeout;

        CamRxLine item;
        if (xQueueReceive(line_queue_, &item,
                          pdMS_TO_TICKS(remaining_us / 1000) + 1) != pdTRUE) {
            return WaitResult::kTimeout;
        }
        const char* s = item.text;

        if (strncmp(s, "ERR", 3) == 0 && (s[3] == '\0' || s[3] == ' ')) {
            response = s;
            ESP_LOGW(TAG, "'%s'", s);
            return WaitResult::kErr;
        }
        if (strncmp(s, "OK", 2) == 0 && (s[2] == '\0' || s[2] == ' ')) {
            if (!is_list) {
                response = s;
                return WaitResult::kOk;
            }
            // §3 LIST block: "OK BEGIN <n>" -> n "F <path> <size>" lines -> "OK END"
            if (!in_list_block && strncmp(s, "OK BEGIN", 8) == 0) {
                in_list_block = true;
                response = s;
                continue;
            }
            if (in_list_block && strcmp(s, "OK END") == 0) {
                response += '\n';
                response += s;
                return WaitResult::kOk;
            }
            ESP_LOGD(TAG, "Ignoring unexpected line inside LIST block: %s", s);
            continue;
        }
        if (in_list_block) {
            response += '\n';
            response += s;
            continue;
        }
        ESP_LOGD(TAG, "Ignoring stray line: %s", s);
    }
}


// -------------------------------------------------------------------
// Health task: HELLO after EVT BOOT, PING probes while down (§5/§7)
// -------------------------------------------------------------------

void WalleCamLink::HealthTaskEntry(void* arg) {
    auto* self = static_cast<WalleCamLink*>(arg);
    self->HealthMain();
    vTaskDelete(nullptr);
}

void WalleCamLink::HealthMain() {
    while (true) {
        // Wakes on xTaskNotifyGive (EVT BOOT) or every kHealthPeriodMs.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kHealthPeriodMs));

        if (hello_pending_.exchange(false)) {
            Hello();
            continue;
        }
        if (!online_.load()) {
            std::string resp;
            if (DoCommand("PING", resp, kControlTimeoutMs, 1, nullptr)) {
                ESP_LOGI(TAG, "CAM link recovered");
                // Push WiFi credentials now that the link is up
                SyncWifi();
            }
        }
    }
}


// -------------------------------------------------------------------
// Semantic commands (§3) with HTTP fallback (§10)
// -------------------------------------------------------------------

/// Copy the token following `prefix` (up to the next space) into `out`.
static bool TokenAfter(const std::string& s, const char* prefix, std::string& out) {
    size_t plen = strlen(prefix);
    if (s.compare(0, plen, prefix) != 0) return false;
    size_t end = s.find(' ', plen);
    out = s.substr(plen, end == std::string::npos ? end : end - plen);
    return !out.empty();
}

bool WalleCamLink::Hello() {
    std::string resp;
    if (!DoCommand("HELLO 1", resp, kControlTimeoutMs, 1, nullptr)) {
        return false;
    }
    // "OK CAM <fw_ver> <proto_ver>"
    ESP_LOGI(TAG, "CAM handshake: %s", resp.c_str());
    std::string status;
    if (GetStatus(status)) {   // §5: refresh state after (re)connect
        ESP_LOGI(TAG, "CAM status: %s", status.c_str());
        // Extract CAM IP from status for the web panel
        size_t ip_pos = status.find("ip=");
        if (ip_pos != std::string::npos) {
            size_t ip_end = status.find(' ', ip_pos);
            cam_ip_ = status.substr(ip_pos + 3, ip_end - ip_pos - 3);
        }
    }
    // Push current WiFi credentials so the CAM can connect to the same network
    SyncWifi();
    return true;
}

bool WalleCamLink::GetStatus(std::string& status_out) {
    std::string resp;
    bool timed_out = false;
    if (Command("STATUS", resp, kControlTimeoutMs, 1, &timed_out)) {
        // Strip the leading "OK " from "OK sd=1 busy=0 ..."
        status_out = (resp.compare(0, 3, "OK ") == 0) ? resp.substr(3) : resp;
        return true;
    }
    if (!timed_out) return false;
    // HTTP fallback: no status endpoint; /eyes is the only liveness probe.
    std::string body;
    if (!HttpGet("/eyes", body)) return false;
    status_out = body;
    return HttpStatusOk(body);
}

bool WalleCamLink::SetEyes(const std::string& expr) {
    if (expr != "neutral" && expr != "sad" && expr != "left" && expr != "right") {
        ESP_LOGW(TAG, "Unknown eye expression '%s'", expr.c_str());
        return false;
    }
    std::string cmd = "EYES " + expr;
    std::string resp;
    bool timed_out = false;
    if (Command(cmd.c_str(), resp, kControlTimeoutMs, 1, &timed_out)) {
        return true;
    }
    if (!timed_out) return false;
    std::string body;
    if (!HttpGet("/eyes?expr=" + expr, body)) return false;
    return HttpStatusOk(body);
}

bool WalleCamLink::TakePhoto(std::string& path_out) {
    std::string resp;
    bool timed_out = false;
    if (Command("PHOTO", resp, kPhotoTimeoutMs, 0, &timed_out)) {
        // "OK FILE <path>"
        if (TokenAfter(resp, "OK FILE ", path_out)) return true;
        ESP_LOGW(TAG, "Unexpected PHOTO response: %s", resp.c_str());
        return false;
    }
    if (!timed_out) return false;   // explicit ERR (BUSY/SD/...) in last_error()
    std::string body;
    if (!HttpGet("/capture", body, 5000)) return false;
    std::string name;
    if (!HttpStatusOk(body) || !ExtractJsonString(body, "file", name)) {
        ESP_LOGW(TAG, "HTTP /capture failed: %s", body.c_str());
        return false;
    }
    path_out = FullSdPath(name, "/photos/");
    return true;
}

bool WalleCamLink::RecordStart(std::string& path_out) {
    std::string resp;
    bool timed_out = false;
    if (Command("REC START", resp, kRecStartTimeoutMs, 1, &timed_out)) {
        // "OK REC ON <path>"
        if (TokenAfter(resp, "OK REC ON ", path_out)) {
            recording_.store(true);
            return true;
        }
        ESP_LOGW(TAG, "Unexpected REC START response: %s", resp.c_str());
        return false;
    }
    if (!timed_out) return false;
    std::string body;
    if (!HttpGet("/record?action=start", body)) return false;
    std::string name;
    if (!HttpStatusOk(body) || !ExtractJsonString(body, "file", name)) {
        ESP_LOGW(TAG, "HTTP /record?action=start failed: %s", body.c_str());
        return false;
    }
    path_out = FullSdPath(name, "/videos/");
    recording_.store(true);
    return true;
}

bool WalleCamLink::RecordStop(std::string& path_out, int& frames_out) {
    frames_out = 0;
    std::string resp;
    bool timed_out = false;
    if (Command("REC STOP", resp, kRecStopTimeoutMs, 0, &timed_out)) {
        // "OK REC OFF <path> <frames>"
        if (TokenAfter(resp, "OK REC OFF ", path_out)) {
            size_t pos = strlen("OK REC OFF ") + path_out.size();
            if (pos < resp.size() && resp[pos] == ' ') {
                frames_out = atoi(resp.c_str() + pos + 1);
            }
            recording_.store(false);
            return true;
        }
        ESP_LOGW(TAG, "Unexpected REC STOP response: %s", resp.c_str());
        return false;
    }
    if (!timed_out) return false;
    // The HTTP stop handler waits for the AVI to be finalised (~3s worst case)
    std::string body;
    if (!HttpGet("/record?action=stop", body, 6000)) return false;
    std::string name;
    if (!HttpStatusOk(body) || !ExtractJsonString(body, "file", name)) {
        ESP_LOGW(TAG, "HTTP /record?action=stop failed: %s", body.c_str());
        return false;
    }
    path_out = FullSdPath(name, "/videos/");
    int frames = ExtractJsonInt(body, "frames");
    if (frames > 0) frames_out = frames;
    recording_.store(false);
    return true;
}

bool WalleCamLink::ShowLatest(std::string& path_out) {
    if (!online_.load()) {
        ESP_LOGW(TAG, "SHOW LATEST unavailable: UART link down (no HTTP fallback)");
        return false;
    }
    std::string resp;
    if (Command("SHOW LATEST", resp, kShowTimeoutMs, 1, nullptr)) {
        // "OK SHOW <path>"
        if (TokenAfter(resp, "OK SHOW ", path_out)) return true;
        ESP_LOGW(TAG, "Unexpected SHOW response: %s", resp.c_str());
    }
    return false;
}

bool WalleCamLink::Show(const std::string& path) {
    if (!online_.load()) {
        ESP_LOGW(TAG, "SHOW unavailable: UART link down (no HTTP fallback)");
        return false;
    }
    std::string cmd = "SHOW " + path;
    std::string resp;
    return Command(cmd.c_str(), resp, kShowTimeoutMs, 1, nullptr);
}

bool WalleCamLink::Abort() {
    if (!online_.load()) {
        ESP_LOGW(TAG, "ABORT unavailable: UART link down (no HTTP fallback)");
        return false;
    }
    std::string resp;
    return Command("ABORT", resp, kControlTimeoutMs, 1, nullptr);
}

bool WalleCamLink::LockPerson() {
    if (!online_.load()) {
        ESP_LOGW(TAG, "LOCK unavailable: UART link down");
        return false;
    }
    std::string resp;
    // 1s timeout — CAM just needs to run face detect + compute histogram
    return Command("LOCK", resp, 1000, 1, nullptr);
}

bool WalleCamLink::UnlockPerson() {
    if (!online_.load()) {
        ESP_LOGW(TAG, "UNLOCK unavailable: UART link down");
        return false;
    }
    std::string resp;
    return Command("UNLOCK", resp, kControlTimeoutMs, 1, nullptr);
}

bool WalleCamLink::SyncWifi() {
    auto &ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "No saved WiFi credentials to sync");
        return false;
    }
    // First entry is the most-recently-connected SSID
    const auto &item = ssid_list[0];
    if (item.ssid.empty()) return false;

    // URL-encode so spaces/special chars survive the protocol line format
    char essid[128], epass[256];
    UrlEncode(essid, item.ssid.c_str());
    UrlEncode(epass, item.password.c_str());

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "WIFI_CREDS %s %s", essid, epass);

    ESP_LOGI(TAG, "Syncing WiFi to CAM: %s", item.ssid.c_str());
    std::string resp;
    // 15s timeout: WiFi reconnection on the CAM side can take a while
    bool ok = Command(cmd, resp, 15000, 1);
    if (ok) {
        ESP_LOGI(TAG, "CAM WiFi sync OK: %s", resp.c_str());
        // "OK WIFI <ip>" → store the IP
        if (resp.compare(0, 8, "OK WIFI ") == 0) {
            cam_ip_ = resp.substr(8);
        }
    } else {
        ESP_LOGW(TAG, "CAM WiFi sync failed: %s", last_error().c_str());
    }
    return ok;
}


// -------------------------------------------------------------------
// Accessors
// -------------------------------------------------------------------

std::string WalleCamLink::last_error() {
    if (!cmd_mutex_) return "";
    xSemaphoreTake(cmd_mutex_, portMAX_DELAY);
    std::string err = last_error_;
    xSemaphoreGive(cmd_mutex_);
    return err;
}
