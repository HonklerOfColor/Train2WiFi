#pragma once

#include "NimBLEUUID.h"
#include "gatt_props.h"

#include <string>
#include <vector>

class Characteristic;

class CharacteristicCallbacks {
 public:
  virtual ~CharacteristicCallbacks() {}
  virtual void onSubscriptionChanged(Characteristic* characteristic, bool removed) {}
};
