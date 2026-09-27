#pragma once

#include "Config.h"
#include "NimBLEDevice.h"
#include "ServiceManager.h"

#include <string>
#include <vector>

struct ScannedBleDevice {
  NimBLEAddress address;
  std::string name;
};

class BTDeviceManager {
 public:
  static bool start();
  static void stop();
  static void update();
  static void setLocalDeviceName(const std::string& localDeviceName);
  static void setRemoteDeviceNameFilter(const std::string& remoteDeviceNameFilter);
  static void setServiceManager(ServiceManager* serviceManager);
  static bool isConnected();
  static std::string getConnecedDeviceName();
  static bool writeBLECharacteristic(const NimBLEUUID& serviceUUID, const NimBLEUUID& characteristicUUID,
                                     std::vector<uint8_t>* data);
  static std::vector<uint8_t> readBLECharacteristic(const NimBLEUUID& serviceUUID, const NimBLEUUID& characteristicUUID);
  static bool writeFECTargetPower(uint16_t targetPower);
  static bool writeFECTrackResistance(uint16_t grade, uint8_t rollingResistance = 0xFF);
  static bool writeFECBasicResistance(uint8_t totalResistance);
  static bool writeFECUserConfiguration(uint16_t bicycleWeight, uint16_t userWeight, uint8_t wheelDiameter,
                                        uint8_t gearRatio);
  static bool writeFECCapabilitiesRequest();
  static std::vector<std::string> getScannedDeviceNames();
  static void requestRescan();
  static bool isScanning();
  static std::string getStatusMessage();

 private:
  friend class BTAdvertisedDeviceCallbacks;
  friend class BTClientCallbacks;
  friend class BTDeviceServiceManagerCallbacks;
  static std::string deviceName;
  static std::string remoteDeviceName;
  static std::string connectedDeviceName;
  static bool connected;
  static bool started;
  static ServiceManager* serviceManager;
  static NimBLEClient* nimBLEClient;
  static NimBLEAddress lastTrainerAddress;
  static std::string lastTrainerName;
  static std::vector<ScannedBleDevice> scannedDevices;
  static void dropClient();
  static void terminatePeer(const NimBLEAddress& address);
  static bool waitForLink(uint32_t timeoutMs);
  static bool discoverTrainerServices();
  static bool copyRemoteDevice(ScannedBleDevice& out);
  static bool resolveTrainer(ScannedBleDevice& out);
  static void rememberDevice(const NimBLEAddress& address, const std::string& name);
  static bool connectRemoteDevice(const ScannedBleDevice& remoteDevice);
  static void onScanFinished();
  static void startScan();
  static void stopScan();
  static void scheduleConnect(uint32_t delayMs = BLE_CONNECT_INTERVAL);
  static void scheduleReconnect();
  static void handleLinkLoss();
  static void restoreBLENotifications();
  static bool doScan();
  static bool doConnect();
  friend void scanTimerCb(void*);
  friend void connectTimerCb(void*);
  static uint32_t getProperties(NimBLERemoteCharacteristic* remoteCharacteristic);
  static void onBLENotify(NimBLERemoteCharacteristic* characteristic, uint8_t* pData, size_t length, bool isNotify);
  static bool changeBLENotify(Characteristic* characteristic, bool remove);
  static uint8_t getFECChecksum(std::vector<uint8_t>* fecData);
  static std::string statusMessage;
};
