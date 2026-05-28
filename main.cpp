#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
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

struct ScheduleConfig {
    bool enabled = false;
    uint8_t days_mask = 0x00;       // bit0=Sun ... bit6=Sat
    uint16_t start_minute = 8 * 60; // 08:00
    uint16_t duration_min = 120;    // 2h
};

static ScheduleConfig schedule_cfg;
static int32_t tz_offset_min = 0;
static int64_t utc_base_sec = 0;
static uint32_t utc_base_millis = 0;
static bool utc_synced = false;
static bool ntp_time_valid = false;
static bool ntp_started = false;
static unsigned long last_ntp_poll_ms = 0;
static long last_schedule_local_day = -1;
static long last_schedule_checked_sec = -1;
static Preferences sched_prefs;
static Preferences time_prefs;

void sendStateAndUptime(unsigned long now);
int64_t now_utc_sec();

AsyncWebSocket ws("/ws");
AsyncWebServer server(80);
WiFiManager WiFiManager(server);

// -------------------- Logging --------------------
void log(const char* format, ...) {
    char buffer[256];
    unsigned long now_ms = millis();
    int size = 0;

    if (utc_synced) {
        int64_t local_sec = now_utc_sec() - (int64_t)tz_offset_min * 60;
        unsigned long ms = now_ms % 1000;
        time_t t = (time_t)local_sec;
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        size = snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03lu: ",
                        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
    } else {
        auto ms = now_ms % 1000, s = now_ms / 1000, m = s / 60, h = m / 60, d = h / 24;
        size = snprintf(buffer, sizeof(buffer), "%lu %02lu:%02lu:%02lu.%03lu: ", d, h % 24, m % 60, s % 60, ms);
    }

    if (size < 0) size = 0;
    if (size >= (int)sizeof(buffer)) size = (int)sizeof(buffer) - 1;
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

void load_schedule() {
    sched_prefs.begin("schedule", false);
    schedule_cfg.enabled = sched_prefs.getBool("enabled", false);
    schedule_cfg.days_mask = (uint8_t)sched_prefs.getUChar("days", 0x00);
    schedule_cfg.start_minute = (uint16_t)sched_prefs.getUInt("start", 8 * 60);
    schedule_cfg.duration_min = (uint16_t)sched_prefs.getUInt("dur", 120);
    sched_prefs.end();

    if (schedule_cfg.start_minute > 1439) schedule_cfg.start_minute = 0;
    if (schedule_cfg.duration_min == 0) schedule_cfg.duration_min = 1;
    if (schedule_cfg.duration_min > 720) schedule_cfg.duration_min = 720;
}

void save_schedule() {
    sched_prefs.begin("schedule", false);
    sched_prefs.putBool("enabled", schedule_cfg.enabled);
    sched_prefs.putUChar("days", schedule_cfg.days_mask);
    sched_prefs.putUInt("start", schedule_cfg.start_minute);
    sched_prefs.putUInt("dur", schedule_cfg.duration_min);
    sched_prefs.end();
}

void load_timezone() {
    // New dedicated namespace for timezone.
    time_prefs.begin("time", true);
    if (time_prefs.isKey("tz")) {
        tz_offset_min = time_prefs.getInt("tz", 0);
        time_prefs.end();
        return;
    }
    time_prefs.end();

    // Backward compatibility: old versions stored tz in schedule namespace.
    sched_prefs.begin("schedule", true);
    tz_offset_min = sched_prefs.getInt("tz", 0);
    sched_prefs.end();
}

void save_timezone() {
    time_prefs.begin("time", false);
    time_prefs.putInt("tz", tz_offset_min);
    time_prefs.end();
}

bool sync_time(int64_t epoch_utc_sec, int32_t tz_min) {
    bool updated = false;
    if (!ntp_time_valid) {
        utc_base_sec = epoch_utc_sec;
        utc_base_millis = millis();
        utc_synced = true;
        updated = true;
    }

    if (tz_offset_min != tz_min) {
        tz_offset_min = tz_min;
        save_timezone();
        updated = true;
    }

    return updated;
}

void ensure_ntp_sync() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (!ntp_started) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
        ntp_started = true;
        log("NTP started");
    }

    auto now_ms = millis();
    if (now_ms - last_ntp_poll_ms < 5000) return;
    last_ntp_poll_ms = now_ms;

    time_t ntp_now = time(nullptr);
    if (ntp_now > 1700000000) {
        utc_base_sec = (int64_t)ntp_now;
        utc_base_millis = now_ms;
        if (!ntp_time_valid) {
            log("NTP time acquired: %lld", (long long)utc_base_sec);
        }
        utc_synced = true;
        ntp_time_valid = true;
    }
}

int64_t now_utc_sec() {
    if (!utc_synced) return -1;
    return utc_base_sec + (int64_t)((millis() - utc_base_millis) / 1000);
}

int64_t now_local_sec() {
    auto utc = now_utc_sec();
    if (utc < 0) return -1;
    return utc - (int64_t)tz_offset_min * 60;
}

void check_schedule(unsigned long now_ms) {
    if (!schedule_cfg.enabled || !utc_synced) return;
    auto local_sec = now_local_sec();
    if (local_sec < 0) return;
    if (local_sec == last_schedule_checked_sec) return;
    last_schedule_checked_sec = (long)local_sec;

    long sec_of_day = (long)(local_sec % 86400);
    if (sec_of_day < 0) sec_of_day += 86400;
    int day_of_week = (int)(((local_sec / 86400) + 4) % 7);  // 1970-01-01 = Thu(4)
    if (day_of_week < 0) day_of_week += 7;
    long local_day = (long)(local_sec / 86400);
    long minute_of_day = sec_of_day / 60;

    if (((schedule_cfg.days_mask >> day_of_week) & 0x01) == 0) return;
    if (minute_of_day != schedule_cfg.start_minute) return;
    if ((sec_of_day % 60) != 0) return;
    if (last_schedule_local_day == local_day) return;

    last_schedule_local_day = local_day;
    switch_off_time = now_ms + (unsigned long)schedule_cfg.duration_min * 60UL * 1000UL;
    switch_state(true);
    log("Schedule trigger: dow=%d start_min=%u duration_min=%u", day_of_week, schedule_cfg.start_minute, schedule_cfg.duration_min);
    sendStateAndUptime(now_ms);
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
    auto local_sec = now_local_sec();
    ws_msg = R"({"remaining_time":")";
    ws_msg += String((switch_off_time > now) ? (switch_off_time - now) / 1000 : 0);
    ws_msg += R"(","uptime":")";
    ws_msg += String(now / 1000);
    ws_msg += R"(","schedule_enabled":")";
    ws_msg += String(schedule_cfg.enabled ? 1 : 0);
    ws_msg += R"(","schedule_days":")";
    ws_msg += String(schedule_cfg.days_mask);
    ws_msg += R"(","schedule_start":")";
    ws_msg += String(schedule_cfg.start_minute);
    ws_msg += R"(","schedule_duration":")";
    ws_msg += String(schedule_cfg.duration_min);
    ws_msg += R"(","time_synced":")";
    ws_msg += String(utc_synced ? 1 : 0);
    ws_msg += R"(","tz_offset_min":")";
    ws_msg += String(tz_offset_min);
    ws_msg += R"(","local_epoch":")";
    ws_msg += String(local_sec);
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

        if (strncmp(msg, "set_duration:", 13) == 0) {
            sendStateAndUptime(change_state(msg + colon + 1));
            return;
        }

        if (strncmp(msg, "sync_time:", 10) == 0) {
            const char *payload = msg + 10;
            int p1 = find_char(payload, ':');
            if (p1 < 0) {
                log("sync_time parse error: %s", msg);
                return;
            }
            char epoch_buf[24] = {0};
            memcpy(epoch_buf, payload, min((int)sizeof(epoch_buf) - 1, p1));
            int64_t epoch_utc = atoll(epoch_buf);
            int32_t tz_min = atoi(payload + p1 + 1);

            // If NTP is already authoritative and TZ did not change, ignore browser sync.
            if (ntp_time_valid && tz_offset_min == tz_min) {
                log("Browser sync ignored (already up to date)");
                return;
            }

            bool updated = sync_time(epoch_utc, tz_min);
            if (!updated) {
                log("Browser sync ignored (no changes)");
            } else if (ntp_time_valid) {
                log("Timezone updated from browser: tz_offset_min=%ld", (long)tz_min);
            } else {
                log("Browser time sync: epoch=%lld tz_offset_min=%ld", epoch_utc, (long)tz_min);
            }
            if (updated) sendStateAndUptime(millis());
            return;
        }

        if (strncmp(msg, "set_schedule:", 13) == 0) {
            const char *payload = msg + 13;
            int c1 = find_char(payload, ':');
            if (c1 < 0) {
                log("set_schedule parse error: %s", msg);
                return;
            }
            int c2 = find_char(payload + c1 + 1, ':');
            if (c2 < 0) {
                log("set_schedule parse error 2: %s", msg);
                return;
            }
            c2 += c1 + 1;
            int c3 = find_char(payload + c2 + 1, ':');
            if (c3 < 0) {
                log("set_schedule parse error 3: %s", msg);
                return;
            }
            c3 += c2 + 1;

            char enabled_buf[8] = {0};
            char days_buf[8] = {0};
            char start_buf[8] = {0};

            memcpy(enabled_buf, payload, min((int)sizeof(enabled_buf) - 1, c1));
            memcpy(days_buf, payload + c1 + 1, min((int)sizeof(days_buf) - 1, c2 - c1 - 1));
            memcpy(start_buf, payload + c2 + 1, min((int)sizeof(start_buf) - 1, c3 - c2 - 1));

            int enabled = atoi(enabled_buf);
            int days = atoi(days_buf);
            int start_min = atoi(start_buf);
            int duration_min = atoi(payload + c3 + 1);

            schedule_cfg.enabled = enabled != 0;
            schedule_cfg.days_mask = (uint8_t)(days & 0x7F);
            schedule_cfg.start_minute = (uint16_t)max(0, min(1439, start_min));
            schedule_cfg.duration_min = (uint16_t)max(1, min(720, duration_min));
            last_schedule_local_day = -1;
            save_schedule();

            log("Schedule saved: enabled=%d days=%u start=%u duration=%u", schedule_cfg.enabled ? 1 : 0, schedule_cfg.days_mask, schedule_cfg.start_minute, schedule_cfg.duration_min);
            sendStateAndUptime(millis());
            return;
        }

        log("Unsupported command: %s", msg);
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
        if (request->hasHeader("If-None-Match") &&
            request->header("If-None-Match") == String(index_html_gz_etag)) {
            request->send(304);
            return;
        }
        AsyncWebServerResponse *response = request->beginResponse_P(
            200, "text/html", index_html_gz, index_html_gz_len);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=86400");
        response->addHeader("ETag", index_html_gz_etag);
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
    load_schedule();
    load_timezone();

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
    ensure_ntp_sync();
    check_schedule(now);

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
