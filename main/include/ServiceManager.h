#pragma once

#include "Service.h"
#include "ServiceManagerCallbacks.h"

#include <string>
#include <vector>

class ServiceManager {
 public:
  friend class BTDeviceManager;
  friend class DirConServiceManagerCallbacks;
  friend class ServiceManagerCharacteristicCallbacks;
  ServiceManager();
  std::vector<Service*> getServices();
  void addService(Service* service);
  Service* getService(const NimBLEUUID& serviceUUID);
  Service* getServiceByCharacteristic(const NimBLEUUID& characteristicUUID);
  Characteristic* getCharacteristic(const NimBLEUUID& characteristicUUID);
  void subscribeCallbacks(ServiceManagerCallbacks* callbacks);
  std::string getStatusMessage();

 private:
  std::vector<Service*> services;
  std::vector<ServiceManagerCallbacks*> serviceManagerCallbacks;
  std::string statusMessage;
  void updateStatusMessage();
};
