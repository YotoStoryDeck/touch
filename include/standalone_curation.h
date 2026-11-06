#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "app_state.h"
#include "storage_sd.h"
#include "app_config.h"

namespace curation {

static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_pinned_ids;
static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_physical_ids;
static bool s_hide_physical = false;
static bool s_loaded = false;

inline bool is_card_pinned(const String& id){
  for(const auto& pid : s_pinned_ids){
    if(pid == id.c_str()) return true;
  }
  return false;
}

inline bool is_card_physical(const String& id){
  for(const auto& pid : s_physical_ids){
    if(pid == id.c_str()) return true;
  }
  return false;
}

inline bool get_hide_physical(){
  return s_hide_physical;
}

inline void set_hide_physical(bool hide){
  s_hide_physical = hide;
}

inline void toggle_card_pinned(const String& id, bool pin){
  for(auto it = s_pinned_ids.begin(); it != s_pinned_ids.end(); ++it){
    if(*it == id.c_str()){
      if(!pin) s_pinned_ids.erase(it);
      return;
    }
  }
  if(pin){
    s_pinned_ids.push_back(id.c_str());
  }
}

inline void toggle_card_physical(const String& id, bool phys){
  for(auto it = s_physical_ids.begin(); it != s_physical_ids.end(); ++it){
    if(*it == id.c_str()){
      if(!phys) s_physical_ids.erase(it);
      return;
    }
  }
  if(phys){
    s_physical_ids.push_back(id.c_str());
  }
}

inline void load(){
  s_pinned_ids.clear();
  s_physical_ids.clear();
  s_hide_physical = false;

  if(s_sd_ok){
    if(!SD.exists("/config")) SD.mkdir("/config");
    if(SD.exists("/config/curation.json")){
      File f = SD.open("/config/curation.json", FILE_READ);
      if(f){
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if(!err){
          s_hide_physical = (bool)(doc["hide_physical"] | false);
          if(!doc["pinned"].isNull()){
            for(JsonVariant v : doc["pinned"].as<JsonArray>()){
              String sid = v.as<String>();
              if(sid.length() > 0) s_pinned_ids.push_back(sid.c_str());
            }
          }
          if(!doc["physical"].isNull()){
            for(JsonVariant v : doc["physical"].as<JsonArray>()){
              String sid = v.as<String>();
              if(sid.length() > 0) s_physical_ids.push_back(sid.c_str());
            }
          }
          s_loaded = true;
          log_msg("[CURATION] Loaded from SD: %d pinned, %d physical, hide_phys=%d",
                  (int)s_pinned_ids.size(), (int)s_physical_ids.size(), s_hide_physical ? 1 : 0);
          return;
        }
      }
    }
  }

  // Fallback to NVS if SD not available
  Preferences p;
  p.begin("curation", true);
  s_hide_physical = p.getBool("hidep", false);
  String pins = p.getString("pinned", "");
  String phys = p.getString("phys", "");
  p.end();

  if(pins.length() > 0){
    int start = 0;
    while(start < pins.length()){
      int comma = pins.indexOf(',', start);
      if(comma == -1) comma = pins.length();
      String token = pins.substring(start, comma);
      token.trim();
      if(token.length() > 0) s_pinned_ids.push_back(token.c_str());
      start = comma + 1;
    }
  }
  if(phys.length() > 0){
    int start = 0;
    while(start < phys.length()){
      int comma = phys.indexOf(',', start);
      if(comma == -1) comma = phys.length();
      String token = phys.substring(start, comma);
      token.trim();
      if(token.length() > 0) s_physical_ids.push_back(token.c_str());
      start = comma + 1;
    }
  }
  s_loaded = true;
  log_msg("[CURATION] Loaded from NVS fallback: %d pinned, %d physical",
          (int)s_pinned_ids.size(), (int)s_physical_ids.size());
}

inline void save(){
  if(s_sd_ok){
    if(!SD.exists("/config")) SD.mkdir("/config");
    File f = SD.open("/config/curation.json", FILE_WRITE);
    if(f){
      JsonDocument doc;
      doc["hide_physical"] = s_hide_physical;
      JsonArray pArr = doc["pinned"].to<JsonArray>();
      for(const auto& id : s_pinned_ids){
        pArr.add(id.c_str());
      }
      JsonArray phArr = doc["physical"].to<JsonArray>();
      for(const auto& id : s_physical_ids){
        phArr.add(id.c_str());
      }
      serializeJson(doc, f);
      f.close();
      log_msg("[CURATION] Saved to /sd/config/curation.json");
      return;
    }
  }

  // Fallback to NVS
  Preferences p;
  p.begin("curation", false);
  p.putBool("hidep", s_hide_physical);
  String pins = "";
  for(size_t i = 0; i < s_pinned_ids.size(); i++){
    if(i > 0) pins += ",";
    pins += s_pinned_ids[i].c_str();
  }
  String phys = "";
  for(size_t i = 0; i < s_physical_ids.size(); i++){
    if(i > 0) phys += ",";
    phys += s_physical_ids[i].c_str();
  }
  p.putString("pinned", pins);
  p.putString("phys", phys);
  p.end();
  log_msg("[CURATION] Saved to NVS fallback");
}

} // namespace curation
