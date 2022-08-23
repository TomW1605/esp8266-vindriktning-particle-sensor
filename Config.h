#pragma once

#include <ArduinoJson.h>
#include <FS.h>

class Config {
  public:
    char mqtt_server[80] = "example.tld";

    char username[24] = "";
    char password[24] = "";

    Config() = default;

    void save() {
      DynamicJsonDocument json(512);
      json["mqtt_server"] = mqtt_server;
      json["username"] = username;
      json["password"] = password;

      if (SPIFFS.begin()) {
  
        File configFile = SPIFFS.open("/config.json", "w");
        if (!configFile) {
          Serial.println("Config file not found or created!");
          return;
        }
  
        serializeJson(json, configFile);
        configFile.close();
        
        Serial.println("Config saved!");
      }
    }

    void load() {
      if (SPIFFS.begin()) {
        if (SPIFFS.exists("/config.json")) {
          File configFile = SPIFFS.open("/config.json", "r");

          if (configFile) {
            const size_t size = configFile.size();
            std::unique_ptr<char[]> buf(new char[size]);

            configFile.readBytes(buf.get(), size);
            DynamicJsonDocument json(512);

            if (DeserializationError::Ok == deserializeJson(json, buf.get())) {
              strcpy(mqtt_server, json["mqtt_server"]);
              strcpy(username, json["username"]);
              strcpy(password, json["password"]);
            }
          }
        }
      }
    }
}; // namespace Config
