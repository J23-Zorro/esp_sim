#pragma once
#include <Arduino.h>
#include <vector>

namespace EmailCfg {

struct Settings {
  // serwery
  String smtp_host = "smtp.wp.pl";
  uint16_t smtp_port = 465; // SSL
  String pop3_host = "pop3.wp.pl";
  uint16_t pop3_port = 995; // SSL

  // konto
  String user;              // np. tv.show@wp.pl
  String pass;              // ustaw w WWW
  String sender;            // From: (domyślnie = user)

  // odbiorcy (CSV: przecinki/średniki)
  String group1_csv;        // max 10
  String group2_csv;        // max 5
  String group3_csv;        // max 2

  // czasy (sekundy)
  uint32_t wait_g1 = 300;   // czekaj_na_potwierdzenie_grupa1
  uint32_t wait_g2 = 300;
  uint32_t wait_g3 = 300;

  // treść (wklei się do body)
  String body_alarm;        // max 256
  String body_recover;      // max 256

  bool enabled = true;      // globalny włącznik
};

bool load(Settings& out);   // z /email.json
bool save(const Settings& in);

void splitCSV(const String& csv, std::vector<String>& out,
              size_t maxItems);

} // namespace EmailCfg
