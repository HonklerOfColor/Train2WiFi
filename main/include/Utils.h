#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

class Utils {
 public:
  static void init();
  static std::string getHexString(uint8_t* data, size_t length);
  static std::string getHexString(std::vector<uint8_t> data);
  static std::string getHexString(std::vector<uint8_t>* data);
  static std::string getMacAddressString();
  static std::string getSerialNumberString();
  static std::string getDeviceName();
  static std::string getHostName();
  static std::string getFQDN();
  static std::vector<uint8_t> getVectorFromStruct(void* data, size_t length);

 private:
  static uint64_t macAddress;
};
