#pragma once
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include "WiFiConfig.h"

#include <algorithm>
#include <stdarg.h>
#include <vector>

// WiFi lifecycle intent:
// - Try STA first when saved credentials exist.
// - If STA is not connected within 30 sec timeout, start AP for the captive portal.
// - AP should start in open mode so users can join it without a password prompt.
// - While AP is active, STA reconnect attempts should be periodic (every 10 sec or so), not continuous.
// - Retry timing should be driven by retry_interval, not a hard-coded delay.
// - If an SSID refresh is requested from the AP page, keep AP+STA enabled and follow the captive portal scan sequence:
//   1. keep WiFi.mode(WIFI_AP_STA),
//   2. start AP,
//   3. start DNS for captive portal with dnsServer.start(53, "*", WiFi.softAPIP()),
//   4. clear stale STA state with WiFi.disconnect(true),
//   5. wait briefly,
//   6. scan,
//   7. leave AP available for the captive portal.
// - Keep AP up while retrying STA unless a successful STA connection allows AP shutdown.
// - Keep the control flow simple: one state machine for startup/fallback, one scan path for portal refresh.

enum class WiFiState {
	BOOT,
	TRY_STA,
	STA_WAIT,
	STA_FAIL,
	START_AP,
	AP_ACTIVE,
	AP_IDLE,
	AP_STOP
};

class WiFiManager {
	using LogFn = void (*)(const char* format, ...);

	AsyncWebServer& server;
	const char* ap_ssid;
	const char* ap_pass;
	LogFn logger;
	unsigned long retry_interval;
	unsigned long ap_timeout;
	unsigned long last_scan = 0;
	unsigned long ap_start = 0;
	unsigned long state_start = 0;
	unsigned long disconnect_detected_at = 0;
	WiFiState state = WiFiState::BOOT;
	bool ap_mode = false;
	wl_status_t last_wifi_status = WL_IDLE_STATUS;
	Preferences prefs;
	String ssid, pass;
	void (*start_web_server)();

	struct ScanResult {
		std::vector<String> ssids;
		std::vector<int> rssis;
		bool success = false;
	} scanCache;

	static const char* stateName(WiFiState value) {
		switch (value) {
			case WiFiState::BOOT:
				return "BOOT";
			case WiFiState::TRY_STA:
				return "TRY_STA";
			case WiFiState::STA_WAIT:
				return "STA_WAIT";
			case WiFiState::STA_FAIL:
				return "STA_FAIL";
			case WiFiState::START_AP:
				return "START_AP";
			case WiFiState::AP_ACTIVE:
				return "AP_ACTIVE";
			case WiFiState::AP_IDLE:
				return "AP_IDLE";
			case WiFiState::AP_STOP:
				return "AP_STOP";
		}
		return "UNKNOWN";
	}

	static const char* wifiStatusName(wl_status_t value) {
		switch (value) {
			case WL_NO_SHIELD:
				return "WL_NO_SHIELD";
			case WL_IDLE_STATUS:
				return "WL_IDLE_STATUS";
			case WL_NO_SSID_AVAIL:
				return "WL_NO_SSID_AVAIL";
			case WL_SCAN_COMPLETED:
				return "WL_SCAN_COMPLETED";
			case WL_CONNECTED:
				return "WL_CONNECTED";
			case WL_CONNECT_FAILED:
				return "WL_CONNECT_FAILED";
			case WL_CONNECTION_LOST:
				return "WL_CONNECTION_LOST";
			case WL_DISCONNECTED:
				return "WL_DISCONNECTED";
		}
		return "WL_UNKNOWN";
	}

	void logMessage(const char* format, ...) {
		char buffer[256];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		if (logger) {
			logger("[WiFiManager] %s", buffer);
			return;
		}
		Serial.println(String("[WiFiManager] ") + buffer);
	}

	void transitionTo(WiFiState nextState, unsigned long now,
					  const char* reason = nullptr) {
		if (state == nextState) return;
		if (reason && reason[0] != '\0') {
			logMessage("State %s -> %s (%s)", stateName(state),
					   stateName(nextState), reason);
		} else {
			logMessage("State %s -> %s", stateName(state), stateName(nextState));
		}
		state = nextState;
		state_start = now;
	}

	void monitorConnection(unsigned long now) {
		wl_status_t current_status = WiFi.status();
		if (current_status != last_wifi_status) {
			logMessage("WiFi status %s -> %s", wifiStatusName(last_wifi_status),
					   wifiStatusName(current_status));
			last_wifi_status = current_status;
		}

		if (state == WiFiState::BOOT && current_status != WL_CONNECTED) {
			if (disconnect_detected_at == 0) {
				disconnect_detected_at = now;
				logMessage("STA disconnected while running. Starting recovery.");
			}
			if (ap_mode) {
				if (now - disconnect_detected_at >= retry_interval) {
					stopAP();
					transitionTo(WiFiState::TRY_STA, now, "periodic retry while disconnected");
					disconnect_detected_at = 0;
				}
			} else {
				transitionTo(WiFiState::START_AP, now, "runtime STA disconnect");
			}
		} else if (current_status == WL_CONNECTED) {
			disconnect_detected_at = 0;
		}
	}

   public:
	WiFiManager(AsyncWebServer& srv,
				 const char* apSSID = WiFiConfig::kApSsid,
				 const char* apPASS = WiFiConfig::kApPassword,
				 LogFn log_fn = nullptr,
				 unsigned long retry_ms = 60000,
				 unsigned long ap_timeout_ms = 60000)
		: server(srv),
		  start_web_server(start_web_server),
		  ap_ssid(apSSID),
		  ap_pass(apPASS),
		  logger(log_fn),
		  retry_interval(retry_ms),
		  ap_timeout(ap_timeout_ms) {}

	void begin(void (*start_web_server)() = nullptr) {
		this->start_web_server = start_web_server;
		prefs.begin("wifi", false);
		ssid = prefs.getString("ssid", "");
		pass = prefs.getString("pass", "");
		prefs.end();

		transitionTo(WiFiState::TRY_STA, millis(), "initial startup");
		if (ssid.length()) {
			WiFi.mode(WIFI_STA);
			WiFi.begin(ssid.c_str(), pass.c_str());
		} else {
			transitionTo(WiFiState::START_AP, millis(), "no saved credentials");
		}
	}

	void setLogger(LogFn log_fn) { logger = log_fn; }

	void registerResetWiFi() {
		server.on("/clear", HTTP_GET, [](AsyncWebServerRequest* req) {
			if (req->hasArg("confirm")) {
				Preferences prefs;
				prefs.begin("wifi", false);
				prefs.clear();
				prefs.end();
				req->send(200, "text/html", "<h1>Preferences cleared.</h1>"
					"<h2>Device will restart in 1.5 seconds.</h2>");
				delay(1500);
				ESP.restart();				
				return;
			}

			String html = R"rawliteral(
				<!DOCTYPE html>
				<html>
				<head>
					<meta name="viewport" content="width=device-width, initial-scale=1">
					<title>Clear Preferences</title>
					<style>
						body { font-family: sans-serif; padding: 2em; max-width: 600px; margin: auto; }
						h2 { color: #b00; }
						a.button {
							display: inline-block; padding: 0.5em 1em; margin: 1em 0;
							background: #b00; color: #fff; text-decoration: none; border-radius: 4px;
						}
					</style>
				</head>
				<body>
					<h2>Are you sure you want to clear saved WiFi preferences?</h2>
					<p>This will delete stored SSID and password from memory.</p>
					<a href="/clear?confirm=1" class="button">Yes, clear preferences</a>
					<a href="/" class="button" style="background:#555;">Cancel</a>
				</body>
				</html>
			)rawliteral";

			req->send(200, "text/html", html);
		});
	}

	void handle() {
		unsigned long now = millis();
		monitorConnection(now);

		switch (state) {
			case WiFiState::TRY_STA:
				if (ssid.length()) {
					logMessage("Attempting STA connection to SSID '%s'", ssid.c_str());
					WiFi.mode(WIFI_STA);
					WiFi.begin(ssid.c_str(), pass.c_str());
					transitionTo(WiFiState::STA_WAIT, now, "STA connect requested");
				} else {
					logMessage("No credentials. Skipping STA attempt.");
					transitionTo(WiFiState::START_AP, now, "STA credentials unavailable");
				}
				break;

			case WiFiState::STA_WAIT:
				if (WiFi.status() == WL_CONNECTED) {
					last_wifi_status = WL_CONNECTED;
					disconnect_detected_at = 0;
					logMessage("STA connected. IP: %s", WiFi.localIP().toString().c_str());
					if (start_web_server) start_web_server();
					transitionTo(WiFiState::BOOT, now, "STA connected");
				} else if (now - state_start > 10000) {
					logMessage("STA connection timed out after %lu ms", now - state_start);
					transitionTo(WiFiState::START_AP, now, "STA timeout");
				}
				break;

			case WiFiState::START_AP:
				startAP();
				disconnect_detected_at = now;
				transitionTo(WiFiState::AP_ACTIVE, now, "AP started");
				ap_start = now;
				last_scan = now;
				break;

			case WiFiState::AP_ACTIVE:
				if ((now - last_scan > 5000 && !scanCache.success) || now - last_scan > 180*1000) {
					scanCache = scanWiFi();
					last_scan = now;
				}
				if (now - ap_start > ap_timeout &&
					WiFi.softAPgetStationNum() == 0) {
					if (ssid.length()) {
						logMessage("AP timeout expired with no clients. Retrying STA.");
						stopAP();
						transitionTo(WiFiState::TRY_STA, now, "AP timeout retry");
						disconnect_detected_at = 0;
					} else {
						logMessage("No credentials. Holding AP active.");
						ap_start = now;	 // extend AP lease
						// optionally re-scan
					}
				}
				break;

			default:
				break;
		}
	}

   private:
	ScanResult scanWiFi() {
		ScanResult result;
		int n = WiFi.scanNetworks();
		if (n <= 0) return scanCache;  // return previous result if scan failed
		logMessage("scanWiFi started");
		std::vector<int> indices(n);
		for (int i = 0; i < n; ++i) indices[i] = i;
		std::sort(indices.begin(), indices.end(),
				  [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
		for (int i : indices) {
			String s = WiFi.SSID(i);
			logMessage("Found SSID '%s' (%d dBm)", s.c_str(), WiFi.RSSI(i));
			if (s.length() == 0) continue;
			result.ssids.push_back(s);
			result.rssis.push_back(WiFi.RSSI(i));
		}
		result.success = true;
		return result;
	}

	void startAP() {
		logMessage("Starting AP");
		WiFi.mode(WIFI_AP);
		IPAddress ap_ip;
		if (!ap_ip.fromString(WiFiConfig::kApIp)) {
			logMessage("Invalid kApIp config, using 192.168.4.1");
			ap_ip = IPAddress(192, 168, 4, 1);
		}
		IPAddress ap_gateway = ap_ip;
		IPAddress ap_subnet;
		if (!ap_subnet.fromString(WiFiConfig::kApSubnet)) {
			logMessage("Invalid kApSubnet config, using 255.255.255.0");
			ap_subnet = IPAddress(255, 255, 255, 0);
		}
		if (!WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet)) {
			logMessage("softAPConfig failed, using defaults");
		}
		WiFi.softAP(ap_ssid, ap_pass);
		logMessage("Captive portal: http://%s", WiFiConfig::kApIp);
		ap_mode = true;
		ap_start = millis();

		server.on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/scan", HTTP_GET, [&](AsyncWebServerRequest* req) {
			req->send(200, "text/plain", renderWiFiOptions());
		});
		registerResetWiFi();
		server.on("/restart", HTTP_GET, [](AsyncWebServerRequest* req) {
			req->send(200, "text/plain", "Restarting...");
			delay(500);
			ESP.restart();
		});


		server.on("/config", HTTP_POST, [&](AsyncWebServerRequest* req) {
			if (!req->hasParam("ssid", true) || !req->hasParam("pass", true)) {
				req->send(400, "text/plain", "Missing SSID or password");
				return;
			}

			String ssid = req->getParam("ssid", true)->value();
			String pass = req->getParam("pass", true)->value();

			prefs.begin("wifi", false);
			prefs.putString("ssid", ssid);
			prefs.putString("pass", pass);
			prefs.end();

			req->send(200, "text/plain", "Credentials saved. Rebooting...");
			delay(500);
			ESP.restart();
		});

		// Captive portal probes (inline lambdas instead of serveCaptive)
		server.on("/generate_204", HTTP_GET,
				  [this](AsyncWebServerRequest* req) {
					  req->send(200, "text/html", renderConfigPage());
				  });
		server.on("/hotspot-detect.html", HTTP_GET,
				  [this](AsyncWebServerRequest* req) {
					  req->send(200, "text/html", renderConfigPage());
				  });
		server.on("/connecttest.txt", HTTP_GET,
				  [this](AsyncWebServerRequest* req) {
					  req->send(200, "text/html", renderConfigPage());
				  });
		server.on("/ncsi.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});

		server.begin();
		logMessage("AP web server started");
	}

	void stopAP() {
		logMessage("Stopping AP");
		server.reset();	 // Clear handlers
		WiFi.softAPdisconnect(true);
		ap_mode = false;
	}

	String renderWiFiOptions() {
		if (!scanCache.success) return "<option disabled>Scan failed</option>";
		String options;
		for (size_t i = 0; i < scanCache.ssids.size(); ++i) {
			options += "<option value='" + scanCache.ssids[i] + "'>";
			options += scanCache.ssids[i] + " (" + String(scanCache.rssis[i]) +
					   " dBm)</option>";
		}
		return options;
	}

String renderConfigPage() {
    return R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>WiFi Setup</title>
        <style>
            body {
                font-family: sans-serif;
                margin: 0;
                padding: 1.5em;
                max-width: 600px;
                margin-left: auto;
                margin-right: auto;
                background-color: #f8f8f8;
            }
            h2 {
                margin-top: 0;
                font-size: 1.5em;
                text-align: center;
            }
            form {
                background: #fff;
                padding: 1em;
                border-radius: 8px;
                box-shadow: 0 0 10px rgba(0,0,0,0.05);
            }
            label {
                display: block;
                margin-top: 1em;
                font-weight: bold;
            }
            select, input[type="password"], input[type="submit"] {
                width: 100%;
                padding: 0.7em;
                margin-top: 0.5em;
                font-size: 1em;
                border-radius: 4px;
                border: 1px solid #ccc;
                box-sizing: border-box;
            }
            input[type="submit"] {
                background-color: #007bff;
                color: white;
                border: none;
                cursor: pointer;
            }
            input[type="submit"]:hover {
                background-color: #0056b3;
            }
            @media (max-width: 400px) {
                body { padding: 1em; }
            }
        </style>
    </head>
    <body>
        <h2>Configure WiFi</h2>
        <form method="POST" action="/config">
            <label for="ssidList">SSID:</label>
            <select name="ssid" id="ssidList">
                <option disabled>Loading...</option>
            </select>

            <label for="pass">Password:</label>
            <input type="password" name="pass" id="pass" autocomplete="new-password">

            <input type="submit" value="Save & Reboot">
        </form>

        <script>
            window.addEventListener('load', () => {
                setTimeout(() => {
                    fetch('/scan')
                        .then(response => response.text())
                        .then(html => {
                            const select = document.getElementById('ssidList');
                            if (select) {
                                select.innerHTML = html;
                            }
                        })
                        .catch(err => {
                            console.error('Scan fetch failed:', err);
                        });
                }, 1000);
            });
        </script>
    </body>
    </html>
    )rawliteral";
}
};
