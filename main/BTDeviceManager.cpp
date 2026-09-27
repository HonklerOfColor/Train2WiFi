#include "BTDeviceManager.h"

#include "BTAdvertisedDeviceCallbacks.h"
#include "BTClientCallbacks.h"
#include "Config.h"
#include "DirConManager.h"
#include "Service.h"
#include "SettingsManager.h"
#include "UUIDs.h"
#include "gatt_props.h"
#include "app_log.h"

#include "esp_coexist.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"

std::vector<ScannedBleDevice> BTDeviceManager::scannedDevices{};
ServiceManager* BTDeviceManager::serviceManager{};
std::string BTDeviceManager::deviceName;
std::string BTDeviceManager::remoteDeviceName;
std::string BTDeviceManager::connectedDeviceName;
NimBLEClient* BTDeviceManager::nimBLEClient{};
NimBLEAddress BTDeviceManager::lastTrainerAddress;
std::string BTDeviceManager::lastTrainerName;
bool BTDeviceManager::connected = false;
bool BTDeviceManager::started = false;
std::string BTDeviceManager::statusMessage;

static esp_timer_handle_t scanTimer;
static esp_timer_handle_t connectTimer;
static BTAdvertisedDeviceCallbacks scanCallbacks;
static SemaphoreHandle_t deviceMutex;
static volatile bool rescanRequested = false;
static volatile bool scanAbort = false;
static volatile bool userScan = false;
static volatile bool suppressReconnect = false;
static volatile bool connectInProgress = false;
static bool scanActive = false;
static bool everConnected = false;
static uint32_t reconnectAttempts = 0;
static int64_t nextConnectAllowedUs = 0;

class BTDeviceServiceManagerCallbacks : public ServiceManagerCallbacks {
  void onCharacteristicSubscriptionChanged(Characteristic* characteristic, bool removed) override {
    if (!characteristic->getService()->isInternal()) {
      if (removed && (characteristic->getSubscriptions().size() == 0)) {
        BTDeviceManager::changeBLENotify(characteristic, true);
      } else if (characteristic->getSubscriptions().size() > 0) {
        BTDeviceManager::changeBLENotify(characteristic, false);
      }
    }
  }
};

void scanTimerCb(void*) { BTDeviceManager::doScan(); }
void connectTimerCb(void*) { BTDeviceManager::doConnect(); }

void BTDeviceManager::onBLENotify(NimBLERemoteCharacteristic* characteristic, uint8_t* pData, size_t length, bool isNotify) {
  DirConManager::notifyDirConCharacteristic(characteristic->getUUID(), pData, length);
}

std::vector<std::string> BTDeviceManager::getScannedDeviceNames() {
  std::vector<std::string> names;
  if (deviceMutex == nullptr || xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return names;
  }
  for (const ScannedBleDevice& device : scannedDevices) {
    if (!device.name.empty()) {
      names.push_back(device.name);
    }
  }
  xSemaphoreGive(deviceMutex);
  return names;
}

void BTDeviceManager::rememberDevice(const NimBLEAddress& address, const std::string& name) {
  if (deviceMutex == nullptr || xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }
  bool known = false;
  for (ScannedBleDevice& existing : scannedDevices) {
    if (existing.address == address) {
      if (existing.name.empty() || existing.name == address.toString()) {
        existing.name = name;
      }
      known = true;
      break;
    }
  }
  if (!known) {
    scannedDevices.push_back(ScannedBleDevice{address, name});
  }
  xSemaphoreGive(deviceMutex);
  if (!remoteDeviceName.empty() && name.rfind(remoteDeviceName, 0) == 0) {
    lastTrainerAddress = address;
    lastTrainerName = name;
  }
  ScannedBleDevice matched;
  if (!userScan && !connectInProgress && copyRemoteDevice(matched)) {
    if (esp_timer_get_time() < nextConnectAllowedUs) {
      return;
    }
    // The configured trainer is in range, stop scanning and connect. NimBLE does not report
    // a scan end for a cancelled scan, so the connect has to be scheduled here.
    stopScan();
    scheduleConnect();
  }
}

void BTDeviceManager::scheduleConnect(uint32_t delayMs) {
  if (!started || connectTimer == nullptr || connectInProgress) {
    return;
  }
  if (connected && nimBLEClient != nullptr && nimBLEClient->isConnected()) {
    return;
  }
  if (scanTimer) {
    esp_timer_stop(scanTimer);
  }
  esp_timer_stop(connectTimer);
  uint64_t delayUs = delayMs == 0 ? 1 : (uint64_t)delayMs * 1000ULL;
  esp_timer_start_once(connectTimer, delayUs);
}

void BTDeviceManager::scheduleReconnect() {
  uint32_t delay = reconnectAttempts == 0 ? BLE_RECONNECT_DELAY_MS : BLE_RECONNECT_RETRY_MS;
  if (!lastTrainerName.empty()) {
    statusMessage = "Reconnecting to '" + lastTrainerName + "'";
  } else {
    statusMessage = "Reconnecting...";
  }
  log_i("Scheduling BLE reconnect in %u ms (attempt %u)", delay, reconnectAttempts + 1);
  scheduleConnect(delay);
}

void BTDeviceManager::handleLinkLoss() {
  connected = false;
  connectedDeviceName = "";
  if (!started || suppressReconnect || connectInProgress) {
    return;
  }
  if (everConnected && !lastTrainerAddress.isNull() && !remoteDeviceName.empty()) {
    reconnectAttempts = 0;
    scheduleReconnect();
    return;
  }
  startScan();
}

void BTDeviceManager::requestRescan() { rescanRequested = true; }

bool BTDeviceManager::isScanning() { return scanActive; }

bool BTDeviceManager::start() {
  if (started) {
    return false;
  }
  if (serviceManager == nullptr) {
    return false;
  }
  scannedDevices.clear();
  deviceMutex = xSemaphoreCreateMutex();
  serviceManager->subscribeCallbacks(new BTDeviceServiceManagerCallbacks);
  NimBLEDevice::init(deviceName);
  esp_timer_create_args_t scanArgs = {};
  scanArgs.callback = scanTimerCb;
  scanArgs.name = "ble_scan";
  esp_timer_create_args_t connectArgs = {};
  connectArgs.callback = connectTimerCb;
  connectArgs.name = "ble_connect";
  esp_timer_create(&scanArgs, &scanTimer);
  esp_timer_create(&connectArgs, &connectTimer);
  started = true;
  suppressReconnect = false;
  everConnected = false;
  reconnectAttempts = 0;
  nextConnectAllowedUs = 0;
  startScan();
  return true;
}

void BTDeviceManager::stop() {
  suppressReconnect = true;
  connected = false;
  started = false;
  if (connectTimer) {
    esp_timer_stop(connectTimer);
  }
  stopScan();
  if (nimBLEClient != nullptr && nimBLEClient->isConnected()) {
    nimBLEClient->disconnect();
  }
}

void BTDeviceManager::update() {
  if (rescanRequested) {
    rescanRequested = false;
    scanAbort = true;
    suppressReconnect = true;
    if (NimBLEDevice::getScan() != nullptr && NimBLEDevice::getScan()->isScanning()) {
      stopScan();
    }
    scanAbort = false;
    userScan = true;
    doScan();
    return;
  }
  if (connected && nimBLEClient != nullptr && !nimBLEClient->isConnected()) {
    handleLinkLoss();
  }
}

bool BTDeviceManager::isConnected() { return connected; }
std::string BTDeviceManager::getConnecedDeviceName() { return connectedDeviceName; }

void BTDeviceManager::setLocalDeviceName(const std::string& localDeviceName) { deviceName = localDeviceName; }

void BTDeviceManager::setRemoteDeviceNameFilter(const std::string& remoteDeviceNameFilter) {
  if (remoteDeviceName != remoteDeviceNameFilter) {
    lastTrainerAddress = NimBLEAddress();
    lastTrainerName.clear();
    reconnectAttempts = 0;
  }
  remoteDeviceName = remoteDeviceNameFilter;
  if (!started || NimBLEDevice::getScan() == nullptr) {
    return;
  }
  ScannedBleDevice matched;
  if (copyRemoteDevice(matched)) {
    stopScan();
    scheduleConnect();
  } else if (!NimBLEDevice::getScan()->isScanning()) {
    startScan();
  }
}

void BTDeviceManager::setServiceManager(ServiceManager* manager) { serviceManager = manager; }

bool BTDeviceManager::copyRemoteDevice(ScannedBleDevice& out) {
  if (remoteDeviceName.empty() || deviceMutex == nullptr) {
    return false;
  }
  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  bool found = false;
  for (const ScannedBleDevice& device : scannedDevices) {
    if (device.name.rfind(remoteDeviceName, 0) == 0) {
      out = device;
      found = true;
      break;
    }
  }
  xSemaphoreGive(deviceMutex);
  return found;
}

bool BTDeviceManager::resolveTrainer(ScannedBleDevice& out) {
  if (copyRemoteDevice(out)) {
    return true;
  }
  if (lastTrainerAddress.isNull()) {
    return false;
  }
  out.address = lastTrainerAddress;
  out.name = lastTrainerName.empty() ? remoteDeviceName : lastTrainerName;
  return !out.name.empty() || !lastTrainerAddress.isNull();
}

void BTDeviceManager::terminatePeer(const NimBLEAddress& address) {
  if (address.isNull()) {
    return;
  }
  ble_gap_conn_desc desc = {};
  if (ble_gap_conn_find_by_addr(address.getBase(), &desc) != 0) {
    return;
  }
  log_w("Terminating leftover GAP connection handle %u to %s", desc.conn_handle, address.toString().c_str());
  ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  vTaskDelay(pdMS_TO_TICKS(200));
}

void BTDeviceManager::dropClient() {
  suppressReconnect = true;
  if (nimBLEClient != nullptr && nimBLEClient->isConnected()) {
    nimBLEClient->disconnect();
    int64_t deadline = esp_timer_get_time() + 2000 * 1000;
    while (nimBLEClient->isConnected() && esp_timer_get_time() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  if (!lastTrainerAddress.isNull()) {
    terminatePeer(lastTrainerAddress);
  }
  connected = false;
  connectedDeviceName = "";
  suppressReconnect = false;
}

bool BTDeviceManager::waitForLink(uint32_t timeoutMs) {
  int64_t deadline = esp_timer_get_time() + (int64_t)timeoutMs * 1000;
  while (nimBLEClient != nullptr && !nimBLEClient->isConnected() && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (nimBLEClient == nullptr || !nimBLEClient->isConnected()) {
    return false;
  }
  // Give the trainer a moment after GAP-up; Tacx rejects MTU exchange and then updates params.
  vTaskDelay(pdMS_TO_TICKS(400));
  return nimBLEClient->isConnected();
}

bool BTDeviceManager::discoverTrainerServices() {
  if (nimBLEClient == nullptr) {
    return false;
  }
  for (int attempt = 1; attempt <= 4; attempt++) {
    if (!nimBLEClient->isConnected()) {
      log_e("Service discovery aborted, BLE link dropped");
      return false;
    }
    if (attempt > 1) {
      log_i("Retrying BLE service discovery (%d/4)", attempt);
      statusMessage = "Discovering services...";
      vTaskDelay(pdMS_TO_TICKS(400));
    }
    const std::vector<NimBLERemoteService*>& remoteServices = nimBLEClient->getServices(true);
    if (!remoteServices.empty()) {
      log_i("Discovered %d BLE services on '%s'", (int)remoteServices.size(), connectedDeviceName.c_str());
      return true;
    }
    log_e("Service discovery attempt %d returned nothing, error %d", attempt, nimBLEClient->getLastError());
  }
  return false;
}

bool BTDeviceManager::connectRemoteDevice(const ScannedBleDevice& remoteDevice) {
  log_i("Connecting to BLE device %s (%s)", remoteDevice.address.toString().c_str(), remoteDevice.name.c_str());
  stopScan();
  lastTrainerAddress = remoteDevice.address;
  lastTrainerName = remoteDevice.name;
  if (nimBLEClient == nullptr) {
    nimBLEClient = NimBLEDevice::createClient();
    nimBLEClient->setClientCallbacks(new BTClientCallbacks(), true);
    nimBLEClient->setConnectRetries(everConnected ? 2 : 1);
    nimBLEClient->setConnectionParams(24, 40, 0, 400, 16, 16);
  }
  uint32_t timeoutMs = (everConnected ? BLE_RECONNECT_TIMEOUT : BLE_CONNECT_TIMEOUT) * 1000;
  nimBLEClient->setConnectTimeout(timeoutMs);
  esp_coex_preference_set(ESP_COEX_PREFER_BT);

  if (!nimBLEClient->isConnected()) {
    // Skip MTU exchange: Tacx answers with unsupported remote feature.
    if (!nimBLEClient->connect(remoteDevice.address, true, false, false)) {
      int err = nimBLEClient->getLastError();
      log_e("Connection to BLE device failed, error %d", err);
      if (err == BLE_HS_EDONE) {
        log_w("BLE stack already has a connection, waiting for it");
      } else if (!waitForLink(800)) {
        statusMessage = "Connection failed";
        dropClient();
        return false;
      }
    }
  }
  if (!waitForLink(timeoutMs)) {
    log_e("BLE link did not come up in time");
    statusMessage = "Connection failed";
    dropClient();
    return false;
  }

  connectedDeviceName = remoteDevice.name;
  statusMessage = "Discovering services...";

  if (serviceManager == nullptr) {
    log_e("Connection to BLE device failed, service manager not available");
    dropClient();
    return false;
  }

  if (!discoverTrainerServices()) {
    log_e("Service discovery returned nothing, dropping the connection");
    statusMessage = "Service discovery failed";
    dropClient();
    return false;
  }

  const std::vector<NimBLERemoteService*>& remoteServices = nimBLEClient->getServices(false);
  for (NimBLERemoteService* remoteService : remoteServices) {
    if (SettingsManager::isFTMSEnabled() && remoteService->getUUID().equals(NimBLEUUID(FITNESS_MACHINE_SERVICE_UUID))) {
      continue;
    }
    Service* service = serviceManager->getService(remoteService->getUUID());
    bool isAdvertising = remoteService->getUUID().equals(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID));
    if (service == nullptr) {
      service = new Service(remoteService->getUUID(), isAdvertising, false);
      serviceManager->addService(service);
    }
    service->Advertise = isAdvertising;
    const std::vector<NimBLERemoteCharacteristic*>& remoteCharacteristics = remoteService->getCharacteristics(true);
    for (NimBLERemoteCharacteristic* remoteCharacteristic : remoteCharacteristics) {
      Characteristic* characteristic = service->getCharacteristic(remoteCharacteristic->getUUID());
      if (characteristic == nullptr) {
        characteristic = new Characteristic(remoteCharacteristic->getUUID(), getProperties(remoteCharacteristic));
        service->addCharacteristic(characteristic);
      }
      characteristic->setProperties(getProperties(remoteCharacteristic));
    }
  }
  connected = true;
  statusMessage = "Connected to '" + connectedDeviceName + "'";
  restoreBLENotifications();
  DirConManager::subscribeClientsToTrainer();
  DirConManager::restoreTrainerTarget();
  everConnected = true;
  return true;
}

void BTDeviceManager::restoreBLENotifications() {
  if (serviceManager == nullptr) {
    return;
  }
  for (Service* service : serviceManager->getServices()) {
    if (service == nullptr || service->isInternal()) {
      continue;
    }
    for (Characteristic* characteristic : service->getCharacteristics()) {
      if (characteristic != nullptr && !characteristic->getSubscriptions().empty()) {
        changeBLENotify(characteristic, false);
      }
    }
  }
}

void BTDeviceManager::startScan() {
  if (!started) {
    return;
  }
  if (connectTimer) {
    esp_timer_stop(connectTimer);
  }
  if (scanTimer) {
    esp_timer_stop(scanTimer);
    esp_timer_start_once(scanTimer, BLE_SCAN_INTERVAL * 1000);
  }
}

void BTDeviceManager::stopScan() {
  log_i("Stopping BLE scan...");
  if (scanTimer) {
    esp_timer_stop(scanTimer);
  }
  if (NimBLEDevice::getScan() != nullptr) {
    NimBLEDevice::getScan()->stop();
  }
  scanActive = false;
}

bool BTDeviceManager::doScan() {
  if (!started) {
    log_e("BLE scan start failed");
    return false;
  }
  NimBLEScan* scanner = NimBLEDevice::getScan();
  log_i("Starting BLE scan...");
  if (nimBLEClient != nullptr && nimBLEClient->isConnected()) {
    if (connected) {
      log_i("Trainer already connected, skipping scan");
      return false;
    }
    log_i("BLE link is up, finishing service discovery instead of scanning");
    scheduleConnect(100);
    return false;
  }
  scanner->setScanCallbacks(&scanCallbacks, false);
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(userScan ? 100 : 90);
  scanner->setScanResponseTimeout(userScan ? 1500 : 800);
  scanner->setMaxResults(0);
  esp_coex_preference_set(ESP_COEX_PREFER_BT);
  uint32_t duration = userScan ? 10000 : 5000;
  if (scanner->start(duration, false, true)) {
    scanActive = true;
    statusMessage = "Scanning...";
    log_i("BLE scan started successfully");
    return true;
  }
  scanActive = false;
  log_e("BLE scan start failed");
  if (scanTimer) {
    esp_timer_start_once(scanTimer, BLE_SCAN_INTERVAL * 1000);
  }
  return false;
}

void BTDeviceManager::onScanFinished() {
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  if (NimBLEDevice::getScan() != nullptr && NimBLEDevice::getScan()->isScanning()) {
    return;
  }
  scanActive = false;
  if (scanAbort) {
    log_i("BLE scan aborted for a new search");
    return;
  }
  bool wasUserScan = userScan;
  userScan = false;
  suppressReconnect = false;
  int count = 0;
  if (deviceMutex != nullptr && xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    count = (int)scannedDevices.size();
    xSemaphoreGive(deviceMutex);
  }
  log_i("BLE scan finished with %d devices", count);
  if (connected) {
    return;
  }
  ScannedBleDevice trainer;
  if (resolveTrainer(trainer)) {
    int64_t now = esp_timer_get_time();
    if (now < nextConnectAllowedUs) {
      uint32_t delayMs = (uint32_t)((nextConnectAllowedUs - now) / 1000ULL);
      if (delayMs < BLE_CONNECT_INTERVAL) {
        delayMs = BLE_CONNECT_INTERVAL;
      }
      scheduleConnect(delayMs);
      return;
    }
    scheduleConnect();
    return;
  }
  if (wasUserScan) {
    if (scanTimer) {
      esp_timer_stop(scanTimer);
      esp_timer_start_once(scanTimer, 4000 * 1000);
    }
    return;
  }
  startScan();
}

bool BTDeviceManager::doConnect() {
  log_i("Starting BLE connection...");
  if (!started) {
    log_e("BLE connection failed: Bluetooth device manager not started");
    return false;
  }
  if (connectInProgress) {
    return false;
  }
  if (nimBLEClient != nullptr && nimBLEClient->isConnected()) {
    if (connected) {
      return true;
    }
    log_i("BLE link already up, discovering services");
  }
  connectInProgress = true;
  suppressReconnect = false;
  if (!lastTrainerName.empty()) {
    statusMessage = (everConnected ? "Reconnecting to '" : "Connecting to '") + lastTrainerName + "'";
  } else {
    statusMessage = "Connecting...";
  }
  ScannedBleDevice device;
  bool ok = resolveTrainer(device) && connectRemoteDevice(device);
  connectInProgress = false;
  if (ok) {
    reconnectAttempts = 0;
    nextConnectAllowedUs = 0;
    return true;
  }
  reconnectAttempts++;
  uint32_t cooldown = BLE_CONNECT_COOLDOWN_MS;
  if (reconnectAttempts > 1) {
    cooldown = BLE_CONNECT_COOLDOWN_MS * (reconnectAttempts > 4 ? 4 : reconnectAttempts);
  }
  nextConnectAllowedUs = esp_timer_get_time() + (int64_t)cooldown * 1000;
  if (everConnected && !lastTrainerAddress.isNull() && !remoteDeviceName.empty() &&
      (reconnectAttempts % BLE_RECONNECT_SCAN_EVERY) != 0) {
    scheduleReconnect();
    return false;
  }
  log_i("Connect failed, scanning again in %u ms", cooldown);
  startScan();
  return false;
}

uint32_t BTDeviceManager::getProperties(NimBLERemoteCharacteristic* remoteCharacteristic) {
  uint32_t returnValue = 0;
  if (remoteCharacteristic->canRead()) {
    returnValue |= PROP_READ;
  }
  if (remoteCharacteristic->canWrite() || remoteCharacteristic->canWriteNoResponse()) {
    returnValue |= PROP_WRITE;
  }
  if (remoteCharacteristic->canIndicate() || remoteCharacteristic->canNotify() || remoteCharacteristic->canBroadcast()) {
    returnValue |= PROP_NOTIFY;
  }
  return returnValue;
}

bool BTDeviceManager::writeBLECharacteristic(const NimBLEUUID& serviceUUID, const NimBLEUUID& characteristicUUID,
                                            std::vector<uint8_t>* data) {
  if (!connected || nimBLEClient == nullptr) {
    log_e("BLE characteristic write failed: Not connected");
    return false;
  }
  NimBLERemoteService* remoteService = nimBLEClient->getService(serviceUUID);
  if (remoteService == nullptr) {
    log_e("BLE characteristic write failed: Remote service not found");
    return false;
  }
  NimBLERemoteCharacteristic* remoteCharacteristic = remoteService->getCharacteristic(characteristicUUID);
  if (remoteCharacteristic == nullptr || !remoteCharacteristic->canWrite()) {
    log_e("BLE characteristic write failed: Remote characteristic not writable");
    return false;
  }
  return remoteCharacteristic->writeValue(data->data(), data->size(), true);
}

std::vector<uint8_t> BTDeviceManager::readBLECharacteristic(const NimBLEUUID& serviceUUID,
                                                           const NimBLEUUID& characteristicUUID) {
  std::vector<uint8_t> returnData;
  if (!connected || nimBLEClient == nullptr) {
    log_e("BLE characteristic read failed: Not connected");
    return returnData;
  }
  NimBLERemoteService* remoteService = nimBLEClient->getService(serviceUUID);
  if (remoteService == nullptr) {
    log_e("BLE characteristic read failed: Remote service not found");
    return returnData;
  }
  NimBLERemoteCharacteristic* remoteCharacteristic = remoteService->getCharacteristic(characteristicUUID);
  if (remoteCharacteristic == nullptr || !remoteCharacteristic->canRead()) {
    log_e("BLE characteristic read failed: Remote characteristic not readable");
    return returnData;
  }
  NimBLEAttValue value = remoteCharacteristic->readValue();
  returnData.assign(value.data(), value.data() + value.size());
  return returnData;
}

bool BTDeviceManager::changeBLENotify(Characteristic* characteristic, bool remove) {
  if (!connected || nimBLEClient == nullptr) {
    log_e("BLE characteristic change notify failed: Not connected");
    return false;
  }
  NimBLERemoteService* remoteService = nimBLEClient->getService(characteristic->getService()->UUID);
  if (remoteService == nullptr) {
    log_e("BLE characteristic change notify failed: Remote service not found");
    return false;
  }
  NimBLERemoteCharacteristic* remoteCharacteristic = remoteService->getCharacteristic(characteristic->UUID);
  if (remoteCharacteristic == nullptr || !(remoteCharacteristic->canNotify() || remoteCharacteristic->canIndicate())) {
    log_e("BLE characteristic change notify failed: Remote characteristic cannot notify");
    return false;
  }
  if (!remove) {
    return remoteCharacteristic->subscribe(true, onBLENotify);
  }
  return remoteCharacteristic->unsubscribe();
}

bool BTDeviceManager::writeFECTargetPower(uint16_t targetPower) {
  std::vector<uint8_t> fecData{0xA4, 0x09, 0x4E, 0x05, 0x31, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, (uint8_t)targetPower,
                               (uint8_t)(targetPower >> 8)};
  fecData.push_back(getFECChecksum(&fecData));
  return writeBLECharacteristic(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID), NimBLEUUID(TACX_FEC_WRITE_CHARACTERISTIC_UUID),
                                &fecData);
}

bool BTDeviceManager::writeFECTrackResistance(uint16_t grade, uint8_t rollingResistance) {
  std::vector<uint8_t> fecData{0xA4, 0x09, 0x4E, 0x05, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, (uint8_t)grade,
                               (uint8_t)(grade >> 8), rollingResistance};
  fecData.push_back(getFECChecksum(&fecData));
  return writeBLECharacteristic(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID), NimBLEUUID(TACX_FEC_WRITE_CHARACTERISTIC_UUID),
                                &fecData);
}

bool BTDeviceManager::writeFECBasicResistance(uint8_t totalResistance) {
  std::vector<uint8_t> fecData{0xA4, 0x09, 0x4E, 0x05, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, totalResistance};
  fecData.push_back(getFECChecksum(&fecData));
  return writeBLECharacteristic(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID), NimBLEUUID(TACX_FEC_WRITE_CHARACTERISTIC_UUID),
                                &fecData);
}

bool BTDeviceManager::writeFECUserConfiguration(uint16_t bicycleWeight, uint16_t userWeight, uint8_t wheelDiameter,
                                                uint8_t gearRatio) {
  std::vector<uint8_t> fecData{0xA4,
                               0x09,
                               0x4F,
                               0x05,
                               0x37,
                               (uint8_t)userWeight,
                               (uint8_t)(userWeight >> 8),
                               0xFF,
                               (uint8_t)(bicycleWeight << 4),
                               (uint8_t)(bicycleWeight >> 8),
                               wheelDiameter,
                               gearRatio};
  fecData.push_back(getFECChecksum(&fecData));
  return writeBLECharacteristic(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID), NimBLEUUID(TACX_FEC_WRITE_CHARACTERISTIC_UUID),
                                &fecData);
}

bool BTDeviceManager::writeFECCapabilitiesRequest() {
  std::vector<uint8_t> fecData{0xA4, 0x09, 0x4F, 0x05, 0x46, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x36, 0x01};
  fecData.push_back(getFECChecksum(&fecData));
  return writeBLECharacteristic(NimBLEUUID(TACX_FEC_PRIMARY_SERVICE_UUID), NimBLEUUID(TACX_FEC_WRITE_CHARACTERISTIC_UUID),
                                &fecData);
}

uint8_t BTDeviceManager::getFECChecksum(std::vector<uint8_t>* fecData) {
  uint8_t checksum = 0;
  if (!fecData->empty()) {
    checksum = fecData->at(0);
    for (size_t checksumIndex = 1; checksumIndex < fecData->size(); checksumIndex++) {
      checksum = checksum ^ fecData->at(checksumIndex);
    }
  }
  return checksum;
}

std::string BTDeviceManager::getStatusMessage() { return statusMessage; }
