#include "Arduino.h"
#include "esp_camera.h"
#include "WiFi.h"
#include "HTTPClient.h"

const char* WIFI_SSID = "Mazin";
const char* WIFI_PASS = "12345678";

const char* SERVER_URL = "http://172.20.10.12:3000/frame";


// AI-Thinker pin map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count     = 2;
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
    }

    return esp_camera_init(&config) == ESP_OK;
}

void setup() {
    Serial.begin(115200);

    if (!initCamera()) {
        Serial.println("Camera init failed!");
        while (true) delay(1000);
    }
    Serial.println("Camera OK");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.println("WiFi connected");
    Serial.println(WiFi.localIP());
}

HTTPClient http;
bool httpConnected = false;

void loop() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        delay(100);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!httpConnected) {
            http.begin(SERVER_URL);
            http.addHeader("Content-Type", "image/jpeg");
            http.setReuse(true);
            httpConnected = true;
        }
        int code = http.POST(fb->buf, fb->len);
        if (code != 200) {
            Serial.printf("POST failed: %d\n", code);
            http.end();
            httpConnected = false;
        }
    }

    esp_camera_fb_return(fb);
}