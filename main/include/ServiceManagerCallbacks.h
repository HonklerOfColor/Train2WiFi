#pragma once

class Service;
class Characteristic;

class ServiceManagerCallbacks {
 public:
  virtual ~ServiceManagerCallbacks() {}
  virtual void onServiceAdded(Service* service) {}
  virtual void onCharacteristicSubscriptionChanged(Characteristic* characteristic, bool removed) {}
};
