# Extractor
A smart solder extractor

# ESP32-C3 powered solder fume extractor

Controls a standard **12 V, 4-pin PWM PC fan** through an **NPN transistor**. It reads a numeric PM2.5 sensor entity from Home Assistant, lets you switch between automatic and manual control in a local phone browser, and reports tachometer RPM on that web page.

 A fan with a nonstandard actively driven tach signal may require level shifting.
- The fan receives 12 V directly from its own power supply. Do not connect fan from the ESP.
- Fan and ESP32 grounds must be connected together.
- NPN logic inversion is handled **GPIO3 transistor drive duty = 100 − requested fan PWM %**. At 100% request, NPN is OFF and the fan's PWM input is HIGH.

## PM2.5 automation

| PM2.5 (µg/m³) | PWM command |
|---|---:|
| below 5 | 0% |
| 5 to below 12 | 0% |
| 12 to below 35 | 0% |
| 35 to below 75 | 20% |
| 75 and above | 85% |

- Recommended to change based on your own fan values

The sketch reads HA via the [Home Assistant REST API](https://developers.home-assistant.io/docs/api/rest/), normally every **2 seconds**. It parses the sensor's `state` as a number. The same reading at successive successful polls is **still considered fresh**. In Auto mode, unknown values, connection failures, Wi-Fi outages or a reading older than **90 seconds** command 100% fan speed. Manual mode can still control the fan when Home Assistant is offline, as long as you're on the controller's LAN.

Tachometer uses falling-edge interrupts, normally **2 pulses per revolution**, with RPM calculated every **5 seconds**. Change `TACH_PULSES_PER_REV` if the fan's specifications differ.

## Security and Home Assistant differences

- **Keep the device on a trusted local network. Do not port-forward it.**
- This simple sketch talks to Home Assistant via **HTTP** so it can work with default local installs. **The long-lived token goes over the network without transport encryption** on HTTP. Protect the Wi-Fi network and Home Assistant token. For better security you can use HTTPS with proper certificate verification, which requires adapting the HTTP client code, or use MQTT with credentials/TLS.


- AN NPN transistor is not always needed, but is generally the safer option.
