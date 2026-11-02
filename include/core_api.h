#pragma once
// StoryDeck Core Client for ESP32
// Interacts with StoryDeck Core hub (Docker container, default port 5055/5060)
// providing device registration, child profiles, and merged catalog.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include "app_config.h"

namespace core {

static String s_access_token = "";
static bool   s_is_registered = false;
static bool   s_is_approved = false;

inline String get_mac(){
  return WiFi.macAddress();
}

// 1. Health Probe
inline bool ping(const String& base_url){
  if(base_url.length() == 0) return false;
  HTTPClient http;
  http.begin(base_url + "/health");
  http.setTimeout(3000);
  int code = http.GET();
  http.end();
  return (code == 200);
}

// 2. Kiosk Device Registration
inline bool register_device(const String& base_url, String& err_out){
  if(base_url.length() == 0){
    err_out = "Core URL not set";
    return false;
  }
  HTTPClient http;
  http.begin(base_url + "/api/devices/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  JsonDocument doc;
  doc["mac"]              = get_mac();
  doc["friendly_name"]    = "StoryDeck Touch (ESP32-S3)";
  doc["firmware_version"] = "1.0.0-s3";

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  if(code == 202){
    s_is_registered = true;
    s_is_approved = false;
    err_out = "Pending Admin Approval in StoryDeck Core";
    return false;
  }
  if(code == 200 || code == 201){
    JsonDocument rdoc;
    if(!deserializeJson(rdoc, resp)){
      const char* tok = rdoc["tokens"]["access_token"];
      if(tok && *tok){
        s_access_token = tok;
      }
    }
    s_is_registered = true;
    s_is_approved = true;
    return true;
  }

  err_out = String("Registration failed (HTTP ") + code + ")";
  return false;
}

// 3. Playback Dispatch via Core Hub
inline int play_media(const String& base_url, const String& child_id, const String& card_type, const String& media_id, const String& device_id, int chapter_idx){
  if(base_url.length() == 0) return -1;
  HTTPClient http;
  http.begin(base_url + "/api/play");
  http.addHeader("Content-Type", "application/json");
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(5000);

  JsonDocument doc;
  doc["child"]     = child_id.length() ? child_id : "default";
  doc["type"]      = card_type.length() ? card_type : "yoto";
  doc["id"]        = media_id;
  doc["device_id"] = device_id;
  doc["chapter"]   = chapter_idx + 1; // 1-based chapter for broker

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  return code;
}

// 4. Transport Control (pause, resume, stop, volume)
inline int player_control(const String& base_url, const String& device_id, const char* action, int volume = -1){
  if(base_url.length() == 0) return -1;
  HTTPClient http;
  http.begin(base_url + "/api/player/control");
  http.addHeader("Content-Type", "application/json");
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(5000);

  JsonDocument doc;
  doc["device_id"] = device_id;
  doc["action"]    = action;
  if(volume >= 0) doc["volume"] = volume;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  return code;
}

} // namespace core

