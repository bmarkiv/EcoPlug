#pragma once

#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <stdarg.h>
#include <vector>

#include "WiFiConfig.h"

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

	struct ScanResult {
		std::vector<String> ssids;
		std::vector<String> bssids;
		std::vector<int> rssis;
		bool success = false;
	};

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
	unsigned long last_sta_attempt = 0;
	unsigned long scan_block_until = 0;
	WiFiState state = WiFiState::BOOT;
	bool ap_mode = false;
	bool sta_attempt_in_progress = false;
	bool scan_requested = false;
	wl_status_t last_wifi_status = WL_IDLE_STATUS;
	Preferences prefs;
	String ssid;
	String pass;
	void (*start_web_server)() = nullptr;
	ScanResult scanCache;

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

	static const char* scanStatusName(int value) {
		switch (value) {
			case WIFI_SCAN_RUNNING:
				return "WIFI_SCAN_RUNNING";
			case WIFI_SCAN_FAILED:
				return "WIFI_SCAN_FAILED";
		}
		if (value == 0) return "WIFI_SCAN_NO_RESULTS";
		return "WIFI_SCAN_RESULT_COUNT";
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

	void transitionTo(WiFiState nextState, unsigned long now, const char* reason = nullptr) {
		if (state == nextState) return;
		if (reason && reason[0] != '\0') {
			logMessage("State %s -> %s (%s)", stateName(state), stateName(nextState), reason);
		} else {
			logMessage("State %s -> %s", stateName(state), stateName(nextState));
		}
		state = nextState;
		state_start = now;
	}

	void monitorConnection(unsigned long now) {
		wl_status_t current_status = WiFi.status();
		if (current_status != last_wifi_status) {
			logMessage("WiFi status %s -> %s", wifiStatusName(last_wifi_status), wifiStatusName(current_status));
			last_wifi_status = current_status;
		}

		if (state == WiFiState::BOOT && current_status != WL_CONNECTED) {
			if (disconnect_detected_at == 0) {
				disconnect_detected_at = now;
				logMessage("STA disconnected while running. Starting recovery.");
			}
			if (ap_mode) {
				if (!sta_attempt_in_progress && now - last_sta_attempt >= retry_interval) {
					transitionTo(WiFiState::TRY_STA, now, "periodic retry while disconnected");
					disconnect_detected_at = 0;
				}
			} else {
				transitionTo(WiFiState::START_AP, now, "runtime STA disconnect");
			}
		} else if (current_status == WL_CONNECTED) {
			disconnect_detected_at = 0;
			sta_attempt_in_progress = false;
		}
	}

	bool shouldRefreshScan(unsigned long now) const {
		if (sta_attempt_in_progress || now < scan_block_until) return false;
		if (scan_requested) return true;
		if (!ap_mode) return false;
		if (!scanCache.success) return now - last_scan > 5000;
		return now - last_scan >= 120000;
	}

	void beginStaAttempt(unsigned long now, const char* reason) {
		if (!ssid.length()) {
			logMessage("No credentials. Skipping STA attempt.");
			transitionTo(WiFiState::START_AP, now, "STA credentials unavailable");
			return;
		}
		last_sta_attempt = now;
		sta_attempt_in_progress = true;
		scan_block_until = now + 15000;
		logMessage("Attempting STA connection to SSID '%s' (%s)", ssid.c_str(), reason);
		WiFi.mode(ap_mode ? WIFI_AP_STA : WIFI_STA);
		WiFi.begin(ssid.c_str(), pass.c_str());
		transitionTo(WiFiState::STA_WAIT, now, reason);
	}

	void prepareScan(unsigned long now) {
		if (WiFi.status() == WL_CONNECTED && !ap_mode) {
			logMessage("Preparing SSID refresh while STA connected");
			WiFi.mode(WIFI_STA);
			scan_block_until = now + 2000;
			return;
		}
		logMessage("Preparing SSID refresh: clearing stale STA state before scan");
		WiFi.mode(WIFI_AP_STA);
		WiFi.disconnect(false, false);
		sta_attempt_in_progress = false;
		last_wifi_status = WiFi.status();
		last_sta_attempt = now;
		scan_block_until = now + 2000;
	}

	ScanResult scanWiFi() {
		ScanResult result;
		if (sta_attempt_in_progress) {
			logMessage("Skipping SSID refresh while STA attempt is in progress");
			return scanCache;
		}
		unsigned long now = millis();
		prepareScan(now);
		logMessage("scanWiFi started");
		int n = WiFi.scanNetworks();
		if (n <= 0) {
			logMessage("scanWiFi failed: %s (%d). Keeping previous scan cache.", scanStatusName(n), n);
			return scanCache;
		}
		logMessage("scanWiFi completed: %d networks", n);
		std::vector<int> indices(n);
		for (int i = 0; i < n; ++i) indices[i] = i;
		std::sort(indices.begin(), indices.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
		for (int i : indices) {
			String found = WiFi.SSID(i);
			String bssid = WiFi.BSSIDstr(i);
			logMessage("Found SSID '%s' [%s] (%d dBm)", found.c_str(), bssid.c_str(), WiFi.RSSI(i));
			if (found.length() == 0) continue;
			result.ssids.push_back(found);
			result.bssids.push_back(bssid);
			result.rssis.push_back(WiFi.RSSI(i));
		}
		result.success = true;
		return result;
	}

	void startAP() {
		if (ap_mode) {
			logMessage("AP already active. Preserving captive portal.");
			return;
		}
		logMessage("Starting AP");
		server.reset();
		WiFi.mode(WIFI_AP_STA);
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
		server.on("/refill", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/refill/", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		registerSetupRoutes();
		registerResetWiFi();
		server.on("/restart", HTTP_GET, [](AsyncWebServerRequest* req) {
			req->send(200, "text/plain", "Restarting...");
			delay(500);
			ESP.restart();
		});
		server.on("/generate_204", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/hotspot-detect.html", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/ncsi.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.onNotFound([this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});

		server.begin();
		logMessage("AP web server started");
	}

	void stopAP() {
		logMessage("Stopping AP");
		server.reset();
		WiFi.softAPdisconnect(true);
		if (WiFi.status() == WL_CONNECTED) {
			WiFi.mode(WIFI_STA);
		}
		ap_mode = false;
	}

public:
	WiFiManager(AsyncWebServer& srv,
				 const char* apSSID = WiFiConfig::kApSsid,
				 const char* apPASS = WiFiConfig::kApPassword,
				 LogFn log_fn = nullptr,
				 unsigned long retry_ms = 60000,
				 unsigned long ap_timeout_ms = 60000)
		: server(srv),
		  ap_ssid(apSSID),
		  ap_pass(apPASS),
		  logger(log_fn),
		  retry_interval(retry_ms),
		  ap_timeout(ap_timeout_ms) {}

	void begin(void (*start_web_server_fn)() = nullptr) {
		start_web_server = start_web_server_fn;
		prefs.begin("wifi", false);
		ssid = prefs.getString("ssid", "");
		pass = prefs.getString("pass", "");
		prefs.end();

		transitionTo(WiFiState::TRY_STA, millis(), "initial startup");
		if (ssid.length()) {
			beginStaAttempt(millis(), "initial startup");
		} else {
			transitionTo(WiFiState::START_AP, millis(), "no saved credentials");
		}
	}

	void setLogger(LogFn log_fn) { logger = log_fn; }

	String getWiFiOptions() {
		if (!scan_requested && !scanCache.success) {
			scan_requested = true;
		}
		return renderWiFiOptions();
	}

	void registerSetupRoutes() {
		server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.on("/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/plain", getWiFiOptions());
		});
		server.on("/config", HTTP_POST, [this](AsyncWebServerRequest* req) {
			if (!req->hasParam("ssid", true) || !req->hasParam("pass", true)) {
				req->send(400, "text/plain", "Missing SSID or password");
				return;
			}

			ssid = req->getParam("ssid", true)->value();
			pass = req->getParam("pass", true)->value();

			prefs.begin("wifi", false);
			prefs.putString("ssid", ssid);
			prefs.putString("pass", pass);
			prefs.end();

			logMessage("WiFi credentials updated for SSID '%s'. Starting reconnect.", ssid.c_str());
			AsyncWebServerResponse* response = req->beginResponse(
				200,
				"text/html",
				"<h1>Credentials saved.</h1><h2>Reconnecting to WiFi...</h2><p>You can close this page and return to the main UI.</p>");
			response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
			response->addHeader("Pragma", "no-cache");
			response->addHeader("Expires", "0");
			req->send(response);
			WiFi.disconnect(true, false);
			sta_attempt_in_progress = false;
			scan_requested = false;
			transitionTo(WiFiState::TRY_STA, millis(), "credentials updated via web UI");
		});
	}

	void registerResetWiFi() {
		server.on("/clear", HTTP_GET, [this](AsyncWebServerRequest* req) {
			if (req->hasArg("confirm")) {
				logMessage("Reset WiFi requested. Clearing saved credentials and rebooting.");
				Preferences clearPrefs;
				clearPrefs.begin("wifi", false);
				clearPrefs.clear();
				clearPrefs.end();
				AsyncWebServerResponse* response = req->beginResponse(
					200,
					"text/html",
					"<h1>Preferences cleared.</h1><h2>Device will restart in 1.5 seconds.</h2>");
				response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
				response->addHeader("Pragma", "no-cache");
				response->addHeader("Expires", "0");
				req->send(response);
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

			AsyncWebServerResponse* response = req->beginResponse(200, "text/html", html);
			response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
			response->addHeader("Pragma", "no-cache");
			response->addHeader("Expires", "0");
			req->send(response);
		});
	}

	void handle() {
		unsigned long now = millis();
		monitorConnection(now);

		switch (state) {
			case WiFiState::BOOT:
				if (shouldRefreshScan(now)) {
					scanCache = scanWiFi();
					last_scan = now;
					scan_requested = false;
				}
				break;

			case WiFiState::TRY_STA:
				beginStaAttempt(now, ap_mode ? "AP+STA reconnect attempt" : "STA connect requested");
				break;

			case WiFiState::STA_WAIT:
				if (WiFi.status() == WL_CONNECTED) {
					last_wifi_status = WL_CONNECTED;
					disconnect_detected_at = 0;
					sta_attempt_in_progress = false;
					logMessage("STA connected. IP: %s, BSSID: %s, RSSI: %d dBm",
						WiFi.localIP().toString().c_str(),
						WiFi.BSSIDstr().c_str(),
						WiFi.RSSI());
					if (start_web_server) start_web_server();
					if (ap_mode) {
						stopAP();
					}
					transitionTo(WiFiState::BOOT, now, "STA connected");
				} else if (now - state_start > 10000) {
					logMessage("STA connection timed out after %lu ms", now - state_start);
					sta_attempt_in_progress = false;
					if (ap_mode) {
						transitionTo(WiFiState::AP_ACTIVE, now, "STA timeout with AP preserved");
					} else {
						transitionTo(WiFiState::START_AP, now, "STA timeout");
					}
				}
				break;

			case WiFiState::START_AP:
				startAP();
				disconnect_detected_at = now;
				transitionTo(WiFiState::AP_ACTIVE, now, "AP started");
				ap_start = now;
				last_scan = now;
				if (ssid.length() && !sta_attempt_in_progress && now - last_sta_attempt >= retry_interval) {
					transitionTo(WiFiState::TRY_STA, now, "AP active retry window open");
				}
				break;

			case WiFiState::AP_ACTIVE:
				if (shouldRefreshScan(now)) {
					scanCache = scanWiFi();
					last_scan = now;
					scan_requested = false;
				}
				if (ssid.length() && !sta_attempt_in_progress && now - last_sta_attempt >= retry_interval) {
					transitionTo(WiFiState::TRY_STA, now, "AP+STA periodic retry");
					break;
				}
				if (now - ap_start > ap_timeout && WiFi.softAPgetStationNum() == 0) {
					if (ssid.length()) {
						logMessage("AP timeout expired with no clients. Keeping AP+STA recovery active.");
						ap_start = now;
					} else {
						logMessage("No credentials. Holding AP active.");
						ap_start = now;
					}
				}
				break;

			default:
				break;
		}
	}

	String renderWiFiOptions() {
		if (scan_requested) return "<option disabled>Scanning...</option>";
		if (!scanCache.success) return "<option disabled>Scan failed</option>";
		String options;
		String connected_bssid = WiFi.status() == WL_CONNECTED ? WiFi.BSSIDstr() : String();
		bool selected_connected_bssid = false;
		bool selected_saved_ssid = false;
		for (size_t i = 0; i < scanCache.ssids.size(); ++i) {
			options += "<option value='" + scanCache.ssids[i] + "'";
			if (!selected_connected_bssid && connected_bssid.length() > 0 && i < scanCache.bssids.size() && scanCache.bssids[i] == connected_bssid) {
				options += " selected";
				selected_connected_bssid = true;
			} else if (!selected_connected_bssid && !selected_saved_ssid && scanCache.ssids[i] == ssid) {
				options += " selected";
				selected_saved_ssid = true;
			}
			options += ">";
			options += scanCache.ssids[i] + " (" + String(scanCache.rssis[i]) + " dBm)";
			if (i < scanCache.bssids.size()) {
				options += " [" + scanCache.bssids[i] + "]";
			}
			options += "</option>";
		}
		if (options.length() == 0) return "<option disabled>No networks found</option>";
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
				.actions {
					margin-top: 1rem;
					display: flex;
					justify-content: center;
				}
				.button-link {
					display: inline-block;
					padding: 0.7em 1em;
					border-radius: 4px;
					background: #b00;
					color: #fff;
					text-decoration: none;
					text-align: center;
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

				<input type="submit" value="Save & Reconnect">
			</form>

			<div class="actions">
				<a href="/clear" class="button-link">Clear Preferences</a>
			</div>

			<script>
				window.addEventListener('load', () => {
					const select = document.getElementById('ssidList');
					const loadScan = () => {
						fetch('/scan')
							.then(response => {
								if (!response.ok) throw new Error('HTTP ' + response.status);
								return response.text();
							})
							.then(html => {
								if (!select) return;
								const content = html && html.trim().length
									? html
									: '<option disabled>No networks found</option>';
								select.innerHTML = content;
								if (content.includes('Scanning...')) {
									setTimeout(loadScan, 1500);
								}
							})
							.catch(err => {
								console.error('Scan fetch failed:', err);
								if (select) {
									select.innerHTML = '<option disabled>Scan request failed</option>';
								}
							});
					};
					setTimeout(loadScan, 300);
				});

				document.querySelector('form').addEventListener('submit', async (event) => {
					event.preventDefault();
					const form = event.currentTarget;
					const formData = new FormData(form);
					const response = await fetch('/config', {
						method: 'POST',
						body: formData
					});
					document.open();
					document.write(await response.text());
					document.close();
				});
			</script>
		</body>
		</html>
		)rawliteral";
	}
};
