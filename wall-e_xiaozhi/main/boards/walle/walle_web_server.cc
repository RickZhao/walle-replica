/**
 * WALL-E WEB CONTROL PANEL
 *
 * @file    walle_web_server.cc
 * @brief   Implementation, see walle_web_server.h
 */

#include "walle_web_server.h"
#include "walle_motion.h"
#include "walle_cam_link.h"
#include "config.h"

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "WalleWeb"


// -------------------------------------------------------------------
// Embedded control page (vanilla JS, mirrors the Flask frontend's API)
// -------------------------------------------------------------------

static const char kIndexHtml[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wall-E Control</title>
<style>
body{font-family:sans-serif;background:#101820;color:#e0e0e0;max-width:520px;margin:0 auto;padding:12px}
h1{color:#ffc832;font-size:22px}h2{font-size:16px;color:#8ab4f8;margin:18px 0 6px}
button{background:#2a3a4a;color:#fff;border:1px solid #4a5a6a;border-radius:6px;padding:10px 14px;margin:3px;font-size:15px}
button:active{background:#3a5a7a}
.row{display:flex;align-items:center;gap:8px;margin:4px 0}
label{width:90px;font-size:14px}
input[type=range]{flex:1}
.bat{font-size:18px;color:#8f8}
</style></head><body>
<h1>WALL-E</h1>
<div class="bat">Battery: <span id="bat">--</span></div>

<h2>Camera</h2>
<img id="cam" style="width:100%;border-radius:8px;background:#000" alt="camera stream">
<div id="camctl" style="display:none">
<button onclick="camPhoto()">Photo</button>
<button id="recbtn" onclick="camRec()">Start rec</button>
<button onclick="camFiles()">Files</button>
<button onclick="camView('preview')">Preview</button>
<button onclick="camView('replay')">Replay</button>
<button onclick="camView('stop')">Stop view</button>
</div>
<div id="cammsg" style="font-size:14px;color:#8f8"></div>
<div id="filebox" style="display:none;background:#1a2634;border-radius:8px;padding:8px;margin-top:6px;font-size:14px"></div>

<h2>Drive</h2>
<div>
<button onclick="drive(0,1)">&#9650;</button><br>
<button onclick="drive(-1,0)">&#9664;</button>
<button onclick="drive(0,0)">STOP</button>
<button onclick="drive(1,0)">&#9654;</button><br>
<button onclick="drive(0,-1)">&#9660;</button>
</div>
<div class="row"><label>Speed</label><input type="range" id="spd" min="20" max="100" value="60"></div>

<h2>Servos</h2>
<div id="sliders"></div>

<h2>Light</h2>
<div class="row"><label>Brightness</label><input type="range" min="0" max="100" value="0"
  onchange="post('/servoControl',{servo:'V',value:this.value})"></div>

<h2>Animations</h2>
<button onclick="post('/animate',{clip:0})">Reset</button>
<button onclick="post('/animate',{clip:1})">Boot eyes</button>
<button onclick="post('/animate',{clip:2})">Curious</button>
<button id="auto" onclick="toggleAuto()">Auto mode: ?</button>

<script>
// ESP32-S3-CAM MJPEG stream (wall-e_esp32_cam/);
// fill in the cam module's address, e.g. "http://192.168.1.50/stream".
// Leave empty to hide the camera area.
const stream_url = "";
if (stream_url) {
  document.getElementById('cam').src = stream_url;
} else {
  document.getElementById('cam').style.display = 'none';
}
// Camera module SD API (photo / record / files) - same origin as the stream
const camapi = stream_url ? new URL(stream_url).origin : "";
if (camapi) document.getElementById('camctl').style.display = 'block';
let recording = false;
function camMsg(t){document.getElementById('cammsg').textContent=t;}
function camPhoto(){camMsg('capturing...');
  fetch(camapi+'/capture').then(r=>r.json()).then(j=>{
    camMsg(j.file?('saved '+j.file+' ('+j.size+' B)'):(j.msg||'error'));})
  .catch(()=>camMsg('camera unreachable'));}
function camRec(){
  const action = recording?'stop':'start';
  fetch(camapi+'/record?action='+action).then(r=>r.json()).then(j=>{
    if(j.status==='OK'){recording=!recording;
      document.getElementById('recbtn').textContent=recording?'Stop rec':'Start rec';
      camMsg(recording?('recording '+j.file):('saved '+j.file+' ('+j.frames+' frames)'));}
    else camMsg(j.msg||'error');})
  .catch(()=>camMsg('camera unreachable'));}
function camFiles(){
  fetch(camapi+'/files').then(r=>r.json()).then(j=>{
    const box=document.getElementById('filebox');
    if(!j.files){camMsg(j.msg||'error');return;}
    box.style.display='block';
    box.innerHTML='<b>SD files</b> <button onclick="document.getElementById(\'filebox\').style.display=\'none\'">x</button><br>'+
      (j.files.map(f=>`${f.path} (${(f.size/1024).toFixed(0)} KB) `+
        `<a href="${camapi}/file?path=${f.path}" style="color:#8ab4f8">dl</a> `+
        `<a href="#" onclick="camDel('${f.path}');return false" style="color:#f88">del</a>`).join('<br>')||'empty');
  }).catch(()=>camMsg('camera unreachable'));}
function camDel(p){fetch(camapi+'/file?path='+p,{method:'DELETE'})
  .then(()=>camFiles()).catch(()=>camMsg('delete failed'));}
// Preview / stop view dispatch via the main controller to the cam module's
// local eye-display playback (CAM UART link); video replay is unsupported (v2)
function camView(a){camMsg(a+'...');
  fetch('/camview',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+a})
  .then(r=>r.json()).then(j=>camMsg(j.status==='OK'?(a=='stop'?'stopped':a+' started - see eye display'):(j.msg||'error')))
  .catch(()=>camMsg('request failed'));}
const servos=[['G','Head rot',50],['T','Neck top',50],['B','Neck bottom',0],
['E','Eye L',40],['U','Eye R',40],['L','Arm L',40],['R','Arm R',40],
['I','Brow L',50],['J','Brow R',50]];
const box=document.getElementById('sliders');
servos.forEach(([id,name,v])=>{
  const d=document.createElement('div');d.className='row';
  d.innerHTML=`<label>${name}</label><input type="range" min="0" max="100" value="${v}"
    onchange="post('/servoControl',{servo:'${id}',value:this.value})">`;
  box.appendChild(d);
});
let auto=false;
function post(url,params){fetch(url,{method:'POST',
  headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:new URLSearchParams(params)}).catch(e=>console.log(e));}
function drive(x,y){const s=document.getElementById('spd').value/100;
  post('/motor',{stickX:(x*s).toFixed(2),stickY:(y*s).toFixed(2)});}
function toggleAuto(){auto=!auto;
  document.getElementById('auto').textContent='Auto mode: '+(auto?'ON':'OFF');
  post('/settings',{type:'animeMode',value:auto?1:0});}
function pollBat(){fetch('/arduinoStatus',{method:'POST',
  headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'type=battery'})
  .then(r=>r.json()).then(j=>{if(j.battery!==undefined)
    document.getElementById('bat').textContent=j.battery+'%';}).catch(()=>{});}
pollBat();setInterval(pollBat,5000);
</script></body></html>)HTML";


// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

/// Read the full POST body (bounded) into a NUL-terminated string.
static std::string ReadBody(httpd_req_t* req) {
    if (req->content_len <= 0 || req->content_len > 1024) return "";
    std::string body(req->content_len + 1, '\0');
    int received = httpd_req_recv(req, body.data(), req->content_len);
    if (received <= 0) return "";
    body.resize(received);
    return body;
}

/// Extract a form field from an application/x-www-form-urlencoded body.
/// Handles percent-decoding and '+' spaces.
static bool FormParam(const std::string& body, const char* key, char* out, size_t out_size) {
    std::string prefix = std::string(key) + "=";
    size_t pos = body.find(prefix);
    if (pos == std::string::npos) return false;
    if (pos > 0 && body[pos - 1] != '&') {
        pos = body.find("&" + prefix);
        if (pos == std::string::npos) return false;
        pos += 1;
    }
    pos += prefix.length();
    size_t end = body.find('&', pos);
    if (end == std::string::npos) end = body.length();

    size_t j = 0;
    for (size_t i = pos; i < end && j + 1 < out_size; i++) {
        char c = body[i];
        if (c == '+') {
            out[j++] = ' ';
        } else if (c == '%' && i + 2 < end) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(body[i + 1]), lo = hex(body[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out[j++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return true;
}

static esp_err_t SendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t SendOk(httpd_req_t* req) {
    return SendJson(req, "{\"status\":\"OK\"}");
}

static esp_err_t SendErr(httpd_req_t* req, const char* msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"status\":\"Error\",\"msg\":\"%s\"}", msg);
    return SendJson(req, buf);
}


// -------------------------------------------------------------------
// Route handlers (same contract as web_interface/app.py)
// -------------------------------------------------------------------

static esp_err_t IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kIndexHtml, sizeof(kIndexHtml) - 1);
}

static esp_err_t MotorHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char sx[16], sy[16];
    if (!FormParam(body, "stickX", sx, sizeof(sx)) || !FormParam(body, "stickY", sy, sizeof(sy))) {
        return SendErr(req, "Unable to read POST data");
    }
    auto& motion = WalleMotion::GetInstance();
    motion.EvaluateCommand('X', (int)(atof(sx) * 100));
    motion.EvaluateCommand('Y', (int)(atof(sy) * 100));
    return SendOk(req);
}

static esp_err_t SettingsHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char type[32], value[16];
    if (!FormParam(body, "type", type, sizeof(type)) || !FormParam(body, "value", value, sizeof(value))) {
        return SendErr(req, "Unable to read POST data");
    }
    auto& motion = WalleMotion::GetInstance();

    if (strcmp(type, "motorOff") == 0) {
        motion.EvaluateCommand('O', atoi(value));
    } else if (strcmp(type, "steerOff") == 0) {
        motion.EvaluateCommand('S', atoi(value));
    } else if (strcmp(type, "animeMode") == 0) {
        motion.EvaluateCommand('M', atoi(value));
    } else if (strcmp(type, "restart") == 0) {
        SendOk(req);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strcmp(type, "volume") == 0) {
        // No local audio player in the IDF port (cloud TTS instead) - accept silently
    } else {
        return SendErr(req, "Unsupported setting on ESP32 firmware");
    }
    return SendOk(req);
}

static esp_err_t AnimateHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char clip[8];
    if (!FormParam(body, "clip", clip, sizeof(clip))) {
        return SendErr(req, "Unable to read POST data");
    }
    WalleMotion::GetInstance().EvaluateCommand('A', atoi(clip));
    return SendOk(req);
}

static esp_err_t ServoControlHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char servo[8], value[8];
    if (!FormParam(body, "servo", servo, sizeof(servo)) || !FormParam(body, "value", value, sizeof(value))) {
        return SendErr(req, "Unable to read POST data");
    }
    WalleMotion::GetInstance().EvaluateCommand(servo[0], atoi(value));
    return SendOk(req);
}

static esp_err_t ArduinoStatusHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char type[16];
    if (!FormParam(body, "type", type, sizeof(type)) || strcmp(type, "battery") != 0) {
        return SendErr(req, "Unable to read POST data");
    }
    int level = WalleMotion::GetInstance().battery_level();
    if (level < 0) return SendJson(req, "{\"status\":\"Info\",\"msg\":\"No battery level available\"}");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"battery\":%d}", level);
    return SendJson(req, buf);
}

static esp_err_t GamepadStatusHandler(httpd_req_t* req) {
    // No Bluetooth gamepad in the IDF port yet - always report disconnected
    return SendJson(req, "{\"status\":\"OK\",\"enabled\":false,\"active\":false,\"connected\":false}");
}

static esp_err_t CamViewHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    char action[16];
    if (!FormParam(body, "action", action, sizeof(action))) {
        return SendErr(req, "Unable to read POST data");
    }
    // CAM_PROTOCOL §11: preview/stop dispatch to the cam module's local
    // eye-display playback over the UART link (no HTTP equivalent).
    auto& cam = WalleCamLink::GetInstance();
    if (strcmp(action, "preview") == 0) {
        std::string path;
        if (!cam.ShowLatest(path)) {
            return SendErr(req, "eye-display preview unavailable (CAM UART link down or no photos)");
        }
    } else if (strcmp(action, "replay") == 0) {
        // Eye-display video replay is protocol v2; download via the cam page.
        return SendErr(req, "eye-display video replay not supported - download from the camera module page");
    } else if (strcmp(action, "stop") == 0) {
        if (!cam.Abort()) {
            return SendErr(req, "abort failed (CAM UART link down)");
        }
    } else {
        return SendErr(req, "unknown action, use preview/replay/stop");
    }
    return SendOk(req);
}


// -------------------------------------------------------------------
// Server lifecycle
// -------------------------------------------------------------------

static httpd_handle_t s_server = nullptr;

esp_err_t WalleWebServerStart() {
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = IndexHandler,         .user_ctx = nullptr },
        { .uri = "/motor",          .method = HTTP_POST, .handler = MotorHandler,         .user_ctx = nullptr },
        { .uri = "/settings",       .method = HTTP_POST, .handler = SettingsHandler,      .user_ctx = nullptr },
        { .uri = "/animate",        .method = HTTP_POST, .handler = AnimateHandler,       .user_ctx = nullptr },
        { .uri = "/servoControl",   .method = HTTP_POST, .handler = ServoControlHandler,  .user_ctx = nullptr },
        { .uri = "/arduinoStatus",  .method = HTTP_POST, .handler = ArduinoStatusHandler, .user_ctx = nullptr },
        { .uri = "/gamepadStatus",  .method = HTTP_GET,  .handler = GamepadStatusHandler, .user_ctx = nullptr },
        { .uri = "/gamepadStatus",  .method = HTTP_POST, .handler = GamepadStatusHandler, .user_ctx = nullptr },
        { .uri = "/camview",        .method = HTTP_POST, .handler = CamViewHandler,       .user_ctx = nullptr },
    };
    for (const auto& route : routes) {
        httpd_register_uri_handler(s_server, &route);
    }

    ESP_LOGI(TAG, "Wall-E web control panel started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

void WalleWebServerStop() {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
}
