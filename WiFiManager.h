#pragma once
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

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
	AsyncWebServer& server;
	const char* ap_ssid;
	const char* ap_pass;
	unsigned long retry_interval;
	unsigned long ap_timeout;
	unsigned long last_scan = 0;
	unsigned long ap_start = 0;
	unsigned long state_start = 0;
	WiFiState state = WiFiState::BOOT;
	bool ap_mode = false;
	Preferences prefs;
	String ssid, pass;
	void (*start_web_server)();

	struct ScanResult {
		std::vector<String> ssids;
		std::vector<int> rssis;
		bool success = false;
	} scanCache;

   public:
	WiFiManager(AsyncWebServer& srv,
				 const char* apSSID = "SetupAP",
				 const char* apPASS = "setup123",
				 unsigned long retry_ms = 60000,
				 unsigned long ap_timeout_ms = 60000)
		: server(srv),
		  start_web_server(start_web_server),
		  ap_ssid(apSSID),
		  ap_pass(apPASS),
		  retry_interval(retry_ms),
		  ap_timeout(ap_timeout_ms) {}

	void begin(void (*start_web_server)() = nullptr) {
		this->start_web_server = start_web_server;
		prefs.begin("wifi", false);
		ssid = prefs.getString("ssid", "");
		pass = prefs.getString("pass", "");
		prefs.end();

		state = WiFiState::TRY_STA;
		state_start = millis();
		if (ssid.length()) {
			WiFi.mode(WIFI_STA);
			WiFi.begin(ssid.c_str(), pass.c_str());
		} else {
			state = WiFiState::START_AP;
		}
	}

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

		switch (state) {
			case WiFiState::TRY_STA:
				if (ssid.length()) {
					WiFi.mode(WIFI_STA);
					WiFi.begin(ssid.c_str(), pass.c_str());
					state = WiFiState::STA_WAIT;
					state_start = now;
				} else {
					Serial.println(
						"[WiFiManager] No credentials. Skipping STA attempt.");
					state = WiFiState::START_AP;
				}
				break;

			case WiFiState::STA_WAIT:
				if (WiFi.status() == WL_CONNECTED) {
					Serial.println("[WiFiManager] STA connected.");
					Serial.println("IP: " + WiFi.localIP().toString());
					if (start_web_server) start_web_server();
					state = WiFiState::BOOT;
				} else if (now - state_start > 10000) {
					Serial.println("[WiFiManager] STA connection timed out.");
					state = WiFiState::START_AP;
				}
				break;

			case WiFiState::START_AP:
				startAP();
				state = WiFiState::AP_ACTIVE;
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
						Serial.println(
							"[WiFiManager] AP timeout. Retrying STA.");
						stopAP();
						state = WiFiState::TRY_STA;
					} else {
						Serial.println(
							"[WiFiManager] No credentials. Holding AP.");
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
		Serial.println("[WiFiManager] scanWiFi...");
		std::vector<int> indices(n);
		for (int i = 0; i < n; ++i) indices[i] = i;
		std::sort(indices.begin(), indices.end(),
				  [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
		for (int i : indices) {
			String s = WiFi.SSID(i);
			Serial.printf("[WiFiManager] %s (%d dBm)\n", s.c_str(), WiFi.RSSI(i));
			if (s.length() == 0) continue;
			result.ssids.push_back(s);
			result.rssis.push_back(WiFi.RSSI(i));
		}
		result.success = true;
		return result;
	}

	void startAP() {
		Serial.println("[WiFiManager] Starting AP...");
		WiFi.mode(WIFI_AP);
		WiFi.softAP(ap_ssid, ap_pass);
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
		Serial.println("Web server started");
	}

	void stopAP() {
		Serial.println("[WiFiManager] Stopping AP.");
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
