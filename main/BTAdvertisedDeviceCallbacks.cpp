#include "BTAdvertisedDeviceCallbacks.h"

#include "BTDeviceManager.h"
#include "UUIDs.h"
#include "app_log.h"

static bool tacxManufacturer(const NimBLEAdvertisedDevice* device) {
  if (!device->haveManufacturerData()) {
    return false;
  }
  std::string data = device->getManufacturerData();
  if (data.size() < 2) {
    return false;
  }
  uint16_t company = (uint8_t)data[0] | ((uint16_t)(uint8_t)data[1] << 8);
  return company == 0x0689;
}

static bool advertisesTrainer(const NimBLEAdvertisedDevice* device) {
  return device->isAdvertisingService(NimBLEUUID(FITNESS_MACHINE_SERVICE_UUID)) ||
         device->isAdvertisingService(NimBLEUUID(CYCLING_POWER_SERVICE_UUID)) ||
         device->isAdvertisingService(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID)) || tacxManufacturer(device);
}

// Advertised names may carry padding, NUL bytes or other control characters.
static std::string sanitizeName(const std::string& raw) {
  std::string name;
  for (char character : raw) {
    if ((uint8_t)character >= 0x20 && (uint8_t)character != 0x7F) {
      name.push_back(character);
    }
  }
  size_t last = name.find_last_not_of(' ');
  if (last == std::string::npos) {
    return "";
  }
  return name.substr(name.find_first_not_of(' '), last + 1 - name.find_first_not_of(' '));
}

static std::string deviceLabel(const NimBLEAdvertisedDevice* device) {
  if (device->haveName()) {
    std::string name = sanitizeName(device->getName());
    if (!name.empty()) {
      return name;
    }
  }
  if (!advertisesTrainer(device)) {
    return "";
  }
  if (tacxManufacturer(device)) {
    return "Tacx " + device->getAddress().toString();
  }
  return device->getAddress().toString();
}

void BTAdvertisedDeviceCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
  std::string name = deviceLabel(advertisedDevice);
  log_i("ADV %s rssi=%d name='%s' services=%d tacx=%d", advertisedDevice->getAddress().toString().c_str(),
        advertisedDevice->getRSSI(), name.c_str(), advertisedDevice->getServiceUUIDCount(),
        tacxManufacturer(advertisedDevice) ? 1 : 0);
  if (name.empty()) {
    return;
  }
  BTDeviceManager::rememberDevice(advertisedDevice->getAddress(), name);
}

void BTAdvertisedDeviceCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
  BTDeviceManager::onScanFinished();
}
