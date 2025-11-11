#include "email_config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace EmailCfg {

static const char* kPath = "/email.json";

static String trim(const String& s){
  String r = s; r.trim(); return r;
}

bool load(Settings& out) {
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, f); f.close();
  if (e) return false;

  auto J = [&](const char* k)->String { return doc.containsKey(k)? String(doc[k].as<const char*>()) : String(); };

  out.smtp_host = J("smtp_host"); out.smtp_port = doc["smtp_port"] | out.smtp_port;
  out.pop3_host = J("pop3_host"); out.pop3_port = doc["pop3_port"] | out.pop3_port;
  out.user = J("user"); out.pass = J("pass"); out.sender = J("sender");
  out.group1_csv = J("group1_csv"); out.group2_csv = J("group2_csv"); out.group3_csv = J("group3_csv");
  out.wait_g1 = doc["wait_g1"] | out.wait_g1; out.wait_g2 = doc["wait_g2"] | out.wait_g2; out.wait_g3 = doc["wait_g3"] | out.wait_g3;
  out.body_alarm = J("body_alarm"); out.body_recover = J("body_recover");
  out.enabled = doc["enabled"] | out.enabled;

  if (out.sender.length()==0) out.sender = out.user;
  return true;
}

bool save(const Settings& in) {
  DynamicJsonDocument doc(4096);
  doc["smtp_host"] = in.smtp_host;
  doc["smtp_port"] = in.smtp_port;
  doc["pop3_host"] = in.pop3_host;
  doc["pop3_port"] = in.pop3_port;
  doc["user"] = in.user;
  doc["pass"] = in.pass;
  doc["sender"] = in.sender;
  doc["group1_csv"] = in.group1_csv;
  doc["group2_csv"] = in.group2_csv;
  doc["group3_csv"] = in.group3_csv;
  doc["wait_g1"] = in.wait_g1;
  doc["wait_g2"] = in.wait_g2;
  doc["wait_g3"] = in.wait_g3;
  doc["body_alarm"] = in.body_alarm;
  doc["body_recover"] = in.body_recover;
  doc["enabled"] = in.enabled;

  File f = LittleFS.open(kPath, "w");
  if (!f) return false;
  serializeJson(doc, f); f.close();
  return true;
}

void splitCSV(const String& csv, std::vector<String>& out, size_t maxItems) {
  out.clear();
  String s = csv; s.replace(';', ',');
  int start = 0;
  while (start < (int)s.length() && out.size() < maxItems) {
    int comma = s.indexOf(',', start);
    String token = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    token.trim();
    if (token.length() > 0) out.push_back(token);
    if (comma < 0) break;
    start = comma + 1;
  }
}

} // namespace EmailCfg
