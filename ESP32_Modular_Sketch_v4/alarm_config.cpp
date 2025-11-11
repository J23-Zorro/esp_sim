#include "alarm_config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace AlarmCfg {

static Config g;

void defaults(Config& c) {
  for (int i=0;i<8;++i) {
    c.analog[i].hi = 1e9;
    c.analog[i].lo = -1e9;
    c.analog[i].hystP = 0.0;
    c.analog[i].hystN = 0.0;
    c.analog[i].timeSec = 0;
    c.analog[i].countReq = 0;
  }
  for (int i=0;i<5;++i) {
    c.binary[i].mode = 2; // nie monitoruj
    c.binary[i].action = 0; // brak
    c.binary[i].timeSec = 0;
    c.binary[i].countReq = 0;
  }
}

Config get() { return g; }

void set(const Config& c) {
  g = c;
  (void)save();
}

bool load() {
  if (!LittleFS.exists("/alarm_config.json")) {
    defaults(g);
    return save();
  }
  File f = LittleFS.open("/alarm_config.json", "r");
  if (!f) { defaults(g); return save(); }

  StaticJsonDocument<4096> doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { defaults(g); return save(); }

  defaults(g); // start od defaults – nadpisz tylko to, co jest
  for (int i=0;i<8;++i) {
    char k[32];
    snprintf(k,sizeof(k),"analog_%d", i+1);
    JsonObject a = doc[k];
    if (!a.isNull()) {
      if (a["hi"].is<float>())    g.analog[i].hi = a["hi"].as<float>();
      if (a["lo"].is<float>())    g.analog[i].lo = a["lo"].as<float>();
      if (a["hystP"].is<float>()) g.analog[i].hystP = a["hystP"].as<float>();
      if (a["hystN"].is<float>()) g.analog[i].hystN = a["hystN"].as<float>();
      if (!a["timeSec"].isNull())  g.analog[i].timeSec = a["timeSec"].as<uint16_t>();
      if (!a["countReq"].isNull()) g.analog[i].countReq = a["countReq"].as<uint16_t>();
    }
  }
  for (int i=0;i<5;++i) {
    char k[32];
    snprintf(k,sizeof(k),"binary_%d", i+1);
    JsonObject b = doc[k];
    if (!b.isNull()) {
      if (!b["mode"].isNull())   g.binary[i].mode = b["mode"].as<uint8_t>();
      if (!b["action"].isNull()) g.binary[i].action = b["action"].as<uint8_t>();
      if (!b["timeSec"].isNull())  g.binary[i].timeSec = b["timeSec"].as<uint16_t>();
      if (!b["countReq"].isNull()) g.binary[i].countReq = b["countReq"].as<uint16_t>();
    }
  }
  return true;
}

bool save() {
  StaticJsonDocument<4096> doc;
  for (int i=0;i<8;++i) {
    char k[32];
    snprintf(k,sizeof(k),"analog_%d", i+1);
    JsonObject a = doc.createNestedObject(k);
    a["hi"] = g.analog[i].hi;
    a["lo"] = g.analog[i].lo;
    a["hystP"] = g.analog[i].hystP;
    a["hystN"] = g.analog[i].hystN;
    a["timeSec"] = g.analog[i].timeSec;
    a["countReq"] = g.analog[i].countReq;
  }
  for (int i=0;i<5;++i) {
    char k[32];
    snprintf(k,sizeof(k),"binary_%d", i+1);
    JsonObject b = doc.createNestedObject(k);
    b["mode"] = g.binary[i].mode;
    b["action"] = g.binary[i].action;
    b["timeSec"] = g.binary[i].timeSec;
    b["countReq"] = g.binary[i].countReq;
  }

  File f = LittleFS.open("/alarm_config.json", "w");
  if (!f) return false;
  bool ok = (serializeJsonPretty(doc, f) > 0);
  f.close();
  return ok;
}

} // namespace
