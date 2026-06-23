#pragma once
#include <time.h>
#include <WiFi.h>
#include <Preferences.h>

static int32_t tz_offset_min = 0;
static int64_t utc_base_sec = 0;
static uint32_t utc_base_millis = 0;
static bool utc_synced = false;
static bool ntp_time_valid = false;
static bool ntp_started = false;
static unsigned long last_ntp_poll_ms = 0;
static Preferences time_prefs;
// extern AsyncWebSocket ws_control;

void app_log(const char* format, ...);
// -------------------- Time --------------------

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
    return;
    if (!ntp_started) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
        ntp_started = true;
        app_log("NTP started");
    }
    auto now_ms = millis();
    if (now_ms - last_ntp_poll_ms < 5000) return;
    last_ntp_poll_ms = now_ms;
    time_t ntp_now = time(nullptr);
    if (ntp_now > 1700000000) {
        utc_base_sec = (int64_t)ntp_now;
        utc_base_millis = now_ms;
        if (!ntp_time_valid) app_log("NTP time acquired: %lld", (long long)utc_base_sec);
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

// -------------------- Logging --------------------

void app_log(const char* format, ...) {
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
    // if (ws_control.count()) ws_control.textAll(buffer);
}
