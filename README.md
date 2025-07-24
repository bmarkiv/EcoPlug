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
