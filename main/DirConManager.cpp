#include "DirConManager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "BTDeviceManager.h"
#include "Calculations.h"
#include "Config.h"
#include "FTMS.h"
#include "Service.h"
#include "SettingsManager.h"
#include "UUIDs.h"
#include "Utils.h"
#include "gatt_props.h"
#include "mdns.h"
#include "app_log.h"
#include "uleb128.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>

ServiceManager *DirConManager::serviceManager{};
bool DirConManager::started = false;
std::string DirConManager::statusMessage = "";

DirConClient DirConManager::dirConClients[DIRCON_MAX_CLIENTS];
int DirConManager::listenFd = -1;

void DirConClient::write(const char* data, size_t len) {
  if (!connected() || data == nullptr || len == 0) {
    return;
  }
  size_t sent = 0;
  while (sent < len) {
    int written = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (written <= 0) {
      stop();
      return;
    }
    sent += written;
  }
}

void DirConClient::stop() {
  alive = false;
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

uint8_t DirConManager::zwiftAsyncRideOnAnswer[18] = {0x2a, 0x08, 0x03, 0x12, 0x0d, 0x22, 0x0b, 0x52, 0x49, 0x44, 0x45, 0x5f, 0x4f, 0x4e, 0x28, 0x30, 0x29, 0x00};
uint8_t DirConManager::zwiftSyncRideOnAnswer[8] = {0x52, 0x69, 0x64, 0x65, 0x4f, 0x6e, 0x02, 0x00};

double DirConManager::defaultGearRatio;
VirtualShiftingMode DirConManager::virtualShiftingMode;

TrainerMode DirConManager::zwiftTrainerMode;
uint64_t DirConManager::zwiftPower;
int64_t DirConManager::zwiftGrade;
uint64_t DirConManager::zwiftGearRatio;
uint16_t DirConManager::zwiftBicycleWeight;
uint16_t DirConManager::zwiftUserWeight;

int64_t DirConManager::smoothedZwiftGrade;

static uint16_t fecAccumRaw = 0;
static uint16_t fecAccumScaled = 0;
static bool fecAccumValid = false;

uint16_t DirConManager::trainerInstantaneousPower;
uint16_t DirConManager::trainerRawPower;
uint16_t DirConManager::trainerInstantaneousSpeed;
uint8_t DirConManager::trainerCadence;
uint16_t DirConManager::trainerMaximumResistance;

uint16_t DirConManager::difficulty;

int16_t DirConManager::ftmsPower;
int16_t DirConManager::ftmsWindSpeed;
int16_t DirConManager::ftmsGrade;
uint8_t DirConManager::ftmsCrr;
uint8_t DirConManager::ftmsCw;

class DirConServiceManagerCallbacks : public ServiceManagerCallbacks {
  void onServiceAdded(Service *service) {
    if (service->isAdvertised()) {
      DirConManager::updateAdvertisedServiceUUIDs();
    };
  };

  void onCharacteristicSubscriptionChanged(Characteristic *characteristic, bool removed) {
    if (characteristic->UUID.equals(NimBLEUUID(CYCLING_POWER_MEASUREMENT_CHARACTERISTIC_UUID))) {
      if (removed && (characteristic->getSubscriptions().size() == 0)) {
        DirConManager::resetValues();
      } 
    }
  };
};

void DirConManager::resetValues() {
  defaultGearRatio = (double)SettingsManager::getChainringTeeth() / (double)SettingsManager::getSprocketTeeth();
  virtualShiftingMode = SettingsManager::getVirtualShiftingMode();
  zwiftTrainerMode = TrainerMode::SIM_MODE;
  zwiftPower = 0;
  zwiftGrade = 0;
  zwiftGearRatio = 0;
  zwiftBicycleWeight = 1000;
  zwiftUserWeight = 7500;

  smoothedZwiftGrade = 0;
  
  trainerInstantaneousPower = 0;
  trainerRawPower = 0;
  fecAccumValid = false;
  trainerInstantaneousSpeed = 0;
  trainerCadence = 0;
  trainerMaximumResistance = 0;

  difficulty = SettingsManager::getDifficulty();

  ftmsPower = 0;
  ftmsWindSpeed = 0;
  ftmsGrade = 0;
  ftmsCrr = 0;
  ftmsCw = 0;
}

void DirConManager::setServiceManager(ServiceManager *serviceManager) {
  DirConManager::serviceManager = serviceManager;
}

// DirCon clients read this TXT record while browsing, so it has to carry the advertised
// services as full 128 bit UUIDs. An empty value is dropped instead of published as a bare key.
void DirConManager::updateAdvertisedServiceUUIDs() {
  if (serviceManager == nullptr) {
    return;
  }
  std::string serviceUUIDs;
  for (Service *currentService : serviceManager->getServices()) {
    if (!currentService->isAdvertised()) {
      continue;
    }
    if (!serviceUUIDs.empty()) {
      serviceUUIDs += ",";
    }
    serviceUUIDs += currentService->UUID.to128().toString();
  }
  if (serviceUUIDs.empty()) {
    mdns_service_txt_item_remove("_wahoo-fitness-tnp", "_tcp", "ble-service-uuids");
    return;
  }
  mdns_service_txt_item_set("_wahoo-fitness-tnp", "_tcp", "ble-service-uuids", serviceUUIDs.c_str());
  log_i("Advertising DirCon services: %s", serviceUUIDs.c_str());
}

bool DirConManager::start() {
  if (!started) {
    if (serviceManager == nullptr) {
      return false;
    }
    resetValues();
    serviceManager->subscribeCallbacks(new DirConServiceManagerCallbacks);
    mdns_service_add(Utils::getDeviceName().c_str(), "_wahoo-fitness-tnp", "_tcp", DIRCON_TCP_PORT, NULL, 0);
    mdns_service_txt_item_set("_wahoo-fitness-tnp", "_tcp", "mac-address", Utils::getMacAddressString().c_str());
    mdns_service_txt_item_set("_wahoo-fitness-tnp", "_tcp", "serial-number", Utils::getSerialNumberString().c_str());
    updateAdvertisedServiceUUIDs();
    started = true;
    if (xTaskCreate(serverTask, "dircon", 8192, nullptr, 5, nullptr) != pdPASS) {
      started = false;
      return false;
    }
    updateStatusMessage();
    return true;
  }
  return false;
}

void DirConManager::stop() {
  started = false;
  resetValues();
  if (listenFd >= 0) {
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
    listenFd = -1;
  }
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    dirConClients[clientIndex].stop();
  }
}

bool DirConManager::doNotifications(void *arg) {
  if (started) {
    notifyInternalCharacteristics();
  }
  return true;
}

void DirConManager::handleNewClient(DirConClient *client) {
  bool clientAccepted = false;
  size_t clientAcceptedIndex = 0;
  log_i("New DirCon connection from %s", client->remote());
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    if (&dirConClients[clientIndex] == client) {
      clientAccepted = true;
      clientAcceptedIndex = clientIndex;
      // enable notifications for FE-C and internal characteristics if virtual shifting or FTMS emulation is enabled
      if (SettingsManager::isVirtualShiftingEnabled() || SettingsManager::isFTMSEnabled()) {
        Characteristic* tacxFECReadCharacteristic = serviceManager->getCharacteristic(NimBLEUUID(TACX_FEC_READ_CHARACTERISTIC_UUID));
        if (tacxFECReadCharacteristic != nullptr) {
          tacxFECReadCharacteristic->addSubscription(clientIndex);
        } else {
          // The trainer may still be asleep. Keep the client so it can discover the emulated
          // services, the FE-C subscription is added once the trainer connects.
          log_w("FE-C characteristic not available yet, client keeps its connection");
        }
        Characteristic* zwiftAsyncCharacteristic = serviceManager->getCharacteristic(NimBLEUUID(ZWIFT_ASYNC_CHARACTERISTIC_UUID));
        if (zwiftAsyncCharacteristic != nullptr) {
          zwiftAsyncCharacteristic->addSubscription(clientIndex);
        }
        Characteristic* indoorBikeDataCharacteristic = serviceManager->getCharacteristic(NimBLEUUID(INDOOR_BIKE_DATA_CHARACTERISTIC_UUID));
        if (indoorBikeDataCharacteristic != nullptr) {
          indoorBikeDataCharacteristic->addSubscription(clientIndex);
        }
      }
      // send the FE-C request to receive the capabilities
      if (BTDeviceManager::isConnected() && !BTDeviceManager::writeFECCapabilitiesRequest()) {
        log_e("Error writing FEC capabilities request");
      }
      break;
    }
  }
  updateStatusMessage();
  if (clientAccepted) {
    log_i("Free connection slot found (%d), DirCon connection from %s accepted", clientAcceptedIndex, client->remote());
  } else {
    log_e("Maximum number of %d connections reached, DirCon connection from %s rejected", DIRCON_MAX_CLIENTS, client->remote());
    client->abort();
  }
}

void DirConManager::handleDirConData(DirConClient *client, void *data, size_t len) {
  size_t clientIndex = DirConManager::getDirConClientIndex(client);
  if (clientIndex != (DIRCON_MAX_CLIENTS + 1)) {
    DirConMessage currentMessage;
    size_t parsedBytes = 0;
    while (parsedBytes < len) {
      parsedBytes += currentMessage.parse((uint8_t *)data + parsedBytes, (len - parsedBytes), 0);
      if (currentMessage.Identifier == DIRCON_MSGID_ERROR) {
        log_e("Error handling data from DirCon client: Unable to parse DirCon message");
        parsedBytes += 1;
        if ((len - parsedBytes) <= 0) {
          return;
        }
      }
      if (!DirConManager::processDirConMessage(&currentMessage, client, clientIndex)) {
        log_e("Error handling data from DirCon client: Unable to process DirCon message");
      }
    }
  } else {
    log_e("Error handling data from DirCon client: Client slot not found");
  }
}

void DirConManager::closeClient(DirConClient *client, const char* reason) {
  log_i("DirCon client %s: %s", client->remote(), reason);
  removeSubscriptions(client);
  client->stop();
  updateStatusMessage();
}

void DirConManager::removeSubscriptions(DirConClient *client) {
  size_t clientIndex = DirConManager::getDirConClientIndex(client);
  if (clientIndex != (DIRCON_MAX_CLIENTS + 1)) {
    for (Service *service : serviceManager->getServices()) {
      for (Characteristic *characteristic : service->getCharacteristics()) {
        characteristic->removeSubscription(clientIndex);
        if (characteristic->UUID.equals(NimBLEUUID(TACX_FEC_READ_CHARACTERISTIC_UUID))) {
          if (characteristic->getSubscriptions().size() == 0) {
            // if no client is connected anymore set default SIM mode on trainer
            zwiftTrainerMode = TrainerMode::SIM_MODE;
            ftmsPower = 0;
            BTDeviceManager::writeFECTrackResistance(0x4E20);
          }
        }
      }
    }
  }
  updateStatusMessage();
}

void DirConManager::subscribeClientsToTrainer() {
  if (serviceManager == nullptr || (!SettingsManager::isVirtualShiftingEnabled() && !SettingsManager::isFTMSEnabled())) {
    return;
  }
  Characteristic *tacxFECReadCharacteristic = serviceManager->getCharacteristic(NimBLEUUID(TACX_FEC_READ_CHARACTERISTIC_UUID));
  if (tacxFECReadCharacteristic == nullptr) {
    return;
  }
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    if (dirConClients[clientIndex].connected() && !tacxFECReadCharacteristic->isSubscribed(clientIndex)) {
      log_i("Trainer available, enabling FE-C notifications for DirCon client %d", clientIndex);
      tacxFECReadCharacteristic->addSubscription(clientIndex);
      if (!BTDeviceManager::writeFECCapabilitiesRequest()) {
        log_e("Error writing FEC capabilities request");
      }
    }
  }
}

size_t DirConManager::getDirConClientIndex(DirConClient *client) {
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    if (&dirConClients[clientIndex] == client) {
      return clientIndex;
    }
  }
  return DIRCON_MAX_CLIENTS + 1;
}

bool DirConManager::processDirConMessage(DirConMessage *dirConMessage, DirConClient *client, size_t clientIndex) {
  Service *service = nullptr;
  Characteristic *characteristic = nullptr;
  Characteristic *ftmsControlPointCharacteristic = nullptr;
  std::vector<uint8_t> *returnData;
  DirConMessage returnMessage;
  returnMessage.Identifier = dirConMessage->Identifier;
  returnMessage.UUID = dirConMessage->UUID;
  switch (dirConMessage->Identifier) {
    case DIRCON_MSGID_DISCOVER_SERVICES:
      returnMessage.ResponseCode = DIRCON_RESPCODE_SUCCESS_REQUEST;
      for (Service *currentService : serviceManager->getServices()) {
        returnMessage.AdditionalUUIDs.push_back(currentService->UUID);
      }
      break;
    case DIRCON_MSGID_DISCOVER_CHARACTERISTICS:
      returnMessage.ResponseCode = DIRCON_RESPCODE_SUCCESS_REQUEST;
      service = serviceManager->getService(dirConMessage->UUID);
      if (service == nullptr) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_SERVICE_NOT_FOUND;
        break;
      }
      for (Characteristic *characteristic : service->getCharacteristics()) {
        returnMessage.AdditionalUUIDs.push_back(characteristic->UUID);
        returnMessage.AdditionalData.push_back(getDirConProperties(characteristic->getProperties()));
      }
      break;
    case DIRCON_MSGID_ENABLE_CHARACTERISTIC_NOTIFICATIONS:
      returnMessage.ResponseCode = DIRCON_RESPCODE_SUCCESS_REQUEST;
      characteristic = serviceManager->getCharacteristic(dirConMessage->UUID);
      if (characteristic == nullptr) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_NOT_FOUND;
        break;
      }
      if (dirConMessage->AdditionalData.size() != 1) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
        break;
      }
      if (dirConMessage->AdditionalData.at(0) == 0) {
        if (characteristic->isSubscribed(clientIndex)) {
          characteristic->removeSubscription(clientIndex);
        }
      } else {
        if (!characteristic->isSubscribed(clientIndex)) {
          characteristic->addSubscription(clientIndex);
        }
      }
      break;
    case DIRCON_MSGID_WRITE_CHARACTERISTIC:
      returnMessage.ResponseCode = DIRCON_RESPCODE_SUCCESS_REQUEST;
      characteristic = serviceManager->getCharacteristic(dirConMessage->UUID);
      if (characteristic == nullptr) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_NOT_FOUND;
        break;
      }
      if (dirConMessage->AdditionalData.size() == 0) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
        break;
      }
      if (characteristic->getService() != nullptr) {
        if (!characteristic->getService()->isInternal()) {
          if (!BTDeviceManager::writeBLECharacteristic(characteristic->getService()->UUID, characteristic->UUID, &(dirConMessage->AdditionalData))) {
            returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
            break;
          }
        } else {
          if (characteristic->UUID.equals(NimBLEUUID(ZWIFT_SYNCRX_CHARACTERISTIC_UUID))) {
            if (!processZwiftSyncRequest(characteristic->getService(), characteristic, &dirConMessage->AdditionalData)) {
              returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
              break;
            }
          }
          if (characteristic->UUID.equals(NimBLEUUID(FITNESS_MACHINE_CONTROL_POINT_CHARACTERISTIC_UUID))) {
            returnMessage.AdditionalData = processFTMSWriteRequest(characteristic->getService(), characteristic, &dirConMessage->AdditionalData);
            if (returnMessage.AdditionalData.size() == 0) {
              returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
              break;
            }
            ftmsControlPointCharacteristic = characteristic;
          }
          break;
        }
      } else {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_NOT_FOUND;
        break;
      }
      break;
    case DIRCON_MSGID_READ_CHARACTERISTIC:
      returnMessage.ResponseCode = DIRCON_RESPCODE_SUCCESS_REQUEST;
      characteristic = serviceManager->getCharacteristic(dirConMessage->UUID);
      if (characteristic == nullptr) {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_NOT_FOUND;
        break;
      }
      if (characteristic->getService() != nullptr) {
        if (!characteristic->getService()->isInternal()) {
          returnMessage.AdditionalData = BTDeviceManager::readBLECharacteristic(characteristic->getService()->UUID, characteristic->UUID);
          if (returnMessage.AdditionalData.size() == 0) {
            returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
            break;
          }
          adjustPowerPayload(characteristic->UUID, returnMessage.AdditionalData);
        } else {
          if (characteristic->getService()->UUID.equals(NimBLEUUID(FITNESS_MACHINE_SERVICE_UUID))) {
            returnMessage.AdditionalData = processFTMSReadRequest(characteristic->getService(), characteristic, &dirConMessage->AdditionalData);
            if (returnMessage.AdditionalData.size() == 0) {
              returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_OPERATION_NOT_SUPPORTED;
              break;
            }
          } else {
            log_e("DirCon read to internal characteristic %s not implemented!", characteristic->UUID.toString().c_str());
          }
        }
      } else {
        returnMessage.ResponseCode = DIRCON_RESPCODE_CHARACTERISTIC_NOT_FOUND;
        break;
      }

      break;
    default:
      log_e("Unknown identifier %d in DirCon message, skipping further processing", dirConMessage->Identifier);
      return false;
      break;
  }
  returnData = returnMessage.encode(dirConMessage->SequenceNumber);
  if (returnData->size() > 0) {
    client->write((char *)returnData->data(), returnData->size());
  } else {
    log_e("Error encoding DirCon message, aborting");
    return false;
  }

  if (ftmsControlPointCharacteristic != nullptr) {
    sendFTMSControlPointFollowUp(ftmsControlPointCharacteristic, returnMessage.AdditionalData);
  }

  return true;
}

// FTMS clients such as MyWhoosh wait for the control point response as an indication on 0x2AD9
// and only then send the next command. The DirCon write response alone does not count.
void DirConManager::sendFTMSControlPointFollowUp(Characteristic *controlPointCharacteristic, const std::vector<uint8_t> &response) {
  std::vector<uint8_t> indication = response;
  sendDirConCharacteristicNotification(controlPointCharacteristic, indication.data(), indication.size(), false);

  if (response.size() < 3 || response.at(2) != FITNESS_MACHINE_CONTROL_POINT_RESULT_CODE::success) {
    return;
  }
  std::vector<uint8_t> status;
  switch (response.at(1)) {
    case 0x01:
      status.push_back(0x01);
      break;
    case 0x05:
      status.push_back(0x08);
      status.push_back((uint8_t)(ftmsPower & 0xFF));
      status.push_back((uint8_t)(ftmsPower >> 8));
      break;
    case 0x11:
      status.push_back(0x12);
      status.push_back((uint8_t)(ftmsWindSpeed & 0xFF));
      status.push_back((uint8_t)(ftmsWindSpeed >> 8));
      status.push_back((uint8_t)(ftmsGrade & 0xFF));
      status.push_back((uint8_t)(ftmsGrade >> 8));
      status.push_back(ftmsCrr);
      status.push_back(ftmsCw);
      break;
    default:
      return;
  }
  sendDirConCharacteristicNotification(NimBLEUUID(FITNESS_MACHINE_STATUS_CHARACTERISTIC_UUID), status.data(), status.size(), true);
}

void DirConManager::restoreTrainerTarget() {
  if (!SettingsManager::isFTMSEnabled() || zwiftTrainerMode != TrainerMode::ERG_MODE || ftmsPower <= 0) {
    return;
  }
  log_i("Restoring ERG target power of %d W after trainer connect", ftmsPower);
  if (!BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower((uint16_t)(ftmsPower * 4)))) {
    log_e("Error restoring ERG FEC target power");
  }
}

uint8_t DirConManager::getDirConProperties(uint32_t characteristicProperties) {
  uint8_t returnValue = 0x00;
  if ((characteristicProperties & PROP_READ) == PROP_READ) {
    returnValue = returnValue | DIRCON_CHAR_PROP_FLAG_READ;
  }
  if ((characteristicProperties & PROP_WRITE) == PROP_WRITE) {
    returnValue = returnValue | DIRCON_CHAR_PROP_FLAG_WRITE;
  }
  if ((characteristicProperties & PROP_NOTIFY) == PROP_NOTIFY) {
    returnValue = returnValue | DIRCON_CHAR_PROP_FLAG_NOTIFY;
  }
  return returnValue;
}

std::map<uint8_t, uint64_t> DirConManager::getZwiftDataValues(std::vector<uint8_t> *requestData) {
  std::map<uint8_t, uint64_t> returnMap;
  if (requestData->size() > 2) {
    if (requestData->at(0) == 0x04) {
      size_t processedBytes = 0;
      size_t dataIndex = 0;
      uint8_t currentKey = 0;
      uint64_t currentValue = 0;
      if (((requestData->at(1) == 0x22) || (requestData->at(1) == 0x2A)) && requestData->size() > 4) {
        dataIndex = 3;
        if (requestData->size() == (requestData->at(2) + dataIndex)) {
          while (dataIndex < requestData->size()) {
            currentKey = requestData->at(dataIndex);
            dataIndex++;
            processedBytes = bfs::DecodeUleb128(requestData->data() + dataIndex, requestData->size() - dataIndex, &currentValue);
            dataIndex = dataIndex + processedBytes;
            if (processedBytes == 0) {
              log_e("Error parsing unsigned Zwift data values, hex: %s", Utils::getHexString(requestData).c_str());
              dataIndex++;
            }
            returnMap.emplace(currentKey, currentValue);
          }
        } else {
          log_e("Error parsing unsigned Zwift data values, length mismatch");
        }
      } else if (requestData->at(1) == 0x18) {
        dataIndex = 2;
        processedBytes = bfs::DecodeUleb128(requestData->data() + dataIndex, requestData->size() - dataIndex, &currentValue);
        if (processedBytes == 0) {
          log_e("Error parsing unsigned Zwift data value, hex: %s", Utils::getHexString(requestData).c_str());
        } else {
          returnMap.emplace(currentKey, currentValue);
        }
      }
    }
  }
  return returnMap;
}

// @Roberto Viola: This is the part you're probably looking for to implement the virtual shifting correctly in qdomyos-zwift :-)
// The values that are being sent do not specify a specific gear but the ratio from 0.75 to 5.49 in LEB128 encoding.
// 0.75 0.87 0.99 1.11 1.23 1.38 1.53 1.68 1.86 2.04 2.22 2.40 2.61 2.82 3.03 3.24 3.49 3.74 3.99 4.24 4.54 4.84 5.14 5.49
// LEB128 is used nearly everywhere here, feel free to contact me to get more insights.
bool DirConManager::processZwiftSyncRequest(Service *service, Characteristic *characteristic, std::vector<uint8_t> *requestData) {
  std::vector<uint8_t> returnData;
  if (requestData->size() >= 3) {
    uint8_t zwiftCommand = requestData->at(0);
    uint8_t zwiftCommandSubtype = requestData->at(1);
    uint8_t zwiftCommandLength = requestData->at(2);
    std::map<uint8_t, uint64_t> requestValues = getZwiftDataValues(requestData);
    TrainerMode newZwiftTrainerMode = zwiftTrainerMode;
    // for development purposes
    /*
    if ((zwiftCommandSubtype == 0x22) && (zwiftCommandSubtype != 0x18)) {
      log_i("Zwift command: %d, commandsubtype: %d", zwiftCommand, zwiftCommandSubtype);
      for (auto requestValue = requestValues.begin(); requestValue != requestValues.end(); requestValue++) {
        log_i("Zwift sync data key: %d, value %d", requestValue->first, requestValue->second);
      }
    }
    log_i("Zwift request: %s", Utils::getHexString(requestData).c_str());
    */
    switch (zwiftCommand) {
      // Status request
      case 0x00:
        log_i("Zwift 0x00 request, hex value: %s", Utils::getHexString(requestData).c_str());
        // do nothing for the moment but TODO in future
        return true;
        break;

      // Change request (ERG / SIM mode / Gear ratio / ...)
      case 0x04:
        switch (zwiftCommandSubtype) {
          // ERG Mode
          case 0x18:
            if ((zwiftTrainerMode == TrainerMode::SIM_MODE) || (zwiftTrainerMode == TrainerMode::SIM_MODE_VIRTUAL_SHIFTING)) {
              log_i("ERG mode requested");
              zwiftTrainerMode = TrainerMode::ERG_MODE;
              updateStatusMessage();            
            }
            if (requestValues.find(0x00) != requestValues.end()) {
              zwiftPower = requestValues.at(0x00);
            }
            log_i("FEC target power: %d", (zwiftPower * 4));
            // FE-C target power is in 0.25W unit
            if (!BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower((uint16_t)(zwiftPower * 4)))) {
              log_e("Error writing FEC target power");
            }
            break;

          // SIM/ERG Mode Inclination
          // TODO: [04 22 09 10 8D 01 18 EC 27 20 90 03] 5100 / 400 what are these values?!
          case 0x22:
            if (requestValues.find(0x10) != requestValues.end()) {
              zwiftGrade = requestValues.at(0x10); 
              // don't know why but they're using bit 0 for signing...
              if ((zwiftGrade & 0x01) == 0x01) {
                zwiftGrade ^= 0x01;
                zwiftGrade *= -1;
              }
              smoothedZwiftGrade += zwiftGrade;
              smoothedZwiftGrade = smoothedZwiftGrade / 2;
            }
            updateSIMModeResistance();
            break;

          // SIM mode parameter update
          case 0x2A:
            if (requestValues.find(0x10) != requestValues.end()) {
              zwiftGearRatio = requestValues.at(0x10);
            }
            if (zwiftGearRatio == 0) {
              newZwiftTrainerMode = TrainerMode::SIM_MODE;
            } else {
              newZwiftTrainerMode = TrainerMode::SIM_MODE_VIRTUAL_SHIFTING;
            }
            if (zwiftTrainerMode != newZwiftTrainerMode) {
              if (newZwiftTrainerMode == TrainerMode::SIM_MODE) {
                log_i("SIM mode requested");
              } else {
                log_i("SIM + VS mode requested");
              }
              zwiftTrainerMode = newZwiftTrainerMode;
              updateStatusMessage();            
            }
            if (requestValues.find(0x20) != requestValues.end()) {
              zwiftBicycleWeight = requestValues.at(0x20);
              if (requestValues.find(0x28) != requestValues.end()) {
                zwiftUserWeight = requestValues.at(0x28);
                if (!BTDeviceManager::writeFECUserConfiguration((uint16_t)(zwiftBicycleWeight / 5), zwiftUserWeight, (uint8_t)(Calculations::wheelDiameter / 0.01), (uint8_t)round(defaultGearRatio / 0.03))) {
                  log_e("Error writing FEC user configuration");
                }
              }
            }
            updateSIMModeResistance();
            break;

          // Unknown
          default:
            log_e("Unknown Zwift Sync change request with hex value: %s", Utils::getHexString(requestData).c_str());
            for (auto requestValue = requestValues.begin(); requestValue != requestValues.end(); requestValue++) {
              log_i("Zwift sync data key: %d, value %d", requestValue->first, requestValue->second);
            }
            break;
        }

        return true;
        break;

      // Unknown request, similar to 0x00
      case 0x41:
        log_e("Zwift 0x41 request, hex value: %s", Utils::getHexString(requestData).c_str());
        // do nothing for the moment but TODO in future
        return true;
        break;

      // RideOn request
      case 0x52:
        if (requestData->size() == 8) {
          sendDirConCharacteristicNotification(NimBLEUUID(ZWIFT_SYNCTX_CHARACTERISTIC_UUID), zwiftSyncRideOnAnswer, sizeof(zwiftSyncRideOnAnswer), false);
          sendDirConCharacteristicNotification(NimBLEUUID(ZWIFT_ASYNC_CHARACTERISTIC_UUID), zwiftAsyncRideOnAnswer, sizeof(zwiftAsyncRideOnAnswer), false);
          return true;
        }
        break;

      default:
        log_e("Unknown Zwift Sync request with hex value: %s", Utils::getHexString(requestData).c_str());
        break;
    }
  }
  return false;
}

void DirConManager::updateSIMModeResistance() {
  // normal SIM mode w/o virtual shifting, use normal track resistance mode
  if (zwiftTrainerMode == TrainerMode::SIM_MODE) {
    if (!BTDeviceManager::writeFECTrackResistance((uint16_t)(0x4E20 + zwiftGrade), 0x53)) {
      log_e("Error writing FEC track resistance");
    }
  } else if (zwiftTrainerMode == TrainerMode::SIM_MODE_VIRTUAL_SHIFTING) {
    // Target Power Mode
    if (virtualShiftingMode == VirtualShiftingMode::TARGET_POWER) {
      uint16_t trainerTargetPower = Calculations::calculateFECTargetPowerValue(
        (zwiftBicycleWeight + zwiftUserWeight) / 100.0,
        (SettingsManager::isGradeSmoothingEnabled() ? smoothedZwiftGrade : zwiftGrade) / 100.0,
        trainerInstantaneousSpeed / 1000.0,
        trainerCadence,
        zwiftGearRatio / 10000.0,
        defaultGearRatio,
        difficulty);
      if (!BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower(trainerTargetPower))) {
        log_e("Error writing SIM+VS FEC target power");
      }
    
    // Track Resistance Mode
    } else if (virtualShiftingMode == VirtualShiftingMode::TRACK_RESISTANCE) {
      uint16_t trainerTrackResistanceGrade = Calculations::calculateFECTrackResistanceGrade(
        (zwiftBicycleWeight + zwiftUserWeight) / 100.0,
        (SettingsManager::isGradeSmoothingEnabled() ? smoothedZwiftGrade : zwiftGrade) / 100.0,
        trainerInstantaneousSpeed / 1000.0,
        trainerCadence,
        zwiftGearRatio / 10000.0,
        defaultGearRatio,
        difficulty);
      if (!BTDeviceManager::writeFECTrackResistance(trainerTrackResistanceGrade, 0x53)) {
        log_e("Error writing SIM+VS FEC track resistance");
      }

    // Basic Resistance Mode
    } else {
      uint8_t trainerBasicResistance = Calculations::calculateFECBasicResistancePercentageValue(
        (zwiftBicycleWeight + zwiftUserWeight) / 100.0,
        (SettingsManager::isGradeSmoothingEnabled() ? smoothedZwiftGrade : zwiftGrade) / 100.0,
        trainerInstantaneousSpeed / 1000.0,
        trainerCadence,
        zwiftGearRatio / 10000.0,
        defaultGearRatio,
        trainerMaximumResistance,
        difficulty);
      if (!BTDeviceManager::writeFECBasicResistance(trainerBasicResistance)) {
        log_e("Error writing SIM+VS FEC basic resistance");
      }
    }
  }
}

void DirConManager::adjustPowerPayload(const NimBLEUUID& uuid, std::vector<uint8_t>& data) {
  if (uuid.equals(NimBLEUUID(TACX_FEC_READ_CHARACTERISTIC_UUID))) {
    if (data.size() != 13 || data[4] != 0x19) {
      return;
    }
    uint16_t raw = (uint16_t)(((data[10] & 0x0F) << 8) | data[9]);
    uint16_t reported = SettingsManager::scaleReportedWatts(raw);
    if (reported > 4095) {
      reported = 4095;
    }
    trainerRawPower = raw;
    trainerInstantaneousPower = reported;
    if (SettingsManager::getPowerOffsetPercent() == 0) {
      fecAccumValid = false;
      return;
    }
    uint16_t accumulated = (uint16_t)(data[7] | (data[8] << 8));
    if (!fecAccumValid) {
      fecAccumRaw = accumulated;
      fecAccumScaled = SettingsManager::scaleReportedWatts(accumulated);
      fecAccumValid = true;
    } else {
      uint16_t delta = (uint16_t)(accumulated - fecAccumRaw);
      fecAccumRaw = accumulated;
      fecAccumScaled = (uint16_t)(fecAccumScaled + SettingsManager::scaleReportedWatts(delta));
    }
    data[7] = (uint8_t)(fecAccumScaled & 0xFF);
    data[8] = (uint8_t)(fecAccumScaled >> 8);
    data[9] = (uint8_t)(reported & 0xFF);
    data[10] = (uint8_t)((data[10] & 0xF0) | ((reported >> 8) & 0x0F));
    uint8_t checksum = data[0];
    for (size_t index = 1; index + 1 < data.size(); index++) {
      checksum ^= data[index];
    }
    data.back() = checksum;
    return;
  }

  if (!uuid.equals(NimBLEUUID(CYCLING_POWER_MEASUREMENT_CHARACTERISTIC_UUID)) || data.size() < 4) {
    return;
  }
  int16_t rawSigned = (int16_t)(data[2] | (data[3] << 8));
  uint16_t raw = rawSigned > 0 ? (uint16_t)rawSigned : 0;
  uint16_t reported = SettingsManager::scaleReportedWatts(raw);
  if (reported > 32767) {
    reported = 32767;
  }
  if (SettingsManager::getPowerOffsetPercent() == 0) {
    return;
  }
  data[2] = (uint8_t)(reported & 0xFF);
  data[3] = (uint8_t)((reported >> 8) & 0xFF);
}

void DirConManager::onPowerOffsetChanged() {
  fecAccumValid = false;
  if (!started) {
    return;
  }
  if (zwiftTrainerMode == TrainerMode::ERG_MODE) {
    if (ftmsPower > 0) {
      BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower((uint16_t)(ftmsPower * 4)));
    } else if (zwiftPower > 0) {
      BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower((uint16_t)(zwiftPower * 4)));
    }
  } else if (zwiftTrainerMode == TrainerMode::SIM_MODE_VIRTUAL_SHIFTING &&
             virtualShiftingMode == VirtualShiftingMode::TARGET_POWER) {
    updateSIMModeResistance();
  }
}

uint16_t DirConManager::getRawPower() { return trainerRawPower; }

uint16_t DirConManager::getReportedPower() { return trainerInstantaneousPower; }

void DirConManager::notifyDirConCharacteristic(const NimBLEUUID &characteristicUUID, uint8_t *pData, size_t length) {
  notifyDirConCharacteristic(serviceManager->getCharacteristic(characteristicUUID), pData, length);
}

void DirConManager::notifyDirConCharacteristic(Characteristic *characteristic, uint8_t *pData, size_t length) {
  if (characteristic == nullptr || pData == nullptr) {
    return;
  }
  std::vector<uint8_t> data(pData, pData + length);
  adjustPowerPayload(characteristic->UUID, data);
  if (characteristic->UUID.equals(NimBLEUUID(TACX_FEC_READ_CHARACTERISTIC_UUID)) && data.size() == 13) {
    switch (data[4]) {
      case 0x10:
        trainerInstantaneousSpeed = (data[9] << 8) | data[8];
        break;
      case 0x19:
        trainerCadence = data[6];
        break;
      case 0x36:
        trainerMaximumResistance = (data[10] << 8) | data[9];
        break;
      default:
        break;
    }
  }
  sendDirConCharacteristicNotification(characteristic, data.data(), data.size(), true);
}

void DirConManager::sendDirConCharacteristicNotification(const NimBLEUUID &characteristicUUID, uint8_t *pData, size_t length, bool onlySubscribers) {
  sendDirConCharacteristicNotification(serviceManager->getCharacteristic(characteristicUUID), pData, length, onlySubscribers);
}

void DirConManager::sendDirConCharacteristicNotification(Characteristic *characteristic, uint8_t *pData, size_t length, bool onlySubscribers) {
  if (characteristic == nullptr) {
    return;
  }
  if (onlySubscribers && (characteristic->getSubscriptions().size() == 0)) {
    return;
  }
  DirConMessage dirConMessage;
  dirConMessage.Identifier = DIRCON_MSGID_UNSOLICITED_CHARACTERISTIC_NOTIFICATION;
  dirConMessage.UUID = characteristic->UUID;
  for (size_t dataIndex = 0; dataIndex < length; dataIndex++) {
    dirConMessage.AdditionalData.push_back(pData[dataIndex]);
  }
  std::vector<uint8_t> *messageData = dirConMessage.encode(0);
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    if (dirConClients[clientIndex].connected()) {
      if ((onlySubscribers && characteristic->isSubscribed(clientIndex)) || !onlySubscribers) {
        dirConClients[clientIndex].write((char *)messageData->data(), messageData->size());
      }
    }
  }
}

void DirConManager::notifyInternalCharacteristics() {
  for (Service *service : serviceManager->getServices()) {
    if (service->isInternal()) {
      for (Characteristic *characteristic : service->getCharacteristics()) {
        if (characteristic->getSubscriptions().size() > 0) {
          if (characteristic->UUID.equals(NimBLEUUID(ZWIFT_ASYNC_CHARACTERISTIC_UUID))) {
            std::vector<uint8_t> notificationData = generateZwiftAsyncNotificationData(trainerInstantaneousPower, trainerCadence, 0, 0, 0);
            notifyDirConCharacteristic(characteristic, notificationData.data(), notificationData.size());
          }
          if (characteristic->UUID.equals(NimBLEUUID(INDOOR_BIKE_DATA_CHARACTERISTIC_UUID))) {
            std::vector<uint8_t> notificationData = generateIndoorBikeDataNotificationData((uint16_t)(trainerInstantaneousSpeed * 36 / 100), (uint16_t)(trainerCadence * 2), (int16_t)trainerInstantaneousPower);
            notifyDirConCharacteristic(characteristic, notificationData.data(), notificationData.size());
          }
        }
      }
    }
  }
}

std::vector<uint8_t> DirConManager::generateZwiftAsyncNotificationData(int64_t power, int64_t cadence, int64_t unknown1, int64_t unknown2, int64_t unknown3, int64_t unknown4) {
  std::vector<uint8_t> notificationData;
  int64_t currentData = 0;
  uint8_t leb128Buffer[16];
  size_t leb128Size = 0;

  notificationData.push_back(0x03);

  for (uint8_t dataBlock = 0x08; dataBlock <= 0x30; dataBlock += 0x08) {
    notificationData.push_back(dataBlock);
    if (dataBlock == 0x08) {
      currentData = power;
    }
    if (dataBlock == 0x10) {
      currentData = cadence;
    }
    if (dataBlock == 0x18) {
      currentData = unknown1;
    }
    if (dataBlock == 0x20) {
      currentData = unknown2;
    }
    if (dataBlock == 0x28) {
      currentData = unknown3;
    }
    if (dataBlock == 0x30) {
      currentData = unknown4;
    }
    leb128Size = bfs::EncodeUleb128(currentData, leb128Buffer, sizeof(leb128Buffer));
    for (uint8_t leb128Byte = 0; leb128Byte < leb128Size; leb128Byte++) {
      notificationData.push_back(leb128Buffer[leb128Byte]);
    }
  }
  return notificationData;
}

std::string DirConManager::getStatusMessage() {
  return statusMessage;
}

void DirConManager::updateStatusMessage() {

  if (zwiftTrainerMode == TrainerMode::SIM_MODE) {
    statusMessage = "SIM";
  } else if (zwiftTrainerMode == TrainerMode::SIM_MODE_VIRTUAL_SHIFTING) {
    statusMessage = "SIM + VS";
  } else {
    statusMessage = "ERG";
  }
  statusMessage += " mode, ";

  size_t connectedClients = 0;
  for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
    if (dirConClients[clientIndex].connected()) {
      connectedClients++;
    }
  }
  statusMessage += std::to_string(connectedClients);
  statusMessage += "/";
  statusMessage += std::to_string(DIRCON_MAX_CLIENTS);
  statusMessage += " client(s)";
}

TrainerMode DirConManager::getZwiftTrainerMode() {
  return zwiftTrainerMode;
}

std::vector<uint8_t> DirConManager::processFTMSReadRequest(Service* service, Characteristic* characteristic, std::vector<uint8_t>* requestData) {
  std::vector<uint8_t> returnData;
  if (characteristic->UUID.equals(NimBLEUUID(FITNESS_MACHINE_FEATURE_CHARACTERISTIC_UUID))) {
    FITNESS_MACHINE_FEATURES_TYPE fitnessMachineFeaturesType;

    fitnessMachineFeaturesType.fitnessMachineFeatures = 
    FITNESS_MACHINE_FEATURES::cadence_supported |
    FITNESS_MACHINE_FEATURES::power_measurement_supported;

    fitnessMachineFeaturesType.targetSettingFeatures = 
    TARGET_SETTING_FEATURES::inclination_target_setting_supported |
    TARGET_SETTING_FEATURES::resistance_target_setting_supported |
    TARGET_SETTING_FEATURES::power_target_setting_supported |
    TARGET_SETTING_FEATURES::indoor_bike_simulation_parameters_supported;

    returnData = Utils::getVectorFromStruct(&fitnessMachineFeaturesType, sizeof(fitnessMachineFeaturesType));
  } else if (characteristic->UUID.equals(NimBLEUUID(TRAINING_STATUS_CHARACTERISTIC_UUID))) {
    // no flags, status "idle" until the trainer sends data
    returnData.push_back(0x00);
    returnData.push_back(trainerInstantaneousPower > 0 ? 0x0D : 0x01);
  }
  return returnData;
}

std::vector<uint8_t> DirConManager::processFTMSWriteRequest(Service* service, Characteristic* characteristic, std::vector<uint8_t>* requestData) {
  std::vector<uint8_t> returnData;
  //log_i("FTMS write request to characteristic %s with data %s", characteristic->UUID.toString().c_str(), Utils::getHexString(requestData).c_str());
  if (requestData->size() > 0) {
    bool success = false;
    uint8_t opCode = requestData->at(0);
    std::vector<uint8_t> parameter;
    if (requestData->size() > 1) {
      parameter = std::vector<uint8_t>(requestData->begin() + 1, requestData->end());
    }
    returnData.push_back(0x80);
    returnData.push_back(opCode);
    switch (opCode)
    {
      // request control, apps repeat this during a ride so it must not touch the active mode
      case 0x00:
        success = true;
        break;

      // reset
      case 0x01:
        zwiftTrainerMode = TrainerMode::SIM_MODE;
        log_i("SIM mode requested");
        updateStatusMessage();
        ftmsPower = 0;
        ftmsWindSpeed = 0;
        ftmsGrade = 0;
        ftmsCrr = 0;
        ftmsCw = 0;
        success = true;
        break;

      // target power
      case 0x05:
        if (parameter.size() == 2) {
          if (zwiftTrainerMode != TrainerMode::ERG_MODE) {
            zwiftTrainerMode = TrainerMode::ERG_MODE;
            log_i("ERG mode requested");
            updateStatusMessage();
          }
          // target power in 1W 
          ftmsPower = (parameter.at(1) << 8) | parameter.at(0);
          if (ftmsPower < 0) {
            log_e("FTMS target power is negative, setting to 0");
            ftmsPower = 0;
          }
          log_i("FTMS target power: %d W", ftmsPower);
          if (!BTDeviceManager::writeFECTargetPower(SettingsManager::scaleTargetPower((uint16_t)(ftmsPower * 4)))) {
            log_e("Error writing ERG FEC target power");
          }
          success = true;
        } else {
          success = false;
        }
        break;

      // start or resume
      case 0x07:
        // todo: start or resume
        success = true;
        break;

      // stop or pause
      case 0x08:
        // todo: stop or pause  
        success = true;
        break;
      // set indoor bike simulation parameters
      case 0x11:
        // [11 00 00 14 00 32 33]
        if (parameter.size() == 6) {
          if (zwiftTrainerMode != TrainerMode::SIM_MODE) {
            zwiftTrainerMode = TrainerMode::SIM_MODE;
            log_i("SIM mode requested");
            updateStatusMessage();
          }
          // wind speed in 0.001 m/s
          ftmsWindSpeed = (parameter.at(1) << 8) | parameter.at(0);
          // grade in 0.01%
          ftmsGrade = (parameter.at(3) << 8) | parameter.at(2);
          // crr in 0.0001
          ftmsCrr = parameter.at(4);
          // cw in 0.01
          ftmsCw = parameter.at(5);
          if (!BTDeviceManager::writeFECTrackResistance((uint16_t)(0x4E20 + ftmsGrade), ftmsCrr)) {
            log_e("Error writing SIM FEC track resistance");
          }
          success = true;
        } else {
          success = false;
        }
        break;

      default:
        log_e("Unknown FTMS opcode %d with parameter data %s received, discarding", opCode, Utils::getHexString(parameter).c_str());
        break;
    }
    if (success) {
      returnData.push_back(FITNESS_MACHINE_CONTROL_POINT_RESULT_CODE::success);
    } else {
      returnData.push_back(FITNESS_MACHINE_CONTROL_POINT_RESULT_CODE::op_code_not_supported);
    }
  }

  //log_i("FTMS write request to characteristic %s returning data %s", characteristic->UUID.toString().c_str(), Utils::getHexString(returnData).c_str());

  return returnData;
}

/**
 * @brief Generate the IndoorBikeData notification data
 * 
 * @param instantaneousSpeed Instantaneous speed in kph with a resolution of 0.01
 * @param instantaneousCadence Instantaneous cadence in rpm with a resolution of 0.5
 * @param instantaneousPower Instantaneous power in watts with a resolution of 1
 * @return std::vector<uint8_t> The notification data
 */
std::vector<uint8_t> DirConManager::generateIndoorBikeDataNotificationData(uint16_t instantaneousSpeed, uint16_t instantaneousCadence, int16_t instantaneousPower) {
  std::vector<uint8_t> notificationData;
  
  // enable instantaneous speed, instantaneous cadence and instantaneous power
  uint16_t flags =  INDOOR_BIKE_DATA_CHARACTERISTICS_FLAGS::instantaneous_cadence_present |
                    INDOOR_BIKE_DATA_CHARACTERISTICS_FLAGS::instantaneous_power_present;

  notificationData.push_back((uint8_t)(flags & 0xFF));
  notificationData.push_back((uint8_t)(flags >> 8));

  // add instantaneous speed (kph with a resolution of 0.01)
  notificationData.push_back((uint8_t)(instantaneousSpeed & 0xFF));
  notificationData.push_back((uint8_t)(instantaneousSpeed >> 8));

  // add instantaneous cadence (rpm with a resolution of 0.5)
  notificationData.push_back((uint8_t)(instantaneousCadence & 0xFF));
  notificationData.push_back((uint8_t)(instantaneousCadence >> 8));

  // add instantaneous power (watts with a resolution of 1)
  notificationData.push_back((uint8_t)(instantaneousPower & 0xFF));
  notificationData.push_back((uint8_t)(instantaneousPower >> 8));
  return notificationData;
}

void DirConManager::serverTask(void* arg) {
  listenFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listenFd < 0) {
    log_e("DirCon socket failed");
    started = false;
    vTaskDelete(nullptr);
    return;
  }
  int yes = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DIRCON_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listenFd, DIRCON_MAX_CLIENTS) != 0) {
    log_e("DirCon bind/listen failed: %d", errno);
    ::close(listenFd);
    listenFd = -1;
    started = false;
    vTaskDelete(nullptr);
    return;
  }
  log_i("DirCon listening on port %d", DIRCON_TCP_PORT);
  int64_t lastNotify = esp_timer_get_time();
  while (started) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxFd = listenFd;
    if (listenFd >= 0) {
      FD_SET(listenFd, &rfds);
    }
    for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
      if (dirConClients[clientIndex].connected()) {
        FD_SET(dirConClients[clientIndex].fd, &rfds);
        if (dirConClients[clientIndex].fd > maxFd) {
          maxFd = dirConClients[clientIndex].fd;
        }
      }
    }
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    int ready = select(maxFd + 1, &rfds, nullptr, nullptr, &tv);
    if (!started) {
      break;
    }
    if (ready > 0 && listenFd >= 0 && FD_ISSET(listenFd, &rfds)) {
      sockaddr_in peer = {};
      socklen_t peerLen = sizeof(peer);
      int fd = accept(listenFd, (sockaddr*)&peer, &peerLen);
      if (fd >= 0) {
        DirConClient* slot = nullptr;
        for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
          if (!dirConClients[clientIndex].connected()) {
            slot = &dirConClients[clientIndex];
            break;
          }
        }
        if (slot == nullptr) {
          ::close(fd);
        } else {
          slot->stop();
          slot->fd = fd;
          slot->alive = true;
          inet_ntop(AF_INET, &peer.sin_addr, slot->ip, sizeof(slot->ip));
          handleNewClient(slot);
        }
      }
    }
    for (size_t clientIndex = 0; clientIndex < DIRCON_MAX_CLIENTS; clientIndex++) {
      DirConClient* client = &dirConClients[clientIndex];
      if (!client->connected() || !FD_ISSET(client->fd, &rfds)) {
        continue;
      }
      uint8_t buffer[512];
      int received = recv(client->fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        closeClient(client, "disconnected");
      } else {
        handleDirConData(client, buffer, (size_t)received);
      }
    }
    int64_t now = esp_timer_get_time();
    if (now - lastNotify >= (int64_t)DIRCON_NOTIFICATION_INTERVAL * 1000) {
      doNotifications(nullptr);
      lastNotify = now;
    }
  }
  vTaskDelete(nullptr);
}