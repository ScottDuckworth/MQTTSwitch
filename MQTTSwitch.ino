// MQTTSwitch implements a power switch which is controlled by either
// a momentary switch button, a MQTT topic, or through a Bluetooth serial
// command line interface.
//
// To use MQTT, a network connection must be configured. The network is
// configured through the Bluetooth serial command line interface. To
// enable Bluetooth, hold down the momentary switch button for 5 seconds.
// Use a Bluetooth serial emulator to control the device through a command
// line interface. Use the "wifi" and "mqtt" commands to configure WiFi
// and MQTT.

#include <algorithm>
#include <limits>

#include <BluetoothSerial.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <WiFi.h>

#define BUTTON_PIN 15
#define POWER_PIN 4
#define STATUS_LED LED_BUILTIN
#define SETTINGS_HEADER 0xD2
#define DEBOUNCE_MILLIS 50

struct Settings {
  uint8_t header = SETTINGS_HEADER;
  char device_name[32] = "MQTTSwitch";
  char wifi_ssid[32] = "";
  char wifi_password[32] = "";
  char mqtt_server[16] = "";
  uint16_t mqtt_port = 1883;
  char mqtt_id[16] = "";
  char mqtt_user[16] = "";
  char mqtt_password[16] = "";
  char mqtt_control_topic[32] = "";
  char mqtt_status_topic[32] = "";
};

Settings settings;
BluetoothSerial bt;
WiFiClient wifi_client;
PubSubClient mqtt(wifi_client);
volatile bool power_enable = false;
volatile bool bluetooth_enable = false;
unsigned long bluetooth_enable_millis;
unsigned long wifi_last_try;
unsigned long mqtt_last_try;

bool SaveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

void PrintHelp() {
  bt.println("Help:");
  bt.println(" help");
  bt.println(" on");
  bt.println(" off");
  bt.println(" status");
  bt.println(" device-name [<name>]");
  bt.println(" mqtt-server [<IPaddr>]");
  bt.println(" mqtt-status");
  bt.println(" mqtt-port [<port>]");
  bt.println(" mqtt-id [<id>]");
  bt.println(" mqtt-auth <user> [<password>]");
  bt.println(" mqtt-control-topic [<topic>]");
  bt.println(" mqtt-status-topic [<topic>]");
  bt.println(" wifi <ssid> [<password>]");
  bt.println(" wifi-status");
}

const char* wifiStatusString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:       return "NO_SHIELD";
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "(unknown)";
  }
}

const char* mqttStateString(int state) {
  switch (state) {
    case MQTT_CONNECTION_TIMEOUT:      return "CONNECTION_TIMEOUT";
    case MQTT_CONNECTION_LOST:         return "CONNECTION_LOST";
    case MQTT_CONNECT_FAILED:          return "CONNECT_FAILED";
    case MQTT_DISCONNECTED:            return "DISCONNECTED";
    case MQTT_CONNECTED:               return "CONNECTED";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "CONNECT_BAD_PROTOCOL";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "CONNECT_BAD_CLIENT_ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "CONNECT_UNAVAILABLE";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "CONNECT_BAD_CREDENTIALS";
    case MQTT_CONNECT_UNAUTHORIZED:    return "CONNECT_UNAUTHORIZED";
    default:                           return "(unknown)";
  }
}

void RestartMQTT() {
  mqtt.disconnect();
  mqtt_last_try = 0;
}

void RestartWiFi() {
  RestartMQTT();
  WiFi.disconnect();
  wifi_last_try = 0;
}

void PublishStatus() {
  if (strlen(settings.mqtt_status_topic) > 0) {
    mqtt.publish(settings.mqtt_status_topic, power_enable ? "ON" : "OFF", true);
  }
}

bool EnsureWiFi() {
  static wl_status_t last_status = WL_NO_SHIELD;
  wl_status_t status = WiFi.status();
  if (status != last_status) {
    last_status = status;
    Serial.print("WiFi state changed to ");
    Serial.print(wifiStatusString(status));
    Serial.println(".");
  }
  if (status == WL_CONNECTED) return true;
  if (strlen(settings.wifi_ssid) == 0) return false;
  // Retry connecting every 60 seconds.
  unsigned long now = millis();
  if (wifi_last_try == 0 || now - wifi_last_try >= 60000) {
    wifi_last_try = now;
    Serial.print("Connecting to WiFi SSID ");
    Serial.print(settings.wifi_ssid);
    Serial.println(".");
    WiFi.setHostname(settings.device_name);
    WiFi.begin(settings.wifi_ssid, settings.wifi_password);
  }
  return false;
}

bool EnsureMQTT() {
  static int last_state = MQTT_DISCONNECTED;
  int state = mqtt.state();
  if (state != last_state) {
    last_state = state;
    Serial.print("MQTT state changed to ");
    Serial.print(mqttStateString(state));
    Serial.println(".");
    if (state == MQTT_CONNECTED) PublishStatus();
  }
  if (state == MQTT_CONNECTED) return true;
  if (strlen(settings.mqtt_server) == 0) return false;
  // Retry connecting every 60 seconds.
  unsigned long now = millis();
  if (mqtt_last_try == 0 || now - mqtt_last_try >= 60000) {
    mqtt_last_try = now;
    Serial.print("Connecting to MQTT server ");
    Serial.print(settings.mqtt_server);
    Serial.println(".");
    mqtt.setServer(settings.mqtt_server, settings.mqtt_port);
    const char* id = settings.mqtt_id;
    if (strlen(id) == 0) id = settings.device_name;
    bool success;
    if (strlen(settings.mqtt_user) == 0) {
      success = mqtt.connect(id, settings.mqtt_status_topic, 1, true, "OFFLINE");
    } else {
      success = mqtt.connect(id, settings.mqtt_user, settings.mqtt_password,
                             settings.mqtt_status_topic, 1, true, "OFFLINE");
    }
    if (success && strlen(settings.mqtt_control_topic) > 0) {
      mqtt.subscribe(settings.mqtt_control_topic, 1);
    }
    return success;
  }
  return false;
}

char* tokenSplit(char*& ptr) {
  if (ptr == nullptr) return nullptr;
  while (*ptr && isspace(*ptr)) ++ptr;
  if (!*ptr) return nullptr;
  char* token = ptr;
  while (*++ptr && !isspace(*ptr));
  if (*ptr) {
    *ptr++ = '\0';
  } else {
    ptr = nullptr;
  }
  return token;
}

// Read or write a single string settings value.
// Returns true iff settings were changed.
bool SettingsReadWriteHelper(char* settings_value, size_t size, const char* new_value) {
  if (new_value) {
    memset(settings_value, 0, size);
    if (strcmp(new_value, "-") != 0) {  // Interpret "-" as blank.
      strncpy(settings_value, new_value, size - 1);
    }
    SaveSettings();
    bt.println("Settings saved.");
    return true;
  } else {
    bt.println(settings_value);
    return false;
  }
}

void PollBluetoothCommand() {
  if (bt.available()) {
    String input = bt.readStringUntil('\n');
    char buf[input.length() + 1];
    strcpy(buf, input.c_str());
    char* ptr = buf;
    char* cmd = tokenSplit(ptr);
    if (strcmp(cmd, "help") == 0) {
      PrintHelp();
    } else if (strcmp(cmd, "on") == 0) {
      power_enable = true;
    } else if (strcmp(cmd, "off") == 0) {
      power_enable = false;
    } else if (strcmp(cmd, "status") == 0) {
      bt.println(power_enable ? "on" : "off");
    } else if (strcmp(cmd, "device-name") == 0) {
      char* name = tokenSplit(ptr);
      if (SettingsReadWriteHelper(settings.device_name, sizeof(settings.device_name), name)) {
        RestartWiFi();
      }
    } else if (strcmp(cmd, "mqtt-status") == 0) {
      bt.println(mqttStateString(mqtt.state()));
    } else if (strcmp(cmd, "mqtt-server") == 0) {
      char* host = tokenSplit(ptr);
      if (SettingsReadWriteHelper(settings.mqtt_server, sizeof(settings.mqtt_server), host)) {
        RestartMQTT();
      }
    } else if (strcmp(cmd, "mqtt-port") == 0) {
      char* port = tokenSplit(ptr);
      if (port) {
        int p = atoi(port);
        if (p < 1 || p > std::numeric_limits<uint16_t>::max()) {
          bt.println("Error: invalid port number");
          return;
        }
        settings.mqtt_port = p;
        SaveSettings();
        bt.println("Settings saved!");
        RestartMQTT();
      } else {
        bt.println(settings.mqtt_port);
      }
    } else if (strcmp(cmd, "mqtt-id") == 0) {
      char* id = tokenSplit(ptr);
      if (SettingsReadWriteHelper(settings.mqtt_id, sizeof(settings.mqtt_id), id)) {
        RestartMQTT();
      }
    } else if (strcmp(cmd, "mqtt-auth") == 0) {
      char* user = tokenSplit(ptr);
      if (user == nullptr) {
        bt.println("Error: must specify user");
        return;
      }
      char* password = tokenSplit(ptr);
      memset(&settings.mqtt_user, 0, sizeof(settings.mqtt_user));
      memset(&settings.mqtt_password, 0, sizeof(settings.mqtt_password));
      strncpy(settings.mqtt_user, user, sizeof(settings.mqtt_user) - 1);
      if (password) {
        strncpy(settings.mqtt_password, password, sizeof(settings.mqtt_password) - 1);
      }
      SaveSettings();
      bt.println("Settings saved!");
      RestartMQTT();
    } else if (strcmp(cmd, "mqtt-control-topic") == 0) {
      char* topic = tokenSplit(ptr);
      if (SettingsReadWriteHelper(settings.mqtt_control_topic, sizeof(settings.mqtt_control_topic), topic)) {
        mqtt.disconnect();
      }
    } else if (strcmp(cmd, "mqtt-status-topic") == 0) {
      char* topic = tokenSplit(ptr);
      if (SettingsReadWriteHelper(settings.mqtt_status_topic, sizeof(settings.mqtt_status_topic), topic)) {
        RestartMQTT();
      }
    } else if (strcmp(cmd, "wifi") == 0) {
      char* ssid = tokenSplit(ptr);
      if (ssid == nullptr) {
        bt.println("Error: must specify SSID");
        return;
      }
      char* password = tokenSplit(ptr);
      memset(&settings.wifi_ssid, 0, sizeof(settings.wifi_ssid));
      memset(&settings.wifi_password, 0, sizeof(settings.wifi_password));
      strncpy(settings.wifi_ssid, ssid, sizeof(settings.wifi_ssid) - 1);
      if (password) {
        strncpy(settings.wifi_password, password, sizeof(settings.wifi_password) - 1);
      }
      SaveSettings();
      bt.println("Settings saved!");
      RestartWiFi();
    } else if (strcmp(cmd, "wifi-status") == 0) {
      int mode = WiFi.getMode();
      bt.print("Mode: ");
      bt.println(mode);
      if (mode == WIFI_OFF) return;
      wl_status_t status = WiFi.status();
      bt.print("Status: ");
      bt.println(wifiStatusString(status));
      if (status != WL_CONNECTED) return;
      bt.print("SSID: ");
      bt.println(WiFi.SSID());
      bt.print("BSSID: ");
      bt.println(WiFi.BSSIDstr());
      bt.print("RSSI: ");
      bt.println(WiFi.RSSI());
      bt.print("IP: ");
      bt.println(WiFi.localIP());
    } else {
      bt.println("Error: command not found");
      return;
    }
  }
}

void ButtonChangeISR() {
  static volatile bool button_press = false;
  static volatile unsigned long button_press_millis = 0;
  unsigned long now = millis();
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!button_press) {
      button_press = true;
      button_press_millis = now;
    }
  } else {
    if (button_press) {
      button_press = false;
      unsigned long hold_millis = now - button_press_millis;
      if (hold_millis >= 5000) {
        // Toggle Bluetooth.
        bluetooth_enable = !bluetooth_enable;
      } else if (hold_millis >= DEBOUNCE_MILLIS) {
        // Toggle power.
        power_enable = !power_enable;
        digitalWrite(POWER_PIN, power_enable);
      }
    }
  }
}

void MQTTCallback(char* topic, byte* payload, unsigned int length) {
  if (strncmp(topic, settings.mqtt_control_topic, sizeof(settings.mqtt_control_topic)) == 0) {
    char str[8] = {0};
    memcpy(str, payload, std::min(length, sizeof(str) - 1));
    if (strcasecmp(str, "on") == 0 ||
        strcasecmp(str, "true") == 0 ||
        strcmp(str, "1") == 0) {
      power_enable = true;
    } else if (strcasecmp(str, "off") == 0 ||
               strcasecmp(str, "false") == 0 ||
               strcmp(str, "0") == 0) {
      power_enable = false;
    }
  }
}

void loop() {
  static unsigned long last_blink = 0;
  static unsigned long blink_delay = 0;
  static bool last_power_enable = power_enable;
  static bool last_bluetooth_enable = bluetooth_enable;
  static bool last_bluetooth_has_client = false;

  if (bluetooth_enable != last_bluetooth_enable) {
    last_bluetooth_enable = bluetooth_enable;
    if (bluetooth_enable) {
      bluetooth_enable_millis = millis();
      bt.begin(settings.device_name);
      Serial.print("Bluetooth enabled; name: ");
      Serial.print(settings.device_name);
      Serial.println(".");
    } else {
      bt.end();
      Serial.println("Bluetooth disabled.");
    }
  }
  
  if (bluetooth_enable) {
    PollBluetoothCommand();
    bool bluetooth_has_client = bt.hasClient();
    if (bluetooth_has_client != last_bluetooth_has_client) {
      last_bluetooth_has_client = bluetooth_has_client;
      if (bluetooth_has_client) {
        Serial.println("Bluetooth client connected.");
        PrintHelp();
      } else {
        Serial.println("Bluetooth client disconnected.");
      }
    }
    // Disable bluetooth after 10 minutes.
    if (!bluetooth_has_client && millis() - bluetooth_enable_millis >= 600000) {
      bluetooth_enable = false;
      Serial.println("Bluetooth timeout.");
    }
  }
  
  if (EnsureWiFi()) {
    if (EnsureMQTT()) {
      mqtt.loop();
      blink_delay = 5000;
    } else {
      blink_delay = 1000;
    }
  } else {
    blink_delay = 250;
  }

  if (blink_delay == 0) {
    digitalWrite(STATUS_LED, true);
  } else if (millis() - last_blink >= blink_delay) {
    last_blink = millis();
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
  }
  
  if (power_enable != last_power_enable) {
    last_power_enable = power_enable;
    digitalWrite(POWER_PIN, power_enable);
    Serial.println(power_enable ? "Power on." : "Power off.");
    PublishStatus();
  }
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(POWER_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), ButtonChangeISR, CHANGE);
  
  Serial.begin(115200);
  
  Settings s;
  EEPROM.begin(sizeof(s));
  EEPROM.get(0, s);
  if (s.header == SETTINGS_HEADER) {
    Serial.println("Loaded valid settings.");
    settings = s;
  } else {
    Serial.println("Found invalid settings header; settings reset.");
  }

    
  // Set a short timeout because this is a blocking MQTT client.
  // I've had endless troubles with the async MQTT client found at
  // https://github.com/marvinroger/async-mqtt-client :-(.
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(MQTTCallback);
}
