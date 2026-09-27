#include "BTClientCallbacks.h"

#include "BTDeviceManager.h"
#include "app_log.h"

void BTClientCallbacks::onConnect(NimBLEClient* pClient) {
  log_i("BLE link up with %s", pClient->getPeerAddress().toString().c_str());
}

void BTClientCallbacks::onConnectFail(NimBLEClient* pClient, int reason) {
  log_e("BLE connect failed to %s, reason %d", pClient->getPeerAddress().toString().c_str(), reason);
  BTDeviceManager::connected = false;
}

void BTClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
  log_e("Disconnected from %s, reason %d", pClient->getPeerAddress().toString().c_str(), reason);
  BTDeviceManager::handleLinkLoss();
}

bool BTClientCallbacks::onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) {
  return true;
}
