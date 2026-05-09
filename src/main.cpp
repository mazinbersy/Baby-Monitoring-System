#include "Arduino.h"
#include "esp_camera.h"
#include "WiFi.h"
#include "HTTPClient.h"

// ─── Configuration ────────────────────────────────────────────────────────────
// Edit these four values; everything else is derived from them.
#define WIFI_SSID    "iPhone"
#define WIFI_PASS    "12345678"
#define SERVER_BASE  "http://172.20.10.3:3000"   // change to your server URL
#define DEVICE_ID    "baby_monitor_1"
#define DEVICE_KEY   "bms-secret-key-2024"       // must match server config.js

// ─── UART (Nucleo-L432KC → ESP32-CAM) ────────────────────────────────────────
//   Nucleo TX  →  ESP32-CAM GPIO 13  (UART2 RX)
//   ESP32-CAM GND  →  Nucleo GND     (common ground — required)
//   Both boards run at 3.3 V logic, no level-shifter needed.
//   NOTE: GPIO 16 cannot be used — it is the PSRAM chip-select on ESP32-CAM.
#define NUCLEO_RX_PIN  13
#define NUCLEO_TX_PIN  -1    // TX not used; -1 disables the pin
#define NUCLEO_BAUD    115200

// ─── Camera pin map (AI-Thinker ESP32-CAM) ───────────────────────────────────
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ─── Timing ───────────────────────────────────────────────────────────────────
static const unsigned long STATUS_INTERVAL_MS = 2000;
static const unsigned long WIFI_RETRY_MS      = 5000;

// ─── Runtime state ────────────────────────────────────────────────────────────
static bool          s_streaming       = false;
static unsigned long s_lastStatusMs    = 0;
static unsigned long s_lastWifiRetryMs = 0;

// ─── UART line buffer ─────────────────────────────────────────────────────────
static char s_uartBuf[64];
static int  s_uartPos = 0;

// ─── WiFi helpers ─────────────────────────────────────────────────────────────

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

static void ensureWifi() {
    if (wifiUp()) return;
    unsigned long now = millis();
    if (now - s_lastWifiRetryMs < WIFI_RETRY_MS) return;
    s_lastWifiRetryMs = now;
    Serial.println("[WiFi] Reconnecting...");
    WiFi.disconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ─── HTTP helpers ─────────────────────────────────────────────────────────────

static int postJson(const char* path, const String& body) {
    HTTPClient h;
    h.begin(String(SERVER_BASE) + path);
    h.addHeader("Content-Type", "application/json");
    h.addHeader("x-device-key", DEVICE_KEY);
    h.setTimeout(5000);
    int code = h.POST(body);
    h.end();
    return code;
}

static void sendAlert(const char* type, const char* message) {
    // Build JSON manually — no extra library needed.
    String body;
    body.reserve(128);
    body  = "{\"device_id\":\"";  body += DEVICE_ID;
    body += "\",\"type\":\"";     body += type;
    body += "\",\"message\":\"";  body += message;
    body += "\"}";

    int code = postJson("/api/alerts", body);
    if (code != 200 && code != 201)
        Serial.printf("[Alert] POST /api/alerts failed: %d\n", code);
    else
        Serial.printf("[Alert] Sent %s\n", type);
}

// ─── UART parsing ─────────────────────────────────────────────────────────────

static void dispatchLine(const char* line) {
    Serial.printf("[UART] <- %s\n", line);

    if      (strcmp(line, "CRY") == 0) sendAlert("CRYING",   "Baby crying detected");
    // Add more codes here as the Nucleo firmware implements them:
    // else if (strcmp(line, "HOT") == 0) sendAlert("TEMP_HIGH", "Temperature too high");
    // else if (strcmp(line, "CLD") == 0) sendAlert("TEMP_LOW",  "Temperature too low");
    // else if (strcmp(line, "MOV") == 0) sendAlert("MOVEMENT",  "Unexpected movement detected");
    else Serial.printf("[UART] Unknown code: %s\n", line);
}

static void pollUart() {
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        Serial.printf("[UART] raw byte: 0x%02X ('%c')\n", (uint8_t)c, c >= 32 ? c : '?');
        if (c == '\n' || c == '\r') {
            if (s_uartPos > 0) {
                s_uartBuf[s_uartPos] = '\0';
                if (wifiUp())
                    dispatchLine(s_uartBuf);
                else
                    Serial.printf("[UART] WiFi down — dropped: %s\n", s_uartBuf);
                s_uartPos = 0;
            }
        } else if (s_uartPos < (int)sizeof(s_uartBuf) - 1) {
            s_uartBuf[s_uartPos++] = c;
        }
    }
}

// ─── Status polling ───────────────────────────────────────────────────────────

static void pollStatus() {
    if (!wifiUp()) return;
    unsigned long now = millis();
    if (now - s_lastStatusMs < STATUS_INTERVAL_MS) return;
    s_lastStatusMs = now;

    HTTPClient h;
    h.begin(String(SERVER_BASE) + "/api/status");
    h.addHeader("x-device-key", DEVICE_KEY);
    h.setTimeout(3000);
    int code = h.GET();
    if (code == 200) {
        bool newState = h.getString() == "1";
        if (newState != s_streaming) {
            s_streaming = newState;
            Serial.println(s_streaming ? "[Cam] Streaming ON" : "[Cam] Streaming OFF");
        }
    }
    h.end();
}

// ─── Frame capture & upload ───────────────────────────────────────────────────

static void captureAndSend() {
    if (!s_streaming || !wifiUp()) return;

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[Cam] Frame capture failed"); return; }

    HTTPClient h;
    h.begin(String(SERVER_BASE) + "/api/frame");
    h.addHeader("Content-Type", "image/jpeg");
    h.addHeader("x-device-key", DEVICE_KEY);
    h.setTimeout(5000);
    int code = h.POST(fb->buf, fb->len);
    if (code != 200 && code != 201)
        Serial.printf("[Cam] Frame POST failed: %d\n", code);
    h.end();

    esp_camera_fb_return(fb);
}

// ─── Camera init ──────────────────────────────────────────────────────────────

static bool initCamera() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sscb_sda = SIOD_GPIO_NUM;
    cfg.pin_sscb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        cfg.frame_size   = FRAMESIZE_VGA;
        cfg.jpeg_quality = 10;
        cfg.fb_count     = 2;
    } else {
        cfg.frame_size   = FRAMESIZE_QVGA;
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 1;
    }

    return esp_camera_init(&cfg) == ESP_OK;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // UART2 — receive alert strings from Nucleo
    Serial2.begin(NUCLEO_BAUD, SERIAL_8N1, NUCLEO_RX_PIN, NUCLEO_TX_PIN);
    Serial.printf("[UART] Serial2 ready — RX on GPIO %d\n", NUCLEO_RX_PIN);

    if (!initCamera()) {
        Serial.println("[Cam] Init FAILED — halting");
        while (true) delay(1000);
    }
    Serial.println("[Cam] Init OK");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting to ");
    Serial.print(WIFI_SSID);
    unsigned long t0 = millis();
    while (!wifiUp() && millis() - t0 < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    if (wifiUp()) {
        Serial.print("[WiFi] Connected — IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WiFi] Initial connect timed out — will retry in loop");
    }
}

// ─── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    ensureWifi();        // reconnect if dropped
    pollUart();          // read Nucleo alert strings (non-blocking)
    pollStatus();        // ask server whether streaming is requested
    captureAndSend();    // upload one frame if streaming is active
}
