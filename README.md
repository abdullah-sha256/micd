# ResonanceRGB

ResonanceRGB is an LED visualizer that listens and reacts to the sound around it.
The onboard microphone on the discovery board will be used to sample ambient
audio, compute real time amplitude envelopes, and drive an addressable LED strip
with dynamic colour and pattern effects. Additionally, the system will also log the
average dB levels to the cloud and offer a threshold flash effect for high volume
sounds.

# Notes

This project runs on a shared Mbed OS instance


## Dependencies

- Mbed OS (Tested on 6.x)  
- `MQTTClient` 
- Audio drivers for `stm32l475e_iot01_audio.h`  
- `FILTER_LIB.h` for high-pass filtering  



---

## Setup Guide

### 1. Update Wi-Fi Credentials

Open `main.cpp` and **replace** the SSID and password in the following line:

```cpp
wifi->connect("YOUR_SSID", "YOUR_PASSWORD", NSAPI_SECURITY_WPA_WPA2);
```

Also, make sure your mbed_app.json contains:

```js
{
  "target_overrides": {
    "*": {
      "target.components_add": ["ISM43362"],
      "nsapi.default-wifi-security": "WPA_WPA2",
      "nsapi.default-wifi-ssid": "\"YOUR_SSID\"",
      "nsapi.default-wifi-password": "\"YOUR_PASSWORD\"",
      "platform.stdio-baud-rate": 115200,
      "target.printf_lib": "std"
    }
  }
}
```

---

### 2. Start Local MQTT Broker

Install and start Mosquitto:

```bash
brew install mosquitto
```

Allow anonymous access with a config like `local.conf`:

```conf
listener 1883
allow_anonymous true
```

Run the broker:

```bash
mosquitto -c local.conf
```

---

### 3. Set Broker IP

In `main.cpp`, replace the IP with your computer’s local IP address:

```cpp
wifi->gethostbyname("192.168.x.xxx", &broker);
```

## MQTT Payload Format

Publishes every \~30s to the topic `sound/volume`:

```json
{
  "avg_decibel": -31.4,
  "min_decibel": -60.0,
  "max_decibel": -10.3
}
```

### 4. Update MQTTClient.h with the provided implementation (IMPORTANT)

Since MQTTClient library was written for MbedOS 5.x, the "write" and "read" methods were 
replaced with `send` and `recv`, please replace `MQTTClient.h` in the `MQTT` library with 
the provided implementation on root folder. 
