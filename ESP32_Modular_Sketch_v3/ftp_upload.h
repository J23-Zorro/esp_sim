#pragma once
#include <Arduino.h>
namespace FTP {
  bool uploadFile(const char* local_path, const char* remote_dir);
}
