#pragma once

#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Update.h>

#include <Preferences.h>

#include <algorithm>
#include <stdarg.h>
#include <vector>

#include "WiFiConfig.h"

enum class WiFiState {
	TRY_STA,
	STA_WAIT,
	STA_CONNECTED,
	AP_ACTIVE
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
	bool* server_initialized;
	LogFn logger;
	unsigned long ap_started_at = 0;
	unsigned long failover_started_at = 0;
	WiFiState state = WiFiState::TRY_STA;
	bool ap_mode = false;
	bool scan_in_progress = false;
	static constexpr size_t kUpdateProgressPollMs = 500;
	Preferences prefs;
	DNSServer dnsServer;
	String ssid;
	String pass;
	String host_name;
	String sta_ip;
	IPAddress sta_local_ip;
	bool sta_ip_valid = false;
	IPAddress ap_ip;
	volatile size_t update_bytes_received = 0;
	volatile size_t update_bytes_total = 0;
	volatile bool update_in_progress = false;
	volatile bool update_finished = false;
	volatile bool update_failed = false;
	String update_filename;
	bool ota_initialized = false;
	void (*start_web_server)() = nullptr;
	ScanResult scanCache;

	static bool parseIPv4(const String& value, IPAddress& out) {
		if (!value.length()) return false;
		return out.fromString(value);
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

	static const char* disconnectReasonName(uint8_t reason) {
		switch (reason) {
			case 1: return "UNSPECIFIED";
			case 2: return "AUTH_EXPIRE";
			case 3: return "AUTH_LEAVE";
			case 4: return "ASSOC_EXPIRE";
			case 5: return "ASSOC_TOOMANY";
			case 6: return "NOT_AUTHED";
			case 7: return "NOT_ASSOCED";
			case 8: return "ASSOC_LEAVE";
			case 9: return "ASSOC_NOT_AUTHED";
			case 10: return "DISASSOC_PWRCAP_BAD";
			case 11: return "DISASSOC_SUPCHAN_BAD";
			case 13: return "IE_INVALID";
			case 14: return "MIC_FAILURE";
			case 15: return "4WAY_HANDSHAKE_TIMEOUT";
			case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
			case 17: return "IE_IN_4WAY_DIFFERS";
			case 18: return "GROUP_CIPHER_INVALID";
			case 19: return "PAIRWISE_CIPHER_INVALID";
			case 20: return "AKMP_INVALID";
			case 21: return "UNSUPP_RSN_IE_VERSION";
			case 22: return "INVALID_RSN_IE_CAP";
			case 23: return "802_1X_AUTH_FAILED";
			case 24: return "CIPHER_SUITE_REJECTED";
			case 200: return "BEACON_TIMEOUT";
			case 201: return "NO_AP_FOUND";
			case 202: return "AUTH_FAIL";
			case 203: return "ASSOC_FAIL";
			case 204: return "HANDSHAKE_TIMEOUT";
			default: return "UNKNOWN";
		}
	}

	static const char* authModeName(wifi_auth_mode_t mode) {
		switch (mode) {
			case WIFI_AUTH_OPEN: return "OPEN";
			case WIFI_AUTH_WEP: return "WEP";
			case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
			case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
			case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
			case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
			case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
			case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
			default: return "AUTH_UNKNOWN";
		}
	}

	void logWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
		switch (event) {
			case ARDUINO_EVENT_WIFI_READY:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_READY");
				break;
			case ARDUINO_EVENT_WIFI_SCAN_DONE:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_SCAN_DONE");
				break;
			case ARDUINO_EVENT_WIFI_STA_START:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_START");
				break;
			case ARDUINO_EVENT_WIFI_STA_STOP:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_STOP");
				break;
			case ARDUINO_EVENT_WIFI_STA_CONNECTED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_CONNECTED ssid='%s' channel=%d auth=%s",
					reinterpret_cast<const char*>(info.wifi_sta_connected.ssid),
					info.wifi_sta_connected.channel,
					authModeName(static_cast<wifi_auth_mode_t>(info.wifi_sta_connected.authmode)));
				break;
			case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_DISCONNECTED reason=%d(%s)",
					info.wifi_sta_disconnected.reason,
					disconnectReasonName(info.wifi_sta_disconnected.reason));
				break;
			case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE");
				break;
			case ARDUINO_EVENT_WIFI_STA_GOT_IP:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_GOT_IP ip=%s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
				break;
			case ARDUINO_EVENT_WIFI_STA_LOST_IP:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_STA_LOST_IP");
				break;
			case ARDUINO_EVENT_WIFI_AP_START:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_START");
				break;
			case ARDUINO_EVENT_WIFI_AP_STOP:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_STOP");
				break;
			case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_STACONNECTED aid=%d", info.wifi_ap_staconnected.aid);
				break;
			case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_STADISCONNECTED aid=%d", info.wifi_ap_stadisconnected.aid);
				break;
			case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED");
				break;
			case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
				logMessage("WiFi event: ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED rssi=%d", info.wifi_ap_probereqrecved.rssi);
				break;
			default:
				logMessage("WiFi event: %d", static_cast<int>(event));
				break;
		}
	}

	static const char* stateName(WiFiState value) {
		switch (value) {
			case WiFiState::TRY_STA:
				return "TRY_STA";
			case WiFiState::STA_WAIT:
				return "STA_WAIT";
			case WiFiState::STA_CONNECTED:
				return "STA_CONNECTED";
			case WiFiState::AP_ACTIVE:
				return "AP_ACTIVE";
		}
		return "UNKNOWN";
	}

	void setState(WiFiState nextState, unsigned long now, const char* reason) {
		if (state == nextState) return;
		logMessage("State %s -> %s (%s)", stateName(state), stateName(nextState), reason);
		state = nextState;
	}

	void handleScanRequest(AsyncWebServerRequest* req, const char* source) {
		logMessage("%s: scan_in_progress=%s cached=%s", source, scan_in_progress ? "true" : "false", scanCache.success ? "true" : "false");
		if (!scan_in_progress) {
			if (scanCache.success) {
				logMessage("%s: returning cached results", source);
				req->send(200, "text/plain", getWiFiOptions());
				return;
			}

			logMessage("%s: scan started", source);
			scan_in_progress = true;
			WiFi.scanDelete();
			int start_result = WiFi.scanNetworks(true, false, false, 0);
			logMessage("%s: WiFi.scanNetworks(true, false, false, 0) -> %d", source, start_result);
			req->send(202, "text/plain", "scan started");
			return;
		}

		int status = WiFi.scanComplete();
		logMessage("%s: WiFi.scanComplete() -> %d", source, status);
		if (status == WIFI_SCAN_RUNNING || status == WIFI_SCAN_FAILED) {
			logMessage("%s: scan in progress", source);
			req->send(202, "text/plain", "scan in progress");
			return;
		}

		int count = status;
		scan_in_progress = false;
		if (count <= 0) {
			logMessage("%s: Scan failed, count=%d", source, count);
			scanCache = ScanResult();
			req->send(200, "text/plain", "<option disabled>Scan failed</option>");
			return;
		}

		ScanResult result;
		std::vector<int> indices(count);
		for (int i = 0; i < count; ++i) indices[i] = i;
		std::sort(indices.begin(), indices.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
		for (int i : indices) {
			String found = WiFi.SSID(i);
			String bssid = WiFi.BSSIDstr(i);
			int rssi = WiFi.RSSI(i);
			if (!found.length()) continue;
			logMessage("%s: scan result: ssid='%s' bssid='%s' rssi=%d", source, found.c_str(), bssid.c_str(), rssi);
			result.ssids.push_back(found);
			result.bssids.push_back(bssid);
			result.rssis.push_back(rssi);
		}
		result.success = true;
		scanCache = result;
		logMessage("%s: scan complete: %u networks cached", source, (unsigned)scanCache.ssids.size());
		req->send(200, "text/plain", getWiFiOptions());
	}

	bool handleStaLink(unsigned long now, const char* disconnect_reason) {
		if (WiFi.status() == WL_CONNECTED) {
			failover_started_at = 0;
			return true;
		}

		if (failover_started_at == 0) {
			failover_started_at = now;
			return false;
		}

		if (now - failover_started_at >= WiFiConfig::sta_timeout) {
			startAP(now, disconnect_reason);
		}
		return false;
	}

    void loadPrefs() {
        prefs.begin("wifi", false);

        ssid      = prefs.getString("ssid", "");
        pass      = prefs.getString("pass", "");
        host_name = prefs.getString("host", "");
        sta_ip    = prefs.getString("ip", "");

        prefs.end();

        // Normalize
        ssid.trim();
        pass.trim();
        host_name.trim();
        sta_ip.trim();

        // Parse static IP
        sta_ip_valid = parseIPv4(sta_ip, sta_local_ip);

        // Logging moved here
        logMessage("Loaded prefs:");
        logMessage("  SSID        = '%s'", ssid.c_str());
        // logMessage("  PASS        = '%s'", pass.c_str());
		logMessage("  PASS length = '%d'", pass.length());
        logMessage("  HOST        = '%s'", host_name.c_str());
        logMessage("  STA_IP      = '%s' (%s)",
                sta_ip.c_str(),
                sta_ip_valid ? "valid" : "invalid");
    }

    bool beginSta(unsigned long now, const char* reason) {
        if (!ssid.length()) {
            logMessage("No saved credentials");
            return false;
        }

        logMessage("Attempting STA connection to SSID '%s' (%s)",
                ssid.c_str(), reason);

        // --- WiFi.mode() ---
        bool mode_ok = WiFi.mode(WIFI_STA);
        logMessage("WiFi.mode(WIFI_STA) -> %s", mode_ok ? "OK" : "FAIL");
        if (!mode_ok) return false;

        // --- Hostname ---
        if (host_name.length()) {
            bool host_ok = WiFi.setHostname(host_name.c_str());
            logMessage("WiFi.setHostname('%s') -> %s",
                    host_name.c_str(),
                    host_ok ? "OK" : "FAIL");
            if (!host_ok) return false;
        }

        // --- IP configuration ---
        if (!sta_ip.length() || !sta_ip_valid) {
            if (!sta_ip.length())
                logMessage("Using DHCP for STA IP");
            else
                logMessage("Invalid STA IP '%s'. Falling back to DHCP.", sta_ip.c_str());

            bool cfg_ok = WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
            logMessage("WiFi.config(DHCP) -> %s", cfg_ok ? "OK" : "FAIL");
            if (!cfg_ok) return false;
        } else {
            IPAddress gateway(sta_local_ip[0], sta_local_ip[1], sta_local_ip[2], 1);
            IPAddress subnet(255, 255, 255, 0);

            bool cfg_ok = WiFi.config(sta_local_ip, gateway, subnet);
            logMessage("WiFi.config(%s, gateway %s) -> %s",
                    sta_ip.c_str(),
                    gateway.toString().c_str(),
                    cfg_ok ? "OK" : "FAIL");
            if (!cfg_ok) return false;
        }

        // --- WiFi.begin() ---
        wl_status_t st = WiFi.begin(ssid.c_str(), pass.c_str());
        logMessage("WiFi.begin('%s') -> wl_status_t %d",
                ssid.c_str(), (int)st);

        // If begin() fails immediately, return false
        if (st == WL_CONNECT_FAILED ||
            st == WL_NO_SSID_AVAIL ||
            st == WL_IDLE_STATUS) {
            return false;
        }

        // Otherwise STA is starting — caller will wait for events
        setState(WiFiState::STA_WAIT, now, reason);
        return true;
    }

	void startAP(unsigned long now, const char* reason) {
		logMessage("Starting AP");
		WiFi.mode(WIFI_AP);
		if (!ap_ip.fromString(WiFiConfig::kApIp)) {
			ap_ip = IPAddress(192, 168, 4, 1);
		}
		IPAddress ap_gateway = ap_ip;
		IPAddress ap_subnet;
		if (!ap_subnet.fromString(WiFiConfig::kApSubnet)) {
			ap_subnet = IPAddress(255, 255, 255, 0);
		}
		WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
		WiFi.softAP(ap_ssid, ap_pass);

		server.reset();
		if (server_initialized) *server_initialized = false;
		server.on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		register_ap_routes();
		server.onNotFound([this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
		server.begin();

		dnsServer.stop();
		dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
		dnsServer.start(53, "*", ap_ip);
		logMessage("Captive portal: http://%s", WiFiConfig::kApIp);
		logMessage("AP web server started");
		ap_mode = true;
		ap_started_at = now;
		setState(WiFiState::AP_ACTIVE, now, reason);
	}

	void register_ap_routes() {
		registerSetupRoutes();
		registerResetWiFi();
		registerPortalRoutes();
		server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *req) {
			req->send(200, "text/plain", "Restarting...");
			delay(500);
			ESP.restart();
		});
	}

public:
	WiFiManager(AsyncWebServer& srv,
				 const char* apSSID = WiFiConfig::kApSsid,
				 const char* apPASS = WiFiConfig::kApPassword,
				 bool* server_initialized_flag = nullptr,
				 LogFn log_fn = nullptr)
		: server(srv),
		  ap_ssid(apSSID),
		  ap_pass(apPASS),
		  server_initialized(server_initialized_flag),
		  logger(log_fn) {}

	void begin(void (*start_web_server_fn)() = nullptr) {
		start_web_server = start_web_server_fn;
		WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
			logWiFiEvent(event, info);
		});
		loadPrefs();
		registerUpdateRoutes();
		state = WiFiState::TRY_STA;
	}

	void setLogger(LogFn log_fn) { logger = log_fn; }

	bool isApMode() const { return ap_mode; }

	void handleWifiRoute(AsyncWebServerRequest* req) {
		logMessage("handleWifiRoute: url='%s' method=%s", req->url().c_str(), req->methodToString());
		if (req->url() == "/scan") {
			logMessage("handleWifiRoute: dispatching to scan handler");
			handleScanRequest(req, "wifi route");
			return;
		}
		if (req->url() == "/config" && req->method() == HTTP_POST) {
			handleConfigPost(req);
			return;
		}
		req->send(200, "text/html", renderConfigPage());
	}

	String getWiFiOptions() {
		return renderWiFiOptions();
	}

	void registerSetupRoutes() {
		server.on("/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
			handleScanRequest(req, "/scan route");
		});
		server.on("/config", HTTP_POST, [this](AsyncWebServerRequest* req) {
			handleConfigPost(req);
		});
		server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderConfigPage());
		});
	}

	void handleConfigPost(AsyncWebServerRequest* req) {
		if (!req->hasParam("ssid", true) || !req->hasParam("pass", true)) {
			req->send(400, "text/plain", "Missing SSID or password");
			return;
		}

		ssid = req->getParam("ssid", true)->value();
		pass = req->getParam("pass", true)->value();
		host_name = req->hasParam("host", true) ? req->getParam("host", true)->value() : String();
		sta_ip = req->hasParam("ip", true) ? req->getParam("ip", true)->value() : String();
		host_name.trim();
		sta_ip.trim();

		prefs.begin("wifi", false);
		prefs.putString("ssid", ssid);
		prefs.putString("pass", pass);
		prefs.putString("host", host_name);
		prefs.putString("ip", sta_ip);
		prefs.end();

		AsyncWebServerResponse* response = req->beginResponse(
			200,
			"text/html",
			"<h1>WiFi settings saved.</h1><h2>Device will reboot now.</h2><p>Reconnect after reboot.</p>");
		response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
		response->addHeader("Pragma", "no-cache");
		response->addHeader("Expires", "0");
		req->send(response);
		delay(500);
		ESP.restart();
	}

	void registerPortalRoutes() {
		const char* portal_paths[] = { "/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt" };
		for (const char* path : portal_paths) {
			server.on(path, HTTP_GET, [this](AsyncWebServerRequest* req) {
				req->send(200, "text/html", renderConfigPage());
			});
		}
	}

	void resetUpdateState() {
		update_bytes_received = 0;
		update_bytes_total = 0;
		update_in_progress = false;
		update_finished = false;
		update_failed = false;
		update_filename = String();
	}

	String renderUpdatePage() {
		String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Firmware update</title>
  <style>
    body { font-family: Arial, Helvetica, sans-serif; max-width: 640px; margin: 40px auto; padding: 0 16px; }
    .bar { width: 100%; height: 20px; background: #e5e7eb; border-radius: 999px; overflow: hidden; }
    .bar > div { height: 100%; width: 0%; background: #2563eb; transition: width 0.15s linear; }
    .status { margin-top: 12px; font-size: 14px; }
    .meta { margin-top: 6px; color: #4b5563; font-size: 13px; }
  </style>
</head>
<body>
  <h1>Firmware update</h1>
  <form id="update-form" enctype="multipart/form-data">
		<input type="file" name="update" id="update-file">
    <input type="submit" value="Update">
  </form>
  <div style="margin-top: 20px;">
    <div class="bar"><div id="progress"></div></div>
    <div class="status" id="status">Idle</div>
    <div class="meta" id="meta">0% | 0 / 0 bytes</div>
  </div>
  <script>
    const progressEl = document.getElementById('progress');
    const statusEl = document.getElementById('status');
    const metaEl = document.getElementById('meta');
    const formEl = document.getElementById('update-form');
    const fileEl = document.getElementById('update-file');

    const setProgress = (received, total) => {
      const percent = total ? Math.min(100, Math.round((received * 100) / total)) : 0;
      progressEl.style.width = percent + '%';
      metaEl.textContent = percent + '% | ' + received + ' / ' + total + ' bytes';
    };

    const refresh = async () => {
      try {
        const response = await fetch('/update/status', { cache: 'no-store' });
        const data = await response.json();
        setProgress(data.received, data.total);

        if (data.failed) {
          statusEl.textContent = 'Update failed';
        } else if (data.finished) {
          statusEl.textContent = 'Verifying update, rebooting...';
        } else if (data.inProgress) {
          statusEl.textContent = 'Uploading ' + (data.filename || 'firmware.bin');
        } else {
          statusEl.textContent = 'Idle';
        }
      } catch (error) {
        statusEl.textContent = 'Status unavailable';
      }
    };

    formEl.addEventListener('submit', (event) => {
      event.preventDefault();

			if (!fileEl.files.length) {
				statusEl.textContent = 'Choose a firmware file first';
        return;
      }

      const formData = new FormData();
			formData.append('update', fileEl.files[0]);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update');

      xhr.upload.onprogress = (event) => {
        if (event.lengthComputable) {
          setProgress(event.loaded, event.total);
					statusEl.textContent = 'Uploading ' + fileEl.files[0].name;
        } else {
					statusEl.textContent = 'Uploading ' + fileEl.files[0].name;
        }
      };

      xhr.onload = () => {
        if (xhr.status === 200) {
          progressEl.style.width = '100%';
					metaEl.textContent = '100% | ' + fileEl.files[0].size + ' / ' + fileEl.files[0].size + ' bytes';
          statusEl.textContent = 'Verifying update, rebooting...';
        } else {
          statusEl.textContent = 'Update failed';
        }
      };

      xhr.onerror = () => {
        statusEl.textContent = 'Update failed';
      };

			statusEl.textContent = 'Starting upload...';
			setProgress(0, fileEl.files[0].size || 0);
      xhr.send(formData);
    });

    setInterval(refresh, 500);
    refresh();
  </script>
</body>
</html>
)rawliteral";
		return page;
	}

	void registerUpdateRoutes() {
		server.on("/update", HTTP_GET, [this](AsyncWebServerRequest* req) {
			req->send(200, "text/html", renderUpdatePage());
		});

		server.on("/update/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
			String json = R"({"received":)";
			json += String((unsigned long)update_bytes_received);
			json += R"(,"total":)";
			json += String((unsigned long)update_bytes_total);
			json += R"(,"inProgress":)";
			json += update_in_progress ? "true" : "false";
			json += R"(,"finished":)";
			json += update_finished ? "true" : "false";
			json += R"(,"failed":)";
			json += update_failed ? "true" : "false";
			json += R"(,"filename":")";
			json += update_filename;
			json += R"("})";
			req->send(200, "application/json", json);
		});

		server.on("/update", HTTP_POST,
			[this](AsyncWebServerRequest* req) {
				bool success = !Update.hasError();
				update_in_progress = false;
				update_finished = success;
				update_failed = !success;
				req->send(200, "text/plain", success ? "OK. Rebooting..." : "FAIL");
				if (success) {
					delay(100);
					ESP.restart();
				}
			},
			[this](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
				if (!index) {
					logMessage("Update start: %s", filename.c_str());
					update_filename = filename;
					update_bytes_received = 0;
					update_bytes_total = req->contentLength();
					update_in_progress = true;
					update_finished = false;
					update_failed = false;
					if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
						Update.printError(Serial);
						update_in_progress = false;
						update_failed = true;
					}
				}

				if (!Update.hasError()) {
					update_bytes_received = index + len;
					if (Update.write(data, len) != len) {
						Update.printError(Serial);
						update_in_progress = false;
						update_failed = true;
					}
				}

				if (final) {
					if (Update.end(true)) {
						logMessage("Update complete: %u bytes", index + len);
						update_bytes_received = index + len;
						update_in_progress = false;
						update_finished = true;
					} else {
						Update.printError(Serial);
						update_in_progress = false;
						update_failed = true;
					}
				}
			});
	}

	void registerResetWiFi() {
		server.on("/clear", HTTP_GET, [this](AsyncWebServerRequest* req) {
			if (req->hasArg("confirm")) {
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
		if (!ota_initialized && (WiFi.status() == WL_CONNECTED || ap_mode)) {
			ArduinoOTA.setHostname(host_name.length() ? host_name.c_str() : ap_ssid);
			ArduinoOTA.begin();
			ota_initialized = true;
			logMessage("ArduinoOTA started");
		}
		if (ota_initialized) {
			ArduinoOTA.handle();
		}
		if (ap_mode) {
			dnsServer.processNextRequest();
		}

		switch (state) {
			case WiFiState::TRY_STA:
				if (!beginSta(now, "initial startup"))
                    startAP(now, "beginSta failed");
				break;

			case WiFiState::STA_WAIT:
				if (handleStaLink(now, "STA timeout")) {
					logMessage("STA connected. IP: %s, BSSID: %s, RSSI: %d dBm",
						WiFi.localIP().toString().c_str(),
						WiFi.BSSIDstr().c_str(),
						WiFi.RSSI());
					if (start_web_server) start_web_server();
					setState(WiFiState::STA_CONNECTED, now, "STA connected");
				}
				break;

			case WiFiState::STA_CONNECTED:
				handleStaLink(now, "runtime STA disconnect");
				break;

			case WiFiState::AP_ACTIVE:
				if (now - ap_started_at > WiFiConfig::ap_timeout) {
					logMessage("AP timeout expired. Rebooting device.");
					delay(500);
					ESP.restart();
				}
				break;
		}
	}

	String renderWiFiOptions() {
		if (!scanCache.success) return "<option disabled>Loading networks...</option>";
		String options;
		for (size_t i = 0; i < scanCache.ssids.size(); ++i) {
			options += "<option value='" + scanCache.ssids[i] + "'";
			if (scanCache.ssids[i] == ssid) options += " selected";
			options += ">";
			options += scanCache.ssids[i] + " (" + String(scanCache.rssis[i]) + " dBm)";
			if (i < scanCache.bssids.size()) {
				options += " [" + scanCache.bssids[i] + "]";
			}
			options += "</option>";
		}
		if (!options.length()) return "<option disabled>No networks found</option>";
		return options;
	}

	String renderConfigPage() {
		String initial_options = renderWiFiOptions();
		if (!initial_options.length()) initial_options = "<option disabled>Loading networks...</option>";
		String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<title>Configure WiFi</title>
	<style>
		body {
			font-family: Arial, Helvetica, Sans-Serif;
			margin: 0;
			padding: 20px;
			display: flex;
			justify-content: center;
		}
		.panel {
			width: min(640px, 100%);
			border-radius: 14px;
			padding: 18px;
			box-sizing: border-box;
		}
		.section {
			padding: 14px;
			border-radius: 10px;
			margin-bottom: 14px;
			background: rgba(127, 127, 127, 0.08);
		}
		h1,
		h2,
		h3 {
			margin: 0 0 12px;
		}
		label {
			display: block;
			margin-bottom: 6px;
			font-weight: 700;
		}
		select,
		input[type="password"],
		input[type="text"] {
			width: 100%;
			padding: 10px;
			font-size: 0.95rem;
			border-radius: 8px;
			border: 1px solid #3a4551;
			box-sizing: border-box;
			background: #232a31;
			color: #e5e7eb;
		}
		select {
			min-height: 44px;
		}
		input::placeholder {
			color: #9ca3af;
		}
		.form-row {
			margin-bottom: 14px;
		}
		.primary,
		.secondary {
			padding: 10px 14px;
			border-radius: 10px;
			font-size: 0.95rem;
			cursor: pointer;
		}
		.primary {
			background: #0f766e;
			color: #e5e7eb;
			border: 1px solid #14b8a6;
		}
		.secondary {
			background: #232a31;
			color: #e5e7eb;
			border: 1px solid #3a4551;
			text-decoration: none;
			display: inline-flex;
			align-items: center;
			justify-content: center;
		}
		.actions {
			margin-top: 1rem;
			display: flex;
			justify-content: center;
		}
		@media (prefers-color-scheme: light) {
			body {
				background-color: #f4f7fb;
				color: #10243b;
			}
			.panel {
				background: #ffffff;
				box-shadow: 0 10px 30px rgba(16, 36, 59, 0.08);
			}
			.section {
				background: rgba(16, 36, 59, 0.04);
			}
			select,
			input[type="password"],
			input[type="text"] {
				background: #fff;
				color: #10243b;
				border-color: #c9d5e3;
			}
			input::placeholder {
				color: #4f647a;
			}
			.secondary {
				background: #fff;
				color: #10243b;
				border-color: #c9d5e3;
			}
		}
		@media (prefers-color-scheme: dark) {
			body {
				background-color: #111315;
				color: #e5e7eb;
			}
			.panel {
				background: #1b2025;
				box-shadow: 0 0 0 1px #2b3138;
			}
			.secondary {
				background: #232a31;
				color: #e5e7eb;
				border-color: #3a4551;
			}
		}
	</style>
</head>
<body>
	<div class="panel">
		<h2>Configure WiFi</h2>
		<div class="section">
			<form method="POST" action="/config">
				<div class="form-row">
					<label for="ssidList">SSID</label>
					<select name="ssid" id="ssidList">
						%SSID_OPTIONS%
					</select>
				</div>

				<div class="form-row">
					<label for="pass">Password</label>
					<input type="password" name="pass" id="pass" value="%PASS%" autocomplete="new-password">
				</div>

				<div class="form-row">
					<label for="host">Host name</label>
					<input type="text" name="host" id="host" value="%HOST_NAME%" placeholder="ecoplug" autocomplete="off">
				</div>

				<div class="form-row">
					<label for="ip">IP address</label>
					<input type="text" name="ip" id="ip" value="%STA_IP%" placeholder="Leave blank for DHCP" inputmode="decimal" autocomplete="off">
				</div>

				<button type="submit" class="primary">Save & Reboot</button>
			</form>
		</div>

		<div class="actions">
			<a href="/clear" class="secondary">Clear Preferences</a>
		</div>
	</div>
	<script>
		const refreshScan = () => {
			let pollActive = true;
			const poll = () => {
				if (!pollActive) return;
				fetch('/scan', { cache: 'no-store' })
					.then((r) => r.text())
					.then((html) => {
						const list = document.getElementById('ssidList');
						if (html.includes('<option')) {
							if (list) list.innerHTML = html;
							pollActive = false;
							return;
						}
						if (html.includes('Scan failed') || html.includes('No networks found')) {
							if (list) list.innerHTML = html;
							pollActive = false;
							return;
						}
						if (html.includes('scan in progress') || html.includes('scan started')) {
							setTimeout(poll, 800);
							return;
						}
						pollActive = false;
					})
					.catch(() => {
						setTimeout(poll, 800);
					});
			};
			poll();
		};
		refreshScan();
	</script>
</body>
</html>
		)rawliteral";
		page.replace("%SSID_OPTIONS%", initial_options);
		page.replace("%PASS%", pass);
		page.replace("%HOST_NAME%", host_name);
		page.replace("%STA_IP%", sta_ip);
		return page;
	}
};