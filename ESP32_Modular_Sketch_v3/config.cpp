#include "config.h"
#include "log.h"

static ConfigData g_cfg;
static const char* CFG_PATH = "/config.json";

ConfigData& Config::get() { return g_cfg; }

void Config::begin() {
  if (!load()) {
    save(g_cfg); // write defaults
  }
}

bool Config::save(const ConfigData& d) {
  g_cfg = d;

  JsonDocument doc;
  doc["net_mode"] = d.net_mode;
  doc["wifi_ssid"] = d.wifi_ssid;
  doc["wifi_pass"] = d.wifi_pass;
  doc["apn"] = d.apn;
  doc["pin"] = d.pin;
  doc["uart_tx"] = d.uart_tx;
  doc["uart_rx"] = d.uart_rx;
  doc["uart_baud"] = d.uart_baud;
  doc["mqtt_host"] = d.mqtt_host;
  doc["mqtt_port"] = d.mqtt_port;
  doc["mqtt_user"] = d.mqtt_user;
  doc["mqtt_pass"] = d.mqtt_pass;
  doc["mqtt_topic_pub"] = d.mqtt_topic_pub;
  doc["ftp_host"] = d.ftp_host;
  doc["ftp_port"] = d.ftp_port;
  doc["ftp_user"] = d.ftp_user;
  doc["ftp_pass"] = d.ftp_pass;
  doc["ftp_dir"] = d.ftp_dir;
  doc["http_user"] = d.http_user;
  doc["http_pass"] = d.http_pass;
  doc["sendFTPInterval"] = d.sendFTPInterval_sec;
  doc["cfgSyncInterval_sec"] = d.cfgSyncInterval_sec; 
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) { LOGE("Config save: open failed"); return false; }
  if (serializeJson(doc, f) == 0) { f.close(); return false; }
  f.close();
  LOGI("Config saved.");
  return true;
}

#include <ArduinoJson.h>

// String
static void setStr(JsonDocument& doc, const char* key, String& out) {
  JsonVariant v = doc[key];
  if (v.is<const char*>())      { out = String(v.as<const char*>()); return; }
  if (v.is<String>())           { out = v.as<String>(); return; }
  if (!v.isNull())              { out = String(v); return; } // fallback (liczba -> "123")
  // gdy brak klucza: zostaw 'out' bez zmian (domyślna wartość już ustawiona)
}

// int
static void setInt(JsonDocument& doc, const char* key, int& out) {
  JsonVariant v = doc[key];
  if (v.is<int>())              { out = v.as<int>(); return; }
  if (v.is<long>())             { out = (int)v.as<long>(); return; }
  if (v.is<unsigned long>())    { out = (int)v.as<unsigned long>(); return; }
  if (v.is<const char*>())      { out = atoi(v.as<const char*>()); return; }
  // brak klucza -> bez zmian
}

// uint32_t
static void setUInt32(JsonDocument& doc, const char* key, uint32_t& out) {
  JsonVariant v = doc[key];
  if (v.is<uint32_t>())         { out = v.as<uint32_t>(); return; }
  if (v.is<unsigned long>())    { out = (uint32_t)v.as<unsigned long>(); return; }
  if (v.is<long>())             { long t = v.as<long>(); out = (uint32_t)(t < 0 ? 0 : t); return; }
  if (v.is<const char*>())      { out = (uint32_t)strtoul(v.as<const char*>(), nullptr, 10); return; }
  // brak klucza -> bez zmian
}

bool Config::load() {
  if (!LittleFS.exists(CFG_PATH)) return false;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) { LOGE("Config load error: %s", err.c_str()); return false; }

  setStr(doc, "net_mode", g_cfg.net_mode);
  setStr(doc, "wifi_ssid", g_cfg.wifi_ssid);
  setStr(doc, "wifi_pass", g_cfg.wifi_pass);
  setStr(doc, "apn", g_cfg.apn);
  setStr(doc, "pin", g_cfg.pin);
  setInt(doc, "uart_tx", g_cfg.uart_tx);
  setInt(doc, "uart_rx", g_cfg.uart_rx);
  setUInt32(doc, "uart_baud", g_cfg.uart_baud);
  setStr(doc, "mqtt_host", g_cfg.mqtt_host);
  setInt(doc, "mqtt_port", g_cfg.mqtt_port);
  setStr(doc, "mqtt_user", g_cfg.mqtt_user);
  setStr(doc, "mqtt_pass", g_cfg.mqtt_pass);
  setStr(doc, "mqtt_topic_pub", g_cfg.mqtt_topic_pub);
  setStr(doc, "ftp_host", g_cfg.ftp_host);
  setInt(doc, "ftp_port", g_cfg.ftp_port);
  setStr(doc, "ftp_user", g_cfg.ftp_user);
  setStr(doc, "ftp_pass", g_cfg.ftp_pass);
  setStr(doc, "ftp_dir", g_cfg.ftp_dir);
  setStr(doc, "http_user", g_cfg.http_user);
  setStr(doc, "http_pass", g_cfg.http_pass);
  setUInt32(doc, "sendFTPInterval", g_cfg.sendFTPInterval_sec);
  setUInt32(doc, "cfgSyncInterval_sec", g_cfg.cfgSyncInterval_sec);
  LOGI("Config loaded from FS.");
  return true;
}
