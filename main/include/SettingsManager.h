#pragma once

#include <stdint.h>

#include <map>
#include <string>

#include "nvs.h"

enum VirtualShiftingMode {
  BASIC_RESISTANCE = 0,
  TARGET_POWER = 1,
  TRACK_RESISTANCE = 2,
};

class SettingsManager {
 public:
  static void load();
  static void save();
  static uint16_t getChainringTeeth();
  static uint16_t getSprocketTeeth();
  static uint16_t getDifficulty();
  static void setChainringTeeth(uint16_t chainringTeeth);
  static void setSprocketTeeth(uint16_t sprocketTeeth);
  static void setDifficulty(uint16_t difficulty);
  static bool isVirtualShiftingEnabled();
  static bool isGradeSmoothingEnabled();
  static VirtualShiftingMode getVirtualShiftingMode();
  static void setVirtualShiftingMode(VirtualShiftingMode virtualShiftingMode);
  static std::map<size_t, std::string> getVirtualShiftingModes();
  static void setVirtualShiftingEnabled(bool enabled);
  static void setGradeSmoothingEnabled(bool enabled);
  static std::string getTrainerDeviceName();
  static void setTrainerDeviceName(const std::string& trainerDevice);
  static std::string getUsername();
  static std::string getAPPassword();
  static bool isFTMSEnabled();
  static void setFTMSEnabled(bool enabled);
  static std::string getWifiSsid();
  static std::string getWifiPassword();
  static void setWifi(const std::string& ssid, const std::string& password);
  static int8_t getPowerOffsetPercent();
  static void setPowerOffsetPercent(int8_t percent);
  static uint16_t scaleReportedWatts(uint16_t watts);
  static uint16_t scaleTargetPower(uint16_t quarterWatts);

 private:
  static void readHandle(nvs_handle_t handle);
  static uint16_t chainringTeeth;
  static uint16_t sprocketTeeth;
  static uint16_t difficulty;
  static uint8_t virtualShifting;
  static uint8_t virtualShiftingMode;
  static uint8_t gradeSmoothing;
  static uint8_t ftms;
  static char trainerDevice[128];
  static char wifiSsid[33];
  static char wifiPassword[65];
  static int8_t powerOffset;
};
