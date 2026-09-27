#pragma once

#include "NimBLEDevice.h"

class BTClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override;
  void onConnectFail(NimBLEClient* pClient, int reason) override;
  void onDisconnect(NimBLEClient* pClient, int reason) override;
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override;
};
