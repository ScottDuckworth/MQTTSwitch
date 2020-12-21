# MQTTSwitch

MQTTSwitch implements a power switch which is controlled by either a momentary
switch button, a MQTT topic, or through a Bluetooth serial command line
interface.

The code is an Arduino sketch written for ESP32/Node32s, but it should easily
adapt to other boards. The original use case is to use home automation tools
to control non-smart devices with a relay.

## Setup

Press and hold the power button for at least 5 seconds, then release. This
temporarily enables setup mode (it turns on Bluetooth, a WiFi access point,
and a web server).

There are two ways to setup the device: via WiFi or via Bluetooth.

### Setup via WiFi

1. Connect a WiFi device to the SSID named `MQTTSwitch`. (The device can be
renamed during setup).
2. Open a web browser and navigate to http://192.168.4.1/.
3. Click the link to `Setup` and fill out the form to configure WiFi and MQTT.

### Setup via Bluetooth 

Caveat: This method doesn't seem to work on Apple's iOS.

1. Connect your smartphone to the Bluetooth device, initially named
   `MQTTSwitch`. (The device can be renamed using the `device-name` command).
2. Open a Bluetooth serial emulator on your smartphone.
3. Enter `setup`. Follow the prompts to configure WiFi and MQTT.

## MQTT Topics

Two MQTT topics are used:

* Control topic: MQTTSwitch subscribes to this topic. Message payloads that
  match `ON`, `TRUE`, or `1` will turn *on* the power switch; message payloads
  that match `OFF`, `FALSE`, or `0` will turn *off* the power switch.
* Status topic: MQTTSwitch publishes its power status to this topic - either
  `ON`, `OFF`, or `OFFLINE`. The `OFFLINE` status is set by an MQTT will in
  the case that the device goes offline.

## Design Goals

* Pressing the physical power button should *always* instantly toggle the
  power. Reporting power status and responding to MQTT messages comes second.
  The main loop can be delayed for various reasons, e.g. network latency of
  the MQTT client, so it is not used for responding to the power button.
  Therefore, interrupts are used to respond to the power button.
* Since the device can be controlled through Bluetooth, and Bluetooth is
  probably subject to buffer overflows and such, it is not safe to leave on
  Bluetooth at all times. Therefore, Bluetooth is disabled by default, and it
  will be disabled 10 minutes after the last client disconnects. You must have
  physical access to the power button to enable Bluetooth.
* The device is intended to be maintenance free and left on for many years.
  The primary means of time tracking in Arduino reports milliseconds since
  boot as an unsigned 32-bit integer (on ESP32), which will overflow every 49
  days. This can cause simple timestamp comparisons to break, but taking the
  *difference* of two timestamps will work as expected. Care has been taken to
  ensure that timestamps are handled correctly.

## Uploading to an ESP32

The compiled code occupies a little over 1.3MB of program storage space. This
is just enough to exceed the default partition size for the typical ESP32 with
4MB of flash, so the partition scheme must be changed. Additionally, the code
does use a small amount of flash for SPIFFS, which must be uploaded separately
from the code, and must be accounted for in the flash partitioning.

If the code will not fit in your board, try this:

1. Select board `ESP32 Dev Module`.
2. Select partition scheme `Minimal SPIFFS`, which provides 1.9MB APP storage
   and 190KB SPIFFS storage.

To upload the required data to SPIFFS storage, you must first install the
<a href="https://github.com/me-no-dev/arduino-esp32fs-plugin">Arduino ESP32
filesystem uploader</a> plugin. Upload the data through the `Tools -> ESP32
Sketch Data Upload` menu item.
