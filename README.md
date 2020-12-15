# MQTTSwitch

MQTTSwitch implements a power switch which is controlled by either a momentary
switch button, a MQTT topic, or through a Bluetooth serial command line
interface.

The code is an Arduino sketch written for ESP32/Node32s, but it should easily
adapt to other boards. The original use case is to use home automation tools
to control non-smart devices with a relay.

## Setup

1. Press and hold the power button for at least 5 seconds, then release. This
   temporarily enables Bluetooth.
2. Connect your smartphone to the Bluetooth device, initially named
   `MQTTSwitch`. (The device can be renamed using the `device-name` command).
3. Open a Bluetooth serial emulator on your smartphone.
4. Enter `setup`. Follow the prompts to configure WiFi and MQTT.

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
