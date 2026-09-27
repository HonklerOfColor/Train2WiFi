#include "SettingsManager.h"

#include "Config.h"
#include "Utils.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

uint16_t SettingsManager::chainringTeeth = 34;
uint16_t SettingsManager::sprocketTeeth = 14;
uint16_t SettingsManager::difficulty = 100;
uint8_t SettingsManager::virtualShifting = 1;
uint8_t SettingsManager::virtualShiftingMode = BASIC_RESISTANCE;
uint8_t SettingsManager::gradeSmoothing = 1;
uint8_t SettingsManager::ftms = 0;
char SettingsManager::trainerDevice[128] = {};
char SettingsManager::wifiSsid[33] = {};
char SettingsManager::wifiPassword[65] = {};
int8_t SettingsManager::powerOffset = 0;

static const char* NVS_NAMESPACE = "train2wifi";

static void nvs_get_str_bounded(nvs_handle_t handle, const char* key, char* out, size_t out_len) {
  size_t length = out_len;
  if (nvs_get_str(handle, key, out, &length) != ESP_OK) {
    out[0] = 0;
  }
}

void SettingsManager::readHandle(nvs_handle_t handle) {
  nvs_get_u16(handle, "chainring", &chainringTeeth);
  nvs_get_u16(handle, "sprocket", &sprocketTeeth);
  nvs_get_u16(handle, "difficulty", &difficulty);
  nvs_get_u8(handle, "vs", &virtualShifting);
  nvs_get_u8(handle, "vs_mode", &virtualShiftingMode);
  nvs_get_u8(handle, "grade", &gradeSmoothing);
  nvs_get_u8(handle, "ftms", &ftms);
  nvs_get_str_bounded(handle, "trainer", trainerDevice, sizeof(trainerDevice));
  nvs_get_str_bounded(handle, "wifi_ssid", wifiSsid, sizeof(wifiSsid));
  nvs_get_str_bounded(handle, "wifi_pass", wifiPassword, sizeof(wifiPassword));
  int8_t offset = 0;
  if (nvs_get_i8(handle, "pwr_off", &offset) == ESP_OK) {
    powerOffset = offset;
  }
}

void SettingsManager::load() {
  nvs_handle_t handle;
  bool migrated = false;
  bool loaded = false;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    uint8_t ready = 0;
    if (nvs_get_u8(handle, "ready", &ready) == ESP_OK) {
      readHandle(handle);
      loaded = true;
    }
    nvs_close(handle);
  }
  if (!loaded && nvs_open("shiftr", NVS_READONLY, &handle) == ESP_OK) {
    readHandle(handle);
    nvs_close(handle);
    migrated = true;
  }
  if (wifiSsid[0] == 0) {
    setWifi(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
  }
  if (chainringTeeth == 0) {
    chainringTeeth = 34;
  }
  if (sprocketTeeth == 0) {
    sprocketTeeth = 14;
  }
  if (difficulty < 1) {
    difficulty = 100;
  }
  if (powerOffset < -10 || powerOffset > 10) {
    powerOffset = 0;
  }
  if (migrated) {
    save();
  }
}

void SettingsManager::save() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_set_u8(handle, "ready", 1);
  nvs_set_u16(handle, "chainring", chainringTeeth);
  nvs_set_u16(handle, "sprocket", sprocketTeeth);
  nvs_set_u16(handle, "difficulty", difficulty);
  nvs_set_u8(handle, "vs", virtualShifting);
  nvs_set_u8(handle, "vs_mode", virtualShiftingMode);
  nvs_set_u8(handle, "grade", gradeSmoothing);
  nvs_set_u8(handle, "ftms", ftms);
  nvs_set_i8(handle, "pwr_off", powerOffset);
  nvs_set_str(handle, "trainer", trainerDevice);
  nvs_set_str(handle, "wifi_ssid", wifiSsid);
  nvs_set_str(handle, "wifi_pass", wifiPassword);
  nvs_commit(handle);
  nvs_close(handle);
}

VirtualShiftingMode SettingsManager::getVirtualShiftingMode() {
  if (virtualShiftingMode == TARGET_POWER) {
    return TARGET_POWER;
  }
  if (virtualShiftingMode == TRACK_RESISTANCE) {
    return TRACK_RESISTANCE;
  }
  return BASIC_RESISTANCE;
}

void SettingsManager::setVirtualShiftingMode(VirtualShiftingMode mode) {
  virtualShiftingMode = (uint8_t)mode;
}

std::map<size_t, std::string> SettingsManager::getVirtualShiftingModes() {
  return {{0, "Basic Resistance"}, {1, "Target Power"}, {2, "Track Resistance"}};
}

uint16_t SettingsManager::getChainringTeeth() { return chainringTeeth; }
void SettingsManager::setChainringTeeth(uint16_t value) { chainringTeeth = value; }
uint16_t SettingsManager::getSprocketTeeth() { return sprocketTeeth; }
void SettingsManager::setSprocketTeeth(uint16_t value) { sprocketTeeth = value; }
bool SettingsManager::isVirtualShiftingEnabled() { return virtualShifting != 0; }
void SettingsManager::setVirtualShiftingEnabled(bool enabled) { virtualShifting = enabled ? 1 : 0; }
std::string SettingsManager::getTrainerDeviceName() { return trainerDevice; }

void SettingsManager::setTrainerDeviceName(const std::string& trainer) {
  strncpy(trainerDevice, trainer.c_str(), sizeof(trainerDevice) - 1);
  trainerDevice[sizeof(trainerDevice) - 1] = 0;
}

std::string SettingsManager::getUsername() { return DEFAULT_USERNAME; }

std::string SettingsManager::getAPPassword() {
  if (wifiPassword[0] != 0) {
    return wifiPassword;
  }
  return Utils::getHostName();
}

bool SettingsManager::isGradeSmoothingEnabled() { return gradeSmoothing != 0; }
void SettingsManager::setGradeSmoothingEnabled(bool enabled) { gradeSmoothing = enabled ? 1 : 0; }

uint16_t SettingsManager::getDifficulty() {
  if (difficulty < 1) {
    return 100;
  }
  return difficulty;
}

void SettingsManager::setDifficulty(uint16_t value) { difficulty = value; }
bool SettingsManager::isFTMSEnabled() { return ftms != 0; }
void SettingsManager::setFTMSEnabled(bool enabled) { ftms = enabled ? 1 : 0; }
std::string SettingsManager::getWifiSsid() { return wifiSsid; }
std::string SettingsManager::getWifiPassword() { return wifiPassword; }

void SettingsManager::setWifi(const std::string& ssid, const std::string& password) {
  strncpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = 0;
  strncpy(wifiPassword, password.c_str(), sizeof(wifiPassword) - 1);
  wifiPassword[sizeof(wifiPassword) - 1] = 0;
}

int8_t SettingsManager::getPowerOffsetPercent() { return powerOffset; }

void SettingsManager::setPowerOffsetPercent(int8_t percent) {
  if (percent < -10) {
    percent = -10;
  } else if (percent > 10) {
    percent = 10;
  }
  powerOffset = percent;
}

uint16_t SettingsManager::scaleReportedWatts(uint16_t watts) {
  if (powerOffset == 0 || watts == 0) {
    return watts;
  }
  int32_t scaled = ((int32_t)watts * (100 + (int)powerOffset) + 50) / 100;
  if (scaled < 0) {
    return 0;
  }
  if (scaled > 65535) {
    return 65535;
  }
  return (uint16_t)scaled;
}

uint16_t SettingsManager::scaleTargetPower(uint16_t quarterWatts) {
  if (powerOffset == 0 || quarterWatts == 0) {
    return quarterWatts;
  }
  int denom = 100 + (int)powerOffset;
  if (denom < 1) {
    denom = 1;
  }
  uint32_t scaled = ((uint32_t)quarterWatts * 100u + (uint32_t)(denom / 2)) / (uint32_t)denom;
  if (scaled > 65535u) {
    return 65535;
  }
  return (uint16_t)scaled;
}
