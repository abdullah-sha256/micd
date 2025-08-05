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
- [`MQTTClient`](http://os.mbed.com/teams/mqtt/code/MQTT/) 
- Audio drivers for `stm32l475e_iot01_audio.h`  
- [`FILTER_LIB.h`](http://os.mbed.com/teams/Project_WIPV_antiSlip/code/FILTER_LIB/) for high-pass filtering  
- [wifi-ism43362](https://github.com/ARMmbed/wifi-ism43362.git) 
- [IIR](http://os.mbed.com/teams/LDSC_Robotics_TAs/code/IIR/)

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

Allow anonymous access in the config file `~/.mosquitto/mosquitto.conf` using the following configuration:

```conf
listener 1883
allow_anonymous true
```

Run the broker:

```bash
 mosquitto -c ~/.mosquitto/mosquitto.conf
 ```

---

### 3. Set Broker IP

In `main.cpp`, replace the IP with your computer’s local IP address:

```cpp
    broker.set_ip_address("192.168.2.138"); // replace the IP with broker's ip
```

If the board complains about MQTT connection, ensure your broker is accessible in your network (i.e disable firewall, try to connect to broker through another device

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
the provided implementation under `./Patch/`
