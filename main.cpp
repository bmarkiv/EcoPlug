#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include "WiFiManager.h"
#include "index_html_gz.h"

#define MAX_DURATION_ON 5 * 3600 * 1000  // 5 hours
#define PIN_BUTTON      14
#define PIN_RELAY       27
#define PIN_BLUE_LED    26

unsigned long switch_off_time = 0;
static long c = 0;
static bool last_button_state = HIGH;
static unsigned long last_button_time = 0;
static unsigned long last_led_toggle_time = 0;
static bool led_state = LOW;
static String ws_msg;

AsyncWebSocket ws("/ws");
AsyncWebServer server(80);
WiFiManager WiFiManager(server);

// -------------------- Logging --------------------
void log(const char* format, ...) {
    char buffer[256];
    unsigned long now = millis();
    auto ms = now % 1000, s = now / 1000, m = s / 60, h = m / 60, d = h / 24;
    auto size = snprintf(buffer, sizeof(buffer), "%lu %02lu:%02lu:%02lu.%03lu: ", d, h % 24, m % 60, s % 60, ms);
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + size, sizeof(buffer) - size, format, args);
    va_end(args);
    Serial.println(buffer);
    if (ws.count()) ws.textAll(buffer);
}

// -------------------- IO Setup --------------------
void setup_io() {
    pinMode(PIN_RELAY, OUTPUT);      digitalWrite(PIN_RELAY, LOW);
    pinMode(PIN_BLUE_LED, OUTPUT);   digitalWrite(PIN_BLUE_LED, LOW);
    pinMode(PIN_BUTTON, INPUT_PULLUP);  // safer for active-low wiring
}

void switch_state(bool on) {
    digitalWrite(PIN_RELAY, on ? HIGH : LOW);
    log("Switch state changed: %s", on ? "ON" : "OFF");
}

int find_char(const char *str, char c) {
    for (int i = 0; str[i]; i++) if (str[i] == c) return i;
    return -1;
}

unsigned long change_state(const char *data) {
    auto now = millis();
    if (data[0] == 'f') return now;
    switch_off_time = 0;
	switch_state(false);
    if (data[0] < '0' || data[0] > '9') {
        log("Invalid URL: '%s'", data);
        return now;
    }
    long duration_on = atol(data);
    switch_off_time = now + duration_on * 1000;
    log("duration_on: %ld, switch_off_time: %ld", duration_on, switch_off_time - now);
    switch_state(duration_on > 0);
    return now;
}

// -------------------- Web Server --------------------
void sendStateAndUptime(unsigned long now) {
    if (!ws.count()) {
        log("sendStateAndUptime: ws.count() == 0");
        return;
    }
    ws_msg = R"({"remaining_time":")";
    ws_msg += String((switch_off_time > now) ? (switch_off_time - now) / 1000 : 0);
    ws_msg += R"(","uptime":")";
    ws_msg += String(now / 1000);
    ws_msg += R"("})";
    log("sendStateAndUptime: %s", ws_msg.c_str());
    ws.textAll(ws_msg);
}

void handleWebSocketMessage(void *arg, char *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        char msg[128];
        len = min(len, sizeof(msg) - 1);
        memcpy(msg, data, len);
        msg[len] = 0;
        log("handleWebSocketMessage: '%s', len: %d", msg, len);
        int colon = find_char(msg, ':');
        if (colon < 0) {
            log("Unsupported command: %s", msg);
            return;
        }
        sendStateAndUptime(change_state(msg + colon + 1));
    } else {
        log("Unsupported command: final:%d index:%d len:%d opcode:%d", info->final, info->index, info->len, info->opcode);
    }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            sendStateAndUptime(millis());
            log("WebSocket client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            log("WebSocket client #%u disconnected", client->id());
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(arg, (char *)data, len);
            break;
        default: break;
    }
}

void handleNotFound(AsyncWebServerRequest *request) {
    const char *uri = request->url().c_str();
    log("uri-raw: %s", uri);
    while (*uri == '/') uri++;
    if (!isdigit(uri[0])) {
        request->send(404, "text/plain", "Not found");
        return;
    }
    sendStateAndUptime(change_state(uri));
    request->send(200, "text/plain", "OK");
}

void start_web_server() {
	server.reset();
	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(
            200, "text/html", index_html_gz, index_html_gz_len);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });
	WiFiManager.registerResetWiFi();
	server.on("/restart", HTTP_GET, [](AsyncWebServerRequest* req) {
		req->send(200, "text/plain", "Restarting...");
		delay(500);
		ESP.restart();
	});

    server.onNotFound(handleNotFound);
    server.addHandler(&ws);
    server.begin();
    log("HTTP server started");
	//==============================
    ws.onEvent(onEvent);
    ArduinoOTA.onStart([]() {
        switch_state(false);  // ensure relay is off during OTA
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        log("Start updating %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        log("OTA update finished—restarting now");
        ESP.restart();
    });

    ArduinoOTA.begin();
    log("OTA service started");	
}

// -------------------- Main Loop --------------------
void setup(void) {
    ws_msg.reserve(256);
    setup_io();
    Serial.begin(115200);

	WiFiManager.begin(start_web_server);
}

void loop(void) {
    delay(2);
    c++;
    if (c % 100 == 0) {
        ArduinoOTA.handle();
        ws.cleanupClients();
		WiFiManager.handle();
    }

    auto now = millis();

    bool button = digitalRead(PIN_BUTTON);
    if (button == LOW && last_button_state == HIGH && now - last_button_time > 200) {
        last_button_time = now;
		bool is_on = switch_off_time > 0;
		switch_off_time = is_on ? 0 : now + MAX_DURATION_ON;
		switch_state(!is_on);
        sendStateAndUptime(now);
        log("Button pressed — toggled switch");
    }
    last_button_state = button;

    if (switch_off_time && now > switch_off_time) {
		switch_off_time = 0;
        switch_state(false);
        sendStateAndUptime(now);
        log("Switch turned off due to timeout");
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (now - last_led_toggle_time >= 500) {
            led_state = !led_state;
            digitalWrite(PIN_BLUE_LED, led_state);
            last_led_toggle_time = now;
        }
    } else {
        digitalWrite(PIN_BLUE_LED, LOW);
    }
}
