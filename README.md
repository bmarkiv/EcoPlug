# Custom Firmware for Dewenwils EcoPlug

This project provides custom ESP32-based firmware for:
- [EcoPlug Outdoor Wireless Controller](http://www.eco-plugs.net/en/products/show.php?id=70)
- [Dewenwils Heavy Duty Outdoor Smart Plug](https://www.amazon.com/dp/B07PP2KNNH)

## ⚠️ Background

Originally shipped with an ESP8266-based module. Reflash attempt using [ESPHome’s HOWT01A instructions](https://devices.esphome.io/devices/Dewenwils-Heavy-Duty-40A-Outdoor-Plug-HOWT01A) did not work on my unit.

ESP8266 was replaced with an ESP32.

## 💡 Purpose

This firmware is used to control a **pool filter pump** with:
- WiFi provisioning via fallback AP
- Web-based captive portal for SSID/password setup
- Automatic STA retry
- Optional OTA update support

## 🛠️ Features

- Deterministic state machine for boot and recovery
- Configurable AP fallback timeout
- WiFi scan caching for stable UI rendering
- Clean mobile-friendly config page
- Debug and reset routes

## 🚀 Build and Upload

1. Build firmware:

```bash
platformio run --environment ESP32
```

2. Upload firmware (OTA):

```bash
platformio run --target upload --environment ESP32
```

3. Verify the OTA target IP in [platformio.ini](platformio.ini) before upload:

```ini
upload_protocol = espota
upload_port = <ESP32-IP>
```

If you have more than one device, always confirm `upload_port` points to the intended ESP32.

AP SSID/password and captive portal IP are centralized in [../WifiManager/WifiManager.h](../WifiManager/WifiManager.h).

## 📶 First-Time WiFi Provisioning

When no STA credentials are saved, the device starts in AP mode.

1. Connect to AP SSID: `SetupAP`
2. AP password: `setup123`
3. Open captive portal: `http://10.0.2.1`
4. Select your WiFi SSID, enter password, then submit to save and reboot.

## 🧯 WiFi Recovery

If device cannot connect to your router or credentials are outdated:

1. Open `http://<device-ip>/clear`
2. Confirm reset of saved WiFi preferences
3. Device reboots and returns to AP mode (`SetupAP`)
4. Reconfigure WiFi at `http://10.0.2.1`

## 🔧 Serial Log Hint

This log line is expected when no STA credentials are stored:

`[WiFiManager] No credentials. Holding AP.`

It means AP provisioning mode is active and waiting for setup via `http://10.0.2.1`.

If you need a different AP IP later, update [WiFiConfig.h](WiFiConfig.h) only.
