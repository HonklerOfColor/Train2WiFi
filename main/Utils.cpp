#include "Utils.h"

#include "Config.h"

#include <stdio.h>

#include "esp_mac.h"

#include <algorithm>

uint64_t Utils::macAddress = 0;

void Utils::init() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  macAddress = 0;
  for (int i = 0; i < 6; i++) {
    macAddress |= (uint64_t)mac[i] << (8 * i);
  }
}

std::string Utils::getMacAddressString() {
  char macAddressString[18];
  sprintf(macAddressString, "%02X-%02X-%02X-%02X-%02X-%02X", (uint8_t)(macAddress), (uint8_t)(macAddress >> 8),
          (uint8_t)(macAddress >> 16), (uint8_t)(macAddress >> 24), (uint8_t)(macAddress >> 32),
          (uint8_t)(macAddress >> 40));
  return macAddressString;
}

std::string Utils::getSerialNumberString() {
  char serialNumberString[16];
  sprintf(serialNumberString, "%i%i%i", (uint8_t)(macAddress >> 24), (uint8_t)(macAddress >> 32),
          (uint8_t)(macAddress >> 40));
  return serialNumberString;
}

std::string Utils::getDeviceName() {
  char deviceName[sizeof(DEVICE_NAME_PREFIX) + 8];
  sprintf(deviceName, "%s %02X%02X%02X", DEVICE_NAME_PREFIX, (uint8_t)(macAddress >> 24), (uint8_t)(macAddress >> 32),
          (uint8_t)(macAddress >> 40));
  return deviceName;
}

std::string Utils::getHostName() {
  std::string returnString = Utils::getDeviceName();
  std::replace(returnString.begin(), returnString.end(), ' ', '-');
  return returnString;
}

std::string Utils::getFQDN() {
  return Utils::getHostName() + ".local";
}

std::string Utils::getHexString(uint8_t* data, size_t length) {
  char hexNumber[3];
  std::string hexString = "[";
  for (size_t index = 0; index < length; index++) {
    sprintf(hexNumber, "%02X", data[index]);
    hexString.append(hexNumber);
    if (index < (length - 1)) {
      hexString.append(" ");
    }
  }
  hexString.append("]");
  return hexString;
}

std::string Utils::getHexString(std::vector<uint8_t> data) {
  return Utils::getHexString(data.data(), data.size());
}

std::string Utils::getHexString(std::vector<uint8_t>* data) {
  return Utils::getHexString(data->data(), data->size());
}

std::vector<uint8_t> Utils::getVectorFromStruct(void* data, size_t length) {
  std::vector<uint8_t> returnVector;
  for (size_t index = 0; index < length; index++) {
    returnVector.push_back(((uint8_t*)data)[index]);
  }
  return returnVector;
}
