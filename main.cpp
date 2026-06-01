#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include "WiFiManager.h"
#include "index_html_gz.h"

#define MAX_DURATION_ON    5 * 3600 * 1000  // 5 hours
#define PIN_BUTTON         14
#define PIN_RELAY          27
#define PIN_REFILL_RELAY   25
#define PIN_BLUE_LED       26

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

static int32_t tz_offset_min = 0;
static int64_t utc_base_sec = 0;
static uint32_t utc_base_millis = 0;
static bool utc_synced = false;
static bool ntp_time_valid = false;
static bool ntp_started = false;
static unsigned long last_ntp_poll_ms = 0;
static Preferences time_prefs;

// -------------------- Channel --------------------
// Groups all per-relay state so filter and refill share one code path.
struct Channel {
    const char *prefs_ns;   // NVS namespace
    const char *label;      // for log messages
    uint8_t pin;            // relay GPIO
    AsyncWebSocket *ws;

    ScheduleConfig cfg;
    unsigned long switch_off_time = 0;
    long last_schedule_local_day = -1;
    long last_schedule_checked_sec = -1;

    Channel(const char *ns, const char *lbl, uint8_t p, AsyncWebSocket *sock)
        : prefs_ns(ns), label(lbl), pin(p), ws(sock) {}
};

static AsyncWebSocket ws_filter("/ws");
static AsyncWebSocket ws_refill("/ws_refill");

static Channel filter_ch{ "schedule", "Filter", PIN_RELAY,        &ws_filter };
static Channel refill_ch{ "refill",   "Refill", PIN_REFILL_RELAY, &ws_refill };

// Forward declarations
void sendChannelState(Channel &ch, unsigned long now);
int64_t now_utc_sec();
int find_char(const char *str, char c);
bool schedules_overlap(const ScheduleConfig &a, const ScheduleConfig &b);
void set_channel_output(Channel &ch, bool on);

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
    if (ws_filter.count()) ws_filter.textAll(buffer);
}

// -------------------- IO Setup --------------------
void setup_io() {
    pinMode(PIN_RELAY,        OUTPUT); digitalWrite(PIN_RELAY,        LOW);
    pinMode(PIN_REFILL_RELAY, OUTPUT); digitalWrite(PIN_REFILL_RELAY, LOW);
    pinMode(PIN_BLUE_LED,     OUTPUT); digitalWrite(PIN_BLUE_LED,     LOW);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

void switch_state(bool on) {
    digitalWrite(PIN_RELAY, on ? HIGH : LOW);
    log("Switch state changed: %s", on ? "ON" : "OFF");
}

void set_channel_output(Channel &ch, bool on) {
    if (&ch == &filter_ch) {
        switch_state(on);
        return;
    }
    digitalWrite(ch.pin, on ? HIGH : LOW);
    log("%s state changed: %s", ch.label, on ? "ON" : "OFF");
}

bool minute_in_wrapped_range(int minute, int start, int end_exclusive) {
    if (start < end_exclusive) return minute >= start && minute < end_exclusive;
    return minute >= start || minute < end_exclusive;
}

bool schedule_active_at_minute(const ScheduleConfig &cfg, int week_minute) {
    if (!cfg.enabled || cfg.days_mask == 0) return false;
    for (int d = 0; d < 7; d++) {
        if (((cfg.days_mask >> d) & 0x01) == 0) continue;
        int start = d * 1440 + (int)cfg.start_minute;
        int end_exclusive = start + (int)cfg.duration_min;
        int m = week_minute;
        if (end_exclusive <= 10080) {
            if (m >= start && m < end_exclusive) return true;
        } else {
            int wrapped_end = end_exclusive - 10080;
            if (minute_in_wrapped_range(m, start, wrapped_end)) return true;
        }
    }
    return false;
}

bool schedules_overlap(const ScheduleConfig &a, const ScheduleConfig &b) {
    if (!a.enabled || !b.enabled) return false;
    for (int m = 0; m < 10080; m++) {
        if (schedule_active_at_minute(a, m) && schedule_active_at_minute(b, m)) return true;
    }
    return false;
}

bool schedules_overlap_ignoring_enabled(const ScheduleConfig &a, const ScheduleConfig &b) {
    ScheduleConfig aa = a;
    ScheduleConfig bb = b;
    aa.enabled = true;
    bb.enabled = true;
    return schedules_overlap(aa, bb);
}

ScheduleConfig load_channel_schedule_snapshot(const char *prefs_ns) {
    ScheduleConfig cfg;
    Preferences p;
    p.begin(prefs_ns, true);
    cfg.enabled      = p.getBool("enabled", false);
    cfg.days_mask    = (uint8_t)p.getUChar("days", 0x00);
    cfg.start_minute = (uint16_t)p.getUInt("start", 8 * 60);
    cfg.duration_min = (uint16_t)p.getUInt("dur", 120);
    p.end();
    if (cfg.start_minute > 1439) cfg.start_minute = 0;
    if (cfg.duration_min == 0)   cfg.duration_min = 1;
    if (cfg.duration_min > 720)  cfg.duration_min = 720;
    return cfg;
}

// -------------------- Schedule persistence --------------------
void load_channel_schedule(Channel &ch) {
    Preferences p;
    p.begin(ch.prefs_ns, false);
    ch.cfg.enabled      = p.getBool("enabled", false);
    ch.cfg.days_mask    = (uint8_t)p.getUChar("days", 0x00);
    ch.cfg.start_minute = (uint16_t)p.getUInt("start", 8 * 60);
    ch.cfg.duration_min = (uint16_t)p.getUInt("dur", 120);
    p.end();
    if (ch.cfg.start_minute > 1439) ch.cfg.start_minute = 0;
    if (ch.cfg.duration_min == 0)   ch.cfg.duration_min = 1;
    if (ch.cfg.duration_min > 720)  ch.cfg.duration_min = 720;
}

void save_channel_schedule(Channel &ch) {
    Preferences p;
    p.begin(ch.prefs_ns, false);
    p.putBool("enabled", ch.cfg.enabled);
    p.putUChar("days",   ch.cfg.days_mask);
    p.putUInt("start",   ch.cfg.start_minute);
    p.putUInt("dur",     ch.cfg.duration_min);
    p.end();
}

void load_timezone() {
    // Dedicated namespace for timezone.
    time_prefs.begin("time", true);
    if (time_prefs.isKey("tz")) {
        tz_offset_min = time_prefs.getInt("tz", 0);
        time_prefs.end();
        return;
    }
    time_prefs.end();

    // Backward compatibility: old versions stored tz in schedule namespace.
    Preferences p;
    p.begin("schedule", true);
    tz_offset_min = p.getInt("tz", 0);
    p.end();
}

void save_timezone() {
    time_prefs.begin("time", false);
    time_prefs.putInt("tz", tz_offset_min);
    time_prefs.end();
}

// -------------------- Time --------------------
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
        if (!ntp_time_valid) log("NTP time acquired: %lld", (long long)utc_base_sec);
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

// -------------------- Channel helpers --------------------
void sendChannelState(Channel &ch, unsigned long now) {
    if (!ch.ws->count()) return;
    auto local_sec = now_local_sec();
    ws_msg = R"({"remaining_time":")";
    ws_msg += String((ch.switch_off_time > now) ? (ch.switch_off_time - now) / 1000 : 0);
    ws_msg += R"(","uptime":")";
    ws_msg += String(now / 1000);
    ws_msg += R"(","schedule_enabled":")";
    ws_msg += String(ch.cfg.enabled ? 1 : 0);
    ws_msg += R"(","schedule_days":")";
    ws_msg += String(ch.cfg.days_mask);
    ws_msg += R"(","schedule_start":")";
    ws_msg += String(ch.cfg.start_minute);
    ws_msg += R"(","schedule_duration":")";
    ws_msg += String(ch.cfg.duration_min);
    ws_msg += R"(","time_synced":")";
    ws_msg += String(utc_synced ? 1 : 0);
    ws_msg += R"(","tz_offset_min":")";
    ws_msg += String(tz_offset_min);
    ws_msg += R"(","local_epoch":")";
    ws_msg += String(local_sec);
    ws_msg += R"("})";
    ch.ws->textAll(ws_msg);
}

int find_char(const char *str, char c) {
    for (int i = 0; str[i]; i++) if (str[i] == c) return i;
    return -1;
}

void handleChannelMessage(Channel &ch, char *msg) {
    int colon = find_char(msg, ':');
    if (colon < 0) { log("%s unsupported command: %s", ch.label, msg); return; }
    Channel *other = (&ch == &filter_ch) ? &refill_ch : &filter_ch;

    if (strncmp(msg, "set_duration:", 13) == 0) {
        auto now = millis();
        const char *p = msg + colon + 1;
        ch.switch_off_time = 0;
        set_channel_output(ch, false);
        if (p[0] >= '0' && p[0] <= '9') {
            long dur = atol(p);
            ch.switch_off_time = now + dur * 1000;
            if (dur > 0 && other->switch_off_time > now) {
                other->switch_off_time = 0;
                set_channel_output(*other, false);
                sendChannelState(*other, now);
                log("%s turned off because %s was turned on", other->label, ch.label);
            }
            set_channel_output(ch, dur > 0);
            log("%s duration_on: %ld", ch.label, dur);
        }
        sendChannelState(ch, now);
        return;
    }

    if (strncmp(msg, "sync_time:", 10) == 0) {
        const char *payload = msg + 10;
        int p1 = find_char(payload, ':');
        if (p1 < 0) return;
        char epoch_buf[24] = {0};
        memcpy(epoch_buf, payload, min((int)sizeof(epoch_buf) - 1, p1));
        int64_t epoch_utc = atoll(epoch_buf);
        int32_t tz_min = atoi(payload + p1 + 1);
        if (ntp_time_valid && tz_offset_min == tz_min) return;
        bool updated = sync_time(epoch_utc, tz_min);
        if (updated) {
            // Broadcast updated time to both channels.
            auto now = millis();
            sendChannelState(filter_ch, now);
            sendChannelState(refill_ch, now);
        }
        return;
    }

    if (strncmp(msg, "set_schedule:", 13) == 0) {
        const char *payload = msg + 13;
        int c1 = find_char(payload, ':');           if (c1 < 0) return;
        int c2 = find_char(payload + c1 + 1, ':'); if (c2 < 0) return; c2 += c1 + 1;
        int c3 = find_char(payload + c2 + 1, ':'); if (c3 < 0) return; c3 += c2 + 1;

        char enabled_buf[8] = {0}, days_buf[8] = {0}, start_buf[8] = {0};
        memcpy(enabled_buf, payload,          min((int)sizeof(enabled_buf) - 1, c1));
        memcpy(days_buf,    payload + c1 + 1, min((int)sizeof(days_buf)    - 1, c2 - c1 - 1));
        memcpy(start_buf,   payload + c2 + 1, min((int)sizeof(start_buf)   - 1, c3 - c2 - 1));

        ScheduleConfig proposed;
        proposed.enabled      = atoi(enabled_buf) != 0;
        proposed.days_mask    = (uint8_t)(atoi(days_buf) & 0x7F);
        proposed.start_minute = (uint16_t)max(0, min(1439, atoi(start_buf)));
        proposed.duration_min = (uint16_t)max(1, min(720,  atoi(payload + c3 + 1)));

        ScheduleConfig other_persisted = load_channel_schedule_snapshot(other->prefs_ns);
        if (schedules_overlap_ignoring_enabled(proposed, other_persisted)) {
            log("%s schedule rejected: overlaps with %s schedule", ch.label, other->label);
            ch.ws->textAll("notice:Schedule overlaps with other channel schedule");
            sendChannelState(ch, millis());
            return;
        }

        ch.cfg = proposed;
        ch.last_schedule_local_day = -1;
        save_channel_schedule(ch);
        log("%s schedule saved: enabled=%d days=%u start=%u duration=%u",
            ch.label, ch.cfg.enabled ? 1 : 0, ch.cfg.days_mask, ch.cfg.start_minute, ch.cfg.duration_min);
        ch.ws->textAll("notice:Schedule saved");
        sendChannelState(ch, millis());
        return;
    }

    log("%s unsupported command: %s", ch.label, msg);
}

void check_channel_schedule(Channel &ch, unsigned long now_ms) {
    if (!ch.cfg.enabled || !utc_synced) return;
    auto local_sec = now_local_sec();
    if (local_sec < 0) return;
    if (local_sec == ch.last_schedule_checked_sec) return;
    ch.last_schedule_checked_sec = (long)local_sec;

    long sec_of_day = (long)(local_sec % 86400);
    if (sec_of_day < 0) sec_of_day += 86400;
    int day_of_week = (int)(((local_sec / 86400) + 4) % 7);  // 1970-01-01 = Thu(4)
    if (day_of_week < 0) day_of_week += 7;
    long local_day    = (long)(local_sec / 86400);
    long minute_of_day = sec_of_day / 60;

    if (((ch.cfg.days_mask >> day_of_week) & 0x01) == 0) return;
    if (minute_of_day != ch.cfg.start_minute) return;
    if ((sec_of_day % 60) != 0) return;
    if (ch.last_schedule_local_day == local_day) return;

    Channel *other = (&ch == &filter_ch) ? &refill_ch : &filter_ch;
    if (other->switch_off_time > now_ms) {
        other->switch_off_time = 0;
        set_channel_output(*other, false);
        sendChannelState(*other, now_ms);
        log("%s turned off because %s schedule started", other->label, ch.label);
    }

    ch.last_schedule_local_day = local_day;
    ch.switch_off_time = now_ms + (unsigned long)ch.cfg.duration_min * 60UL * 1000UL;
    set_channel_output(ch, true);
    log("%s schedule trigger: dow=%d start_min=%u duration_min=%u",
        ch.label, day_of_week, ch.cfg.start_minute, ch.cfg.duration_min);
    sendChannelState(ch, now_ms);
}

// -------------------- WebSocket events --------------------
// Each WS needs its own event handler bound to the right Channel.
// Lambda captures are not allowed for AsyncWebSocket callbacks, so we use
// two thin trampolines.
void handleWebSocketMessage(Channel &ch, void *arg, char *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        char msg[128];
        len = min(len, sizeof(msg) - 1);
        memcpy(msg, data, len);
        msg[len] = 0;
        log("WS[%s]: '%s'", ch.label, msg);
        handleChannelMessage(ch, msg);
    } else {
        log("WS[%s] unsupported frame: final:%d index:%d len:%d opcode:%d",
            ch.label, info->final, info->index, info->len, info->opcode);
    }
}

void onFilterEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            sendChannelState(filter_ch, millis());
            log("%s WS #%u connected from %s", filter_ch.label, client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            log("%s WS #%u disconnected", filter_ch.label, client->id());
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(filter_ch, arg, (char *)data, len);
            break;
        default: break;
    }
}

void onRefillEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            sendChannelState(refill_ch, millis());
            log("%s WS #%u connected from %s", refill_ch.label, client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            log("%s WS #%u disconnected", refill_ch.label, client->id());
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(refill_ch, arg, (char *)data, len);
            break;
        default: break;
    }
}

// -------------------- Web Server --------------------
unsigned long change_state(const char *data) {
    auto now = millis();
    if (data[0] == 'f') return now;
    filter_ch.switch_off_time = 0;
    set_channel_output(filter_ch, false);
    if (data[0] < '0' || data[0] > '9') {
        log("Invalid URL: '%s'", data);
        return now;
    }
    long duration_on = atol(data);
    filter_ch.switch_off_time = now + duration_on * 1000;
    if (duration_on > 0 && refill_ch.switch_off_time > now) {
        refill_ch.switch_off_time = 0;
        set_channel_output(refill_ch, false);
        sendChannelState(refill_ch, now);
        log("%s turned off because %s was turned on", refill_ch.label, filter_ch.label);
    }
    log("duration_on: %ld, switch_off_time: %ld", duration_on, filter_ch.switch_off_time - now);
    set_channel_output(filter_ch, duration_on > 0);
    return now;
}

void handleNotFound(AsyncWebServerRequest *request) {
    const char *uri = request->url().c_str();
    log("uri-raw: %s", uri);
    while (*uri == '/') uri++;
    if (!isdigit(uri[0])) {
        request->send(404, "text/plain", "Not found");
        return;
    }
    sendChannelState(filter_ch, change_state(uri));
    request->send(200, "text/plain", "OK");
}

void serve_index(AsyncWebServerRequest *request, const char *cache_control) {
    if (request->hasHeader("If-None-Match") &&
        request->header("If-None-Match") == String(index_html_gz_etag)) {
        request->send(304);
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse_P(
        200, "text/html", index_html_gz, index_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", cache_control);
    response->addHeader("ETag", index_html_gz_etag);
    request->send(response);
}

void start_web_server() {
    server.reset();
    // /refill must not be cached so the browser sees its pathname and picks the right WS.
    server.on("/refill", HTTP_GET, [](AsyncWebServerRequest *r) { serve_index(r, "no-store"); });
    server.on("/refill/", HTTP_GET, [](AsyncWebServerRequest *r) { serve_index(r, "no-store"); });
    server.on("/",       HTTP_GET, [](AsyncWebServerRequest *r) { serve_index(r, "max-age=86400"); });
    WiFiManager.registerResetWiFi();
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });
    server.onNotFound(handleNotFound);
    server.addHandler(&ws_filter);
    server.addHandler(&ws_refill);
    server.begin();
    log("HTTP server started");

    ws_filter.onEvent(onFilterEvent);
    ws_refill.onEvent(onRefillEvent);

    ArduinoOTA.onStart([]() {
        switch_state(false);
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
    load_channel_schedule(filter_ch);
    load_channel_schedule(refill_ch);
    load_timezone();
    WiFiManager.begin(start_web_server);
}

void loop(void) {
    delay(2);
    c++;
    if (c % 100 == 0) {
        ArduinoOTA.handle();
        ws_filter.cleanupClients();
        ws_refill.cleanupClients();
        WiFiManager.handle();
    }

    auto now = millis();
    ensure_ntp_sync();
    check_channel_schedule(filter_ch, now);
    check_channel_schedule(refill_ch, now);

    bool button = digitalRead(PIN_BUTTON);
    if (button == LOW && last_button_state == HIGH && now - last_button_time > 200) {
        last_button_time = now;
        bool is_on = filter_ch.switch_off_time > 0;
        filter_ch.switch_off_time = is_on ? 0 : now + MAX_DURATION_ON;
        if (!is_on && refill_ch.switch_off_time > now) {
            refill_ch.switch_off_time = 0;
            set_channel_output(refill_ch, false);
            sendChannelState(refill_ch, now);
            log("%s turned off because %s was turned on", refill_ch.label, filter_ch.label);
        }
        set_channel_output(filter_ch, !is_on);
        sendChannelState(filter_ch, now);
        log("Button pressed — toggled switch");
    }
    last_button_state = button;

    if (filter_ch.switch_off_time && now > filter_ch.switch_off_time) {
        filter_ch.switch_off_time = 0;
        set_channel_output(filter_ch, false);
        sendChannelState(filter_ch, now);
        log("Filter switched off due to timeout");
    }

    if (refill_ch.switch_off_time && now > refill_ch.switch_off_time) {
        refill_ch.switch_off_time = 0;
        set_channel_output(refill_ch, false);
        sendChannelState(refill_ch, now);
        log("Refill switched off due to timeout");
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
