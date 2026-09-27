#pragma once

#include "Config.h"
#include "DirConMessage.h"
#include "SettingsManager.h"

class ServiceManager;
class Service;
class Characteristic;

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#define DIRCON_MDNS_SERVICE_NAME "_wahoo-fitness-tnp"
#define DIRCON_MDNS_SERVICE_PROTOCOL "tcp"

enum TrainerMode {
  ERG_MODE = 0,
  SIM_MODE = 1,
  SIM_MODE_VIRTUAL_SHIFTING = 2,
};

class DirConClient {
 public:
  int fd = -1;
  bool alive = false;
  char ip[48] = {};
  bool connected() const { return alive && fd >= 0; }
  void write(const char* data, size_t len);
  void stop();
  void abort() { stop(); }
  const char* remote() const { return ip; }
};

class DirConManager {
 public:
  static bool start();
  static void stop();
  static void setServiceManager(ServiceManager* serviceManager);
  static void notifyDirConCharacteristic(const NimBLEUUID& characteristicUUID, uint8_t* pData, size_t length);
  static std::string getStatusMessage();
  static TrainerMode getZwiftTrainerMode();
  static void subscribeClientsToTrainer();
  static void restoreTrainerTarget();
  static void updateAdvertisedServiceUUIDs();
  static void onPowerOffsetChanged();
  static uint16_t getRawPower();
  static uint16_t getReportedPower();

 private:
  friend class DirConServiceManagerCallbacks;
  static bool doNotifications(void* arg);
  static void handleNewClient(DirConClient* client);
  static void handleDirConData(DirConClient* client, void* data, size_t len);
  static void closeClient(DirConClient* client, const char* reason);
  static void removeSubscriptions(DirConClient* client);
  static size_t getDirConClientIndex(DirConClient* client);
  static bool processDirConMessage(DirConMessage* dirConMessage, DirConClient* client, size_t clientIndex);
  static uint8_t getDirConProperties(uint32_t characteristicProperties);
  static bool processZwiftSyncRequest(Service* service, Characteristic* characteristic, std::vector<uint8_t>* requestData);
  static void notifyDirConCharacteristic(Characteristic* characteristic, uint8_t* pData, size_t length);
  static void notifyInternalCharacteristics();
  static std::vector<uint8_t> generateZwiftAsyncNotificationData(int64_t power, int64_t cadence, int64_t unknown1,
                                                                 int64_t unknown2, int64_t unknown3,
                                                                 int64_t unknown4 = 25714LL);
  static std::map<uint8_t, uint64_t> getZwiftDataValues(std::vector<uint8_t>* requestData);
  static void sendDirConCharacteristicNotification(const NimBLEUUID& characteristicUUID, uint8_t* pData, size_t length,
                                                   bool onlySubscribers);
  static void sendDirConCharacteristicNotification(Characteristic* characteristic, uint8_t* pData, size_t length,
                                                   bool onlySubscribers);
  static void updateStatusMessage();
  static void resetValues();
  static void adjustPowerPayload(const NimBLEUUID& uuid, std::vector<uint8_t>& data);
  static void updateSIMModeResistance();
  static std::vector<uint8_t> processFTMSReadRequest(Service* service, Characteristic* characteristic,
                                                     std::vector<uint8_t>* requestData);
  static std::vector<uint8_t> processFTMSWriteRequest(Service* service, Characteristic* characteristic,
                                                      std::vector<uint8_t>* requestData);
  static void sendFTMSControlPointFollowUp(Characteristic* controlPointCharacteristic, const std::vector<uint8_t>& response);
  static std::vector<uint8_t> generateIndoorBikeDataNotificationData(uint16_t instantaneousSpeed,
                                                                     uint16_t instantaneousCadence,
                                                                     int16_t instantaneousPower);
  static void serverTask(void* arg);

  static ServiceManager* serviceManager;
  static DirConClient dirConClients[DIRCON_MAX_CLIENTS];
  static int listenFd;
  static bool started;
  static std::string statusMessage;
  static uint8_t zwiftAsyncRideOnAnswer[];
  static uint8_t zwiftSyncRideOnAnswer[];

  static double defaultGearRatio;
  static VirtualShiftingMode virtualShiftingMode;
  static TrainerMode zwiftTrainerMode;
  static uint64_t zwiftPower;
  static int64_t zwiftGrade;
  static uint64_t zwiftGearRatio;
  static uint16_t zwiftBicycleWeight;
  static uint16_t zwiftUserWeight;
  static int64_t smoothedZwiftGrade;
  static uint16_t trainerInstantaneousPower;
  static uint16_t trainerRawPower;
  static uint16_t trainerInstantaneousSpeed;
  static uint8_t trainerCadence;
  static uint16_t trainerMaximumResistance;
  static uint16_t difficulty;
  static int16_t ftmsPower;
  static int16_t ftmsWindSpeed;
  static int16_t ftmsGrade;
  static uint8_t ftmsCrr;
  static uint8_t ftmsCw;
};
