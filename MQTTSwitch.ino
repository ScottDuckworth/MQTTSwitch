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
#include <DNSServer.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <WiFi.h>

#define BUTTON_PIN 15
#define POWER_PIN 4
#define STATUS_LED 2
#define SETTINGS_HEADER 0xD3
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
  char mqtt_password[32] = "";
  char mqtt_control_topic[32] = "";
  char mqtt_status_topic[32] = "";
};

Settings settings;
BluetoothSerial bt;
WiFiClient wifi_client;
PubSubClient mqtt(wifi_client);
WebServer web_server;
DNSServer dns_server;
volatile bool power_enable = false;
volatile bool config_enable = false;
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
  bt.println(" setup");
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

bool EnsureWiFi(const Settings& mysettings = settings) {
  static wl_status_t last_status = WL_NO_SHIELD;
  wl_status_t status = WiFi.status();
  if (status != last_status) {
    last_status = status;
    Serial.print("WiFi state changed to ");
    Serial.print(wifiStatusString(status));
    Serial.println(".");
  }
  if (status == WL_CONNECTED) return true;
  if (strlen(mysettings.wifi_ssid) == 0) return false;
  // Retry connecting every 60 seconds.
  unsigned long now = millis();
  if (wifi_last_try == 0 || now - wifi_last_try >= 60000) {
    wifi_last_try = now;
    Serial.print("Connecting to WiFi SSID ");
    Serial.print(mysettings.wifi_ssid);
    Serial.println(".");
    WiFi.setHostname(mysettings.device_name);
    WiFi.begin(mysettings.wifi_ssid, mysettings.wifi_password);
  }
  return false;
}

bool EnsureMQTT(const Settings& mysettings = settings) {
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
  if (strlen(mysettings.mqtt_server) == 0) return false;
  // Retry connecting every 60 seconds.
  unsigned long now = millis();
  if (mqtt_last_try == 0 || now - mqtt_last_try >= 60000) {
    mqtt_last_try = now;
    Serial.print("Connecting to MQTT server ");
    Serial.print(mysettings.mqtt_server);
    Serial.println(".");
    mqtt.setServer(mysettings.mqtt_server, mysettings.mqtt_port);
    const char* id = mysettings.mqtt_id;
    if (strlen(id) == 0) id = mysettings.device_name;
    const char* user = mysettings.mqtt_user;
    if (strlen(user) == 0) user = nullptr;
    const char* password = mysettings.mqtt_password;
    if (strlen(password) == 0) password = nullptr;
    const char* status_topic = mysettings.mqtt_status_topic;
    if (strlen(status_topic) == 0) status_topic = nullptr;
    bool success = mqtt.connect(id, user, password, status_topic, 1, true, "OFFLINE");
    if (success && strlen(mysettings.mqtt_control_topic) > 0) {
      mqtt.subscribe(mysettings.mqtt_control_topic, 1);
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

void StrStrip(char* str) {
  char* in = str;
  char* out = str;
  while (*in && isspace(*in)) ++in;
  while (*in) *out++ = *in++;
  do {
    *out-- = '\0';
  } while (out >= str && isspace(*out));
}

bool BluetoothReadLine(char* line, size_t linesize, bool strip = true) {
  size_t i = 0;
  int c;
  memset(line, 0, linesize);
  while (bt.hasClient()) {
    int c = bt.read();
    if (c < 0) {
      yield();
    } else if (c == '\n') {
      break;
    } else if (i < linesize - 1) {
      line[i++] = c;
    }
  }
  if (strip) StrStrip(line);
  return i > 0;
}

bool Setup() {
  Settings mysettings = settings;
  RestartWiFi();

  bt.println("WiFi SSID:");
  BluetoothReadLine(mysettings.wifi_ssid, sizeof(mysettings.wifi_ssid));
  bt.println("WiFi password:");
  BluetoothReadLine(mysettings.wifi_password, sizeof(mysettings.wifi_password));

  if (!bt.hasClient()) return false;
  bt.print("Connecting to WiFi... ");
  EnsureWiFi(mysettings);
  unsigned long start = millis();
  while (true) {
    if (!bt.hasClient()) return false;
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) break;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      bt.println("Connection failed!");
      return false;
    }
    if (millis() - start >= 30000) {
      bt.println("Timed out!");
      return false;
    }
    yield();
  }
  bt.println("Connected!");

  bt.println("MQTT server:");
  BluetoothReadLine(mysettings.mqtt_server, sizeof(mysettings.mqtt_server));
  bt.println("MQTT user:");
  BluetoothReadLine(mysettings.mqtt_user, sizeof(mysettings.mqtt_user));
  if (strlen(mysettings.mqtt_user) > 0) {
    bt.println("MQTT password:");
    BluetoothReadLine(mysettings.mqtt_password, sizeof(mysettings.mqtt_password));
  }

  if (!bt.hasClient()) return false;
  bt.print("Connecting to MQTT server... ");
  EnsureMQTT(mysettings);
  start = millis();
  while (true) {
    if (!bt.hasClient()) return false;
    mqtt.loop();
    int state = mqtt.state();
    if (state == MQTT_CONNECTED) break;
    if (state == MQTT_CONNECT_FAILED || state == MQTT_CONNECT_UNAUTHORIZED) {
      bt.println("Connection failed!");
      return false;
    }
    if (millis() - start >= 30000) {
      bt.println("Timed out!");
      return false;
    }
    yield();
  }
  bt.println("Connected!");

  bt.println("MQTT control topic:");
  BluetoothReadLine(mysettings.mqtt_control_topic, sizeof(mysettings.mqtt_control_topic));
  bt.println("MQTT status topic:");
  BluetoothReadLine(mysettings.mqtt_status_topic, sizeof(mysettings.mqtt_status_topic));

  if (!bt.hasClient()) return false;
  settings = mysettings;
  SaveSettings();
  bt.println("Settings saved!");
  RestartMQTT();
  return true;
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
    } else if (strcmp(cmd, "setup") == 0) {
      if (!Setup()) {
        bt.println("Setup failed! Try again.");
      }
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
        // Toggle config mode.
        config_enable = !config_enable;
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

String htmlEncode(String input) {
    String output;
    output.reserve(input.length());
    for(size_t pos = 0; pos != input.length(); ++pos) {
        switch(input[pos]) {
            case '&':  output += "&amp;";    break;
            case '\"': output += "&quot;";   break;
            case '\'': output += "&apos;";   break;
            case '<':  output += "&lt;";     break;
            case '>':  output += "&gt;";     break;
            default:   output += input[pos]; break;
        }
    }
    return output;
}

String LoadTemplate(const char* path) {
  File f = SPIFFS.open(path);
  if (!f) {
    Serial.print("Error: failed to open file: ");
    Serial.println(path);
    return "";
  }
  
  String content;
  while (f.available()) {
    content += static_cast<char>(f.read());
  }
  f.close();
  
  content.replace("%power_status%", power_enable ? "ON" : "OFF");
  content.replace("%wifi_status%", wifiStatusString(WiFi.status()));
  content.replace("%mqtt_status%", mqttStateString(mqtt.state()));
  content.replace("%device_name%", htmlEncode(settings.device_name));
  content.replace("%wifi_ssid%", htmlEncode(settings.wifi_ssid));
  content.replace("%wifi_password%", htmlEncode(settings.wifi_password));
  content.replace("%mqtt_server%", htmlEncode(settings.mqtt_server));
  content.replace("%mqtt_port%", String(settings.mqtt_port));
  content.replace("%mqtt_id%", htmlEncode(settings.mqtt_id));
  content.replace("%mqtt_user%", htmlEncode(settings.mqtt_user));
  content.replace("%mqtt_password%", htmlEncode(settings.mqtt_password));
  content.replace("%mqtt_control_topic%", htmlEncode(settings.mqtt_control_topic));
  content.replace("%mqtt_status_topic%", htmlEncode(settings.mqtt_status_topic));
  return content;
}

void httpGETIndex() {
  web_server.sendHeader("Connection", "close");
  web_server.send(200, "text/html", LoadTemplate("/index.html"));
}

void httpGETSettings() {
  web_server.sendHeader("Connection", "close");
  if (web_server.arg("success") == "") {
    web_server.send(200, "text/html", LoadTemplate("/settings.html"));
  } else {
    web_server.send(200, "text/html", LoadTemplate("/settings-saved.html"));
  }
}

void httpPOSTSettings() {
  Settings mysettings;
  strncpy(mysettings.device_name, web_server.arg("device_name").c_str(), sizeof(mysettings.device_name));
  strncpy(mysettings.wifi_ssid, web_server.arg("wifi_ssid").c_str(), sizeof(mysettings.wifi_ssid));
  strncpy(mysettings.wifi_password, web_server.arg("wifi_password").c_str(), sizeof(mysettings.wifi_password));
  strncpy(mysettings.mqtt_server, web_server.arg("mqtt_server").c_str(), sizeof(mysettings.mqtt_server));
  mysettings.mqtt_port = atoi(web_server.arg("mqtt_port").c_str());
  strncpy(mysettings.mqtt_id, web_server.arg("mqtt_id").c_str(), sizeof(mysettings.mqtt_id));
  strncpy(mysettings.mqtt_user, web_server.arg("mqtt_user").c_str(), sizeof(mysettings.mqtt_user));
  strncpy(mysettings.mqtt_password, web_server.arg("mqtt_password").c_str(), sizeof(mysettings.mqtt_password));
  strncpy(mysettings.mqtt_control_topic, web_server.arg("mqtt_control_topic").c_str(), sizeof(mysettings.mqtt_control_topic));
  strncpy(mysettings.mqtt_status_topic, web_server.arg("mqtt_status_topic").c_str(), sizeof(mysettings.mqtt_status_topic));
  settings = mysettings;
  SaveSettings();

  web_server.sendHeader("Connection", "close");
  web_server.sendHeader("Location", "/settings?success=1");
  web_server.send(303, "text/plain", "Settings saved!");
}

void loop() {
  static unsigned long blink_millis = 0;
  static unsigned long blink_period = 0;
  static unsigned long bluetooth_disconnected_millis = 0;
  static bool last_power_enable = power_enable;
  static bool last_config_enable = config_enable;
  static bool last_bluetooth_has_client = false;

  if (config_enable != last_config_enable) {
    last_config_enable = config_enable;
    if (config_enable) {
      bt.begin(settings.device_name);
      Serial.print("Bluetooth enabled; name: ");
      Serial.print(settings.device_name);
      Serial.println(".");
      WiFi.softAP(settings.device_name);
      WiFi.softAPConfig(WiFi.softAPIP(), WiFi.softAPIP(), IPAddress(255, 255, 255, 0));
      web_server.begin();
      dns_server.start(53, "*", WiFi.softAPIP());
      Serial.print("WiFi AP enabled; name: ");
      Serial.print(settings.device_name);
      Serial.print(", IP: ");
      Serial.println(WiFi.softAPIP());
    } else {
      bt.end();
      dns_server.stop();
      web_server.stop();
      WiFi.softAPdisconnect();
      RestartWiFi();
      Serial.println("Bluetooth, web server, and WiFi AP disabled.");
    }
  }

  if (config_enable) {
    dns_server.processNextRequest();
    web_server.handleClient();
    PollBluetoothCommand();
    bool bluetooth_has_client = bt.hasClient();
    if (bluetooth_has_client != last_bluetooth_has_client) {
      last_bluetooth_has_client = bluetooth_has_client;
      if (bluetooth_has_client) {
        Serial.println("Bluetooth client connected.");
        PrintHelp();
      } else {
        bluetooth_disconnected_millis = millis();
        Serial.println("Bluetooth client disconnected.");
      }
    }
    // Disable bluetooth after 10 minutes.
    if (!bluetooth_has_client && millis() - bluetooth_disconnected_millis >= 600000) {
      config_enable = false;
      Serial.println("Config timeout.");
    }
  }

  if (!EnsureWiFi()) {
    // Blink 4 times per second if WiFi is not connected.
    blink_period = 250;
  } else if (!EnsureMQTT()) {
    // Blink 1 time per second if MQTT is not connected.
    blink_period = 1000;
  } else {
    // Blink every 10 seconds if WiFi and MQTT are connected.
    blink_period = 10000;
    mqtt.loop();
  }

  unsigned long blink_duration = millis() - blink_millis;
  if (blink_duration >= blink_period) {
    digitalWrite(STATUS_LED, 1);
    blink_millis = millis();
  } else if (blink_duration >= 100) {
    digitalWrite(STATUS_LED, 0);
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

  if(!SPIFFS.begin()){
     Serial.println("Error: failed to mount SPIFFS.");
  }
  web_server.on("/", HTTP_GET, httpGETIndex);
  web_server.on("/settings", HTTP_GET, httpGETSettings);
  web_server.on("/settings", HTTP_POST, httpPOSTSettings);
}
