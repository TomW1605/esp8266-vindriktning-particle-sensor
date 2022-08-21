#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
//#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

#include "Config.h"
#include "SerialCom.h"
#include "Types.h"

particleSensorState_t state;

uint8_t mqttRetryCounter = 0;

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", Config::mqtt_server, sizeof(Config::mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", Config::username, sizeof(Config::username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", Config::password, sizeof(Config::password));

uint32_t lastMqttConnectionAttempt = 0;
const uint16_t mqttConnectionInterval = 60000; // 1 minute = 60 seconds = 60000 milliseconds

uint32_t statusPublishPreviousMillis = 0;
const uint16_t statusPublishInterval = 30000; // 30 seconds = 30000 milliseconds

char identifier[24];
#define FIRMWARE_PREFIX "esp8266_vindriktning_particle_sensor"
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];

char MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[128];
char MQTT_TOPIC_AUTOCONF_PM25_SENSOR[128];

bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

//ESP8266WebServer server(80);   //Web server object. Will be listening in port 80 (default for HTTP)

void setupCallbacks() {
  wifiManager.server->on("/state", []() {
    String message = "<meta http-equiv=\"refresh\" content=\"10\" />"\
                     "<style>body {background-color: black;color: white;}</style>"\
                     "<div><p style=\"text-align:center;\">"\
                     "WiFi Network: " + String(WiFi.SSID()) + "<br>"\
                     "IP Address: " + WiFi.localIP().toString() + "<br>"\
                     "MAC Address: " + String(WiFi.macAddress()) + "<br>"\
                     "RSSI: " + String(WiFi.RSSI()) + " dBm<br>"\
                     "MQTT Server: " + String(Config::mqtt_server) + "<br>"\
                     "MQTT Connected: " + (isMqttConnected() ? "True" : "False") + "<br>"\
                     "PM 2.5 reading: " + String(state.avgPM25) + " &micro;g/m<sup>3</sup><br>";
    wifiManager.server->send(200, "text/html", message);
  });

  /*server.onNotFound([]() {
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++) {
      message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
    });*/
}

void setup() {
  Serial.begin(115200);
  SerialCom::setup();

  //wifiManager.resetSettings();

  Serial.println("\n");
  Serial.println("Hello from esp8266-vindriktning-particle-sensor");
  Serial.printf("Core Version: %s\n", ESP.getCoreVersion().c_str());
  Serial.printf("Boot Version: %u\n", ESP.getBootVersion());
  Serial.printf("Boot Mode: %u\n", ESP.getBootMode());
  Serial.printf("CPU Frequency: %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());

  delay(3000);

  snprintf(identifier, sizeof(identifier), "VINDRIKTNING-%X", ESP.getChipId());
  snprintf(MQTT_TOPIC_AVAILABILITY, 127, "%s/%s/status", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_STATE, 127, "%s/%s/state", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/command", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_PM25_SENSOR, 127, "homeassistant/sensor/%s/%s_pm25/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, 127, "homeassistant/sensor/%s/%s_wifi/config", FIRMWARE_PREFIX, identifier);

  WiFi.hostname(identifier);

  Config::load();

  setupWifi();
  setupOTA();
  mqttClient.setServer(Config::mqtt_server, 1883);
  mqttClient.setKeepAlive(10);
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(mqttCallback);

  //setupCallbacks();

  //server.begin();
  Serial.println("HTTP server started");
  Serial.print("local ip: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.printf("Hostname: %s\n", identifier);
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

  Serial.println("-- Current GPIO Configuration --");
  Serial.printf("PIN_UART_RX: %d\n", SerialCom::PIN_UART_RX);

  mqttReconnect();
}

void setupOTA() {
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.setHostname(identifier);

  // This is less of a security measure and more a accidential flash prevention
  ArduinoOTA.setPassword("flash");
  ArduinoOTA.begin();
}

void loop() {
  //server.handleClient();
  ArduinoOTA.handle();
  SerialCom::handleUart(state);
  mqttClient.loop();
  wifiManager.process();

  const uint32_t currentMillis = millis();
  if (currentMillis - statusPublishPreviousMillis >= statusPublishInterval) {
    statusPublishPreviousMillis = currentMillis;

    if (state.valid) {
      printf("Publish state\n");
      publishState();
    }
  }

  if (!mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval) {
    lastMqttConnectionAttempt = currentMillis;
    printf("Reconnect mqtt\n");
    mqttReconnect();
  }
}

void setupWifi() {
  wifiManager.setClass("invert");
  wifiManager.setDebugOutput(true); \
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.setWebServerCallback(setupCallbacks);
  std::vector<const char *> menu = {"wifi", "wifinoscan", "info", "param", "custom", "close", "sep", "erase", "update", "restart", "exit"};
  wifiManager.setMenu(menu); // custom menu, pass vector

  const char* menuhtml = "<form action='/state' method='get'><button>State</button></form><br/>\n";
  wifiManager.setCustomMenuHTML(menuhtml);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);

  WiFi.hostname(identifier);
  if (wifiManager.autoConnect(identifier)) {
    Serial.println("connected...yeey :)");
    WiFi.mode(WIFI_STA);
    wifiManager.startWebPortal();
  }
  else {
    Serial.println("Configportal running");
  }
  mqttClient.setClient(wifiClient);

  custom_mqtt_server.setValue(Config::mqtt_server, 80);
  custom_mqtt_user.setValue(Config::username, 24);
  custom_mqtt_pass.setValue(Config::password, 24);

  strcpy(Config::mqtt_server, custom_mqtt_server.getValue());
  strcpy(Config::username, custom_mqtt_user.getValue());
  strcpy(Config::password, custom_mqtt_pass.getValue());

  if (shouldSaveConfig) {
    Config::save();
  } else {
    // For some reason, the read values get overwritten in this function
    // To combat this, we just reload the config
    // This is most likely a logic error which could be fixed otherwise
    Config::load();
  }
}

void resetWifiSettingsAndReboot() {
  wifiManager.resetSettings();
  delay(3000);
  ESP.restart();
}

void mqttReconnect() {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (mqttClient.connect(identifier, Config::username, Config::password, MQTT_TOPIC_AVAILABILITY, 1, true, AVAILABILITY_OFFLINE)) {
      mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, true);
      publishAutoConfig();

      // Make sure to subscribe after polling the status so that we never execute commands with the default data
      mqttClient.subscribe(MQTT_TOPIC_COMMAND);
      break;
    }
    delay(5000);
  }
}

bool isMqttConnected() {
  return mqttClient.connected();
}

void publishState() {
  DynamicJsonDocument wifiJson(192);
  DynamicJsonDocument stateJson(604);
  char payload[256];

  wifiJson["ssid"] = WiFi.SSID();
  wifiJson["ip"] = WiFi.localIP().toString();
  wifiJson["rssi"] = WiFi.RSSI();

  stateJson["pm25"] = state.avgPM25;

  stateJson["wifi"] = wifiJson.as<JsonObject>();

  serializeJson(stateJson, payload);
  mqttClient.publish(&MQTT_TOPIC_STATE[0], &payload[0], true);
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) { }

void publishAutoConfig() {
  char mqttPayload[2048];
  DynamicJsonDocument device(256);
  DynamicJsonDocument autoconfPayload(1024);
  StaticJsonDocument<64> identifiersDoc;
  JsonArray identifiers = identifiersDoc.to<JsonArray>();

  identifiers.add(identifier);

  device["identifiers"] = identifiers;
  device["manufacturer"] = "Ikea";
  device["model"] = "VINDRIKTNING";
  device["name"] = identifier;
  device["sw_version"] = "2021.08.0";

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = identifier + String(" WiFi");
  autoconfPayload["value_template"] = "{{value_json.wifi.rssi}}";
  autoconfPayload["unique_id"] = identifier + String("_wifi");
  autoconfPayload["unit_of_measurement"] = "dBm";
  autoconfPayload["json_attributes_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["json_attributes_template"] = "{\"ssid\": \"{{value_json.wifi.ssid}}\", \"ip\": \"{{value_json.wifi.ip}}\"}";
  autoconfPayload["icon"] = "mdi:wifi";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(&MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[0], &mqttPayload[0], true);

  autoconfPayload.clear();

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = identifier + String(" PM 2.5");
  autoconfPayload["unit_of_measurement"] = "μg/m³";
  autoconfPayload["value_template"] = "{{value_json.pm25}}";
  autoconfPayload["unique_id"] = identifier + String("_pm25");
  autoconfPayload["icon"] = "mdi:air-filter";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(&MQTT_TOPIC_AUTOCONF_PM25_SENSOR[0], &mqttPayload[0], true);

  autoconfPayload.clear();
}
