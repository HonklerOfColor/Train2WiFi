#include "BTDeviceManager.h"
#include "Config.h"
#include "DirConManager.h"
#include "SettingsManager.h"
#include "UUIDs.h"
#include "Utils.h"
#include "Version.h"
#include "gatt_props.h"
#include "app_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t web_style_css_start[] asm("_binary_style_css_start");
extern const uint8_t web_style_css_end[] asm("_binary_style_css_end");
extern const uint8_t web_favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t web_favicon_ico_end[] asm("_binary_favicon_ico_end");

static ServiceManager serviceManager;
static bool wifiUp = false;
static char wifiIp[16] = "0.0.0.0";
static char apIp[16] = "192.168.4.1";
static char wifiJoined[33] = {};
static esp_timer_handle_t wifiRetryTimer;

static void copy_ip(char* out, size_t out_len, esp_ip4_addr_t ip) {
  snprintf(out, out_len, IPSTR, IP2STR(&ip));
}

static void wifi_retry_cb(void*) {
  if (!wifiUp && !SettingsManager::getWifiSsid().empty()) {
    log_i("WiFi reconnect retry");
    esp_wifi_connect();
  }
}

static void start_wifi_retry() {
  if (wifiRetryTimer == nullptr) {
    return;
  }
  esp_timer_stop(wifiRetryTimer);
  esp_timer_start_periodic(wifiRetryTimer, 2000 * 1000);
}

static void stop_wifi_retry() {
  if (wifiRetryTimer != nullptr) {
    esp_timer_stop(wifiRetryTimer);
  }
}

static void on_ip(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (id == IP_EVENT_STA_GOT_IP) {
    auto* event = (ip_event_got_ip_t*)data;
    copy_ip(wifiIp, sizeof(wifiIp), event->ip_info.ip);
    wifiUp = true;
    stop_wifi_retry();
    log_i("WiFi DHCP %s", wifiIp);
  } else if (id == IP_EVENT_STA_LOST_IP) {
    wifiUp = false;
    start_wifi_retry();
  }
}

static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (id == WIFI_EVENT_STA_DISCONNECTED) {
    wifiUp = false;
    log_i("WiFi disconnected");
    if (!SettingsManager::getWifiSsid().empty()) {
      esp_wifi_connect();
      start_wifi_retry();
    }
  } else if (id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  }
}

static void start_wifi() {
  esp_netif_t* sta = esp_netif_create_default_wifi_sta();
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  std::string hostname = Utils::getHostName();
  std::string password = SettingsManager::getAPPassword();
  if (password.size() < 8) {
    password = hostname;
  }
  wifi_config_t ap_cfg = {};
  strncpy((char*)ap_cfg.ap.ssid, hostname.c_str(), sizeof(ap_cfg.ap.ssid) - 1);
  ap_cfg.ap.ssid_len = strlen((char*)ap_cfg.ap.ssid);
  strncpy((char*)ap_cfg.ap.password, password.c_str(), sizeof(ap_cfg.ap.password) - 1);
  ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.channel = 1;

  std::string ssid = SettingsManager::getWifiSsid();
  if (!ssid.empty()) {
    wifi_config_t sta_cfg = {};
    strncpy((char*)sta_cfg.sta.ssid, ssid.c_str(), sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, SettingsManager::getWifiPassword().c_str(), sizeof(sta_cfg.sta.password) - 1);
    strncpy(wifiJoined, ssid.c_str(), sizeof(wifiJoined) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  }
  esp_netif_set_hostname(sta, hostname.c_str());
  esp_netif_set_hostname(ap, hostname.c_str());
  esp_timer_create_args_t retryArgs = {};
  retryArgs.callback = wifi_retry_cb;
  retryArgs.name = "wifi_retry";
  esp_timer_create(&retryArgs, &wifiRetryTimer);
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  esp_netif_ip_info_t ap_info = {};
  if (esp_netif_get_ip_info(ap, &ap_info) == ESP_OK) {
    copy_ip(apIp, sizeof(apIp), ap_info.ip);
  }
  log_i("WiFi AP %s at http://%s", hostname.c_str(), apIp);
}

static std::string url_decode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+' ) {
      out.push_back(' ');
    } else if (in[i] == '%' && i + 2 < in.size()) {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out.push_back((char)strtol(hex, nullptr, 16));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

static bool form_has(const std::string& body, const char* key) {
  std::string needle = std::string(key) + "=";
  return body.find(needle) != std::string::npos;
}

static std::string form_get(const std::string& body, const char* key) {
  std::string needle = std::string(key) + "=";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) {
    return "";
  }
  pos += needle.size();
  size_t end = body.find('&', pos);
  return url_decode(body.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

static std::string read_body(httpd_req_t* req) {
  std::string body;
  body.resize(req->content_len);
  size_t received = 0;
  while (received < body.size()) {
    int n = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (n <= 0) {
      break;
    }
    received += n;
  }
  body.resize(received);
  return body;
}

static esp_err_t send_file(httpd_req_t* req, const uint8_t* start, const uint8_t* end, const char* type) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send(req, (const char*)start, end - start);
  return ESP_OK;
}

static int clamp_int(const std::string& value, int min_value, int max_value, int fallback) {
  if (value.empty()) {
    return fallback;
  }
  int number = atoi(value.c_str());
  if (number < min_value) {
    return min_value;
  }
  if (number > max_value) {
    return max_value;
  }
  return number;
}

static std::string json_escape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if ((uint8_t)c < 0x20 || (uint8_t)c == 0x7F) {
      continue;
    }
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

static esp_err_t status_get(httpd_req_t* req) {
  std::string wifi = wifiUp ? std::string("Connected, SSID: ") + wifiJoined + ", IP: " + wifiIp
                            : (SettingsManager::getWifiSsid().empty() ? "Access-Point mode" : "Disconnected");
  std::string json = "{\"device_name\":\"" + json_escape(Utils::getDeviceName()) + "\"";
  json += ",\"version\":\"" + std::string(VERSION) + "\"";
  json += ",\"build_timestamp\":\"" + std::string(VERSION_TIMESTAMP) + "\"";
  json += ",\"hostname\":\"" + json_escape(Utils::getFQDN()) + "\"";
  json += ",\"ap_address\":\"" + json_escape(apIp) + "\"";
  json += ",\"wifi_status\":\"" + json_escape(wifi) + "\"";
  json += ",\"service_status\":\"" + json_escape(serviceManager.getStatusMessage()) + "\"";
  json += ",\"dircon_status\":\"" + json_escape(DirConManager::getStatusMessage()) + "\"";
  json += ",\"ble_status\":\"" + json_escape(BTDeviceManager::getStatusMessage()) + "\"";
  json += ",\"virtual_shifting\":";
  json += SettingsManager::isVirtualShiftingEnabled() ? "true" : "false";
  json += ",\"ftms_emulation\":";
  json += SettingsManager::isFTMSEnabled() ? "true" : "false";
  json += ",\"power_offset\":" + std::to_string((int)SettingsManager::getPowerOffsetPercent());
  json += ",\"power_raw\":" + std::to_string(DirConManager::getRawPower());
  json += ",\"power_reported\":" + std::to_string(DirConManager::getReportedPower());
  json += ",\"free_heap\":" + std::to_string((unsigned long)esp_get_free_heap_size());
  json += "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.size());
  return ESP_OK;
}

static esp_err_t send_devices_json(httpd_req_t* req) {
  std::string json = "{\"device_name\":\"" + json_escape(Utils::getDeviceName()) + "\",\"trainer_device\":\"" +
                     json_escape(SettingsManager::getTrainerDeviceName()) + "\",\"trainer_devices\":[\"\"";
  for (const std::string& name : BTDeviceManager::getScannedDeviceNames()) {
    json += ",\"" + json_escape(name) + "\"";
  }
  json += "],\"scanning\":";
  json += BTDeviceManager::isScanning() ? "true" : "false";
  json += ",\"virtual_shifting\":";
  json += SettingsManager::isVirtualShiftingEnabled() ? "true" : "false";
  json += ",\"virtual_shifting_mode\":" + std::to_string((int)SettingsManager::getVirtualShiftingMode());
  json += ",\"virtual_shifting_modes\":[";
  bool first = true;
  for (const auto& mode : SettingsManager::getVirtualShiftingModes()) {
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"name\":\"" + mode.second + "\",\"value\":" + std::to_string(mode.first) + "}";
  }
  json += "],\"chainring_teeth\":" + std::to_string(SettingsManager::getChainringTeeth());
  json += ",\"sprocket_teeth\":" + std::to_string(SettingsManager::getSprocketTeeth());
  json += ",\"grade_smoothing\":";
  json += SettingsManager::isGradeSmoothingEnabled() ? "true" : "false";
  json += ",\"difficulty\":" + std::to_string(SettingsManager::getDifficulty());
  json += ",\"ftms_emulation\":";
  json += SettingsManager::isFTMSEnabled() ? "true" : "false";
  json += ",\"power_offset\":" + std::to_string((int)SettingsManager::getPowerOffsetPercent());
  json += ",\"wifi_ssid\":\"" + json_escape(SettingsManager::getWifiSsid()) + "\"";
  json += ",\"wifi_configured\":";
  json += SettingsManager::getWifiPassword().empty() ? "false" : "true";
  json += "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.size());
  return ESP_OK;
}

static esp_err_t settings_post(httpd_req_t* req) {
  std::string body = read_body(req);
  if (!body.empty()) {
    if (form_has(body, "trainer_device")) {
      SettingsManager::setTrainerDeviceName(form_get(body, "trainer_device"));
    }
    SettingsManager::setVirtualShiftingEnabled(form_has(body, "virtual_shifting"));
    if (form_has(body, "virtual_shifting_mode")) {
      SettingsManager::setVirtualShiftingMode((VirtualShiftingMode)atoi(form_get(body, "virtual_shifting_mode").c_str()));
    }
    if (form_has(body, "chainring_teeth")) {
      SettingsManager::setChainringTeeth(clamp_int(form_get(body, "chainring_teeth"), 1, 100, 34));
    }
    if (form_has(body, "sprocket_teeth")) {
      SettingsManager::setSprocketTeeth(clamp_int(form_get(body, "sprocket_teeth"), 1, 100, 14));
    }
    SettingsManager::setGradeSmoothingEnabled(form_has(body, "grade_smoothing"));
    if (form_has(body, "difficulty")) {
      SettingsManager::setDifficulty(clamp_int(form_get(body, "difficulty"), 1, 200, 100));
    }
    if (form_has(body, "ftms_emulation")) {
      SettingsManager::setFTMSEnabled(true);
      SettingsManager::setVirtualShiftingEnabled(false);
    } else {
      SettingsManager::setFTMSEnabled(false);
    }
    if (form_has(body, "wifi_ssid")) {
      std::string password = SettingsManager::getWifiPassword();
      if (form_has(body, "wifi_password") && !form_get(body, "wifi_password").empty()) {
        password = form_get(body, "wifi_password");
      }
      SettingsManager::setWifi(form_get(body, "wifi_ssid"), password);
    }
    SettingsManager::save();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
  }
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
  return ESP_OK;
}

static esp_err_t power_offset_post(httpd_req_t* req) {
  std::string body = read_body(req);
  int percent = clamp_int(form_get(body, "power_offset"), -10, 10, 0);
  SettingsManager::setPowerOffsetPercent((int8_t)percent);
  SettingsManager::save();
  DirConManager::onPowerOffsetChanged();
  std::string json = std::string("{\"ok\":true,\"power_offset\":") + std::to_string(percent) + "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.size());
  return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t* req) {
  log_i("Trainer search requested");
  BTDeviceManager::requestRescan();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t wifi_get(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/#netzwerk");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static esp_err_t wifi_post(httpd_req_t* req) {
  std::string body = read_body(req);
  SettingsManager::setWifi(form_get(body, "ssid"), form_get(body, "password"));
  SettingsManager::save();
  httpd_resp_send(req, "restarting", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
  return ESP_OK;
}

static esp_err_t debug_get(httpd_req_t* req) {
  const char* mode = "ERG mode";
  if (DirConManager::getZwiftTrainerMode() == SIM_MODE) {
    mode = "SIM mode";
  } else if (DirConManager::getZwiftTrainerMode() == SIM_MODE_VIRTUAL_SHIFTING) {
    mode = "SIM + VS mode";
  }
  std::string json = std::string("{\"zwift_trainer_mode\":\"") + mode + "\",\"ble_services\":{";
  bool firstService = true;
  for (Service* service : serviceManager.getServices()) {
    if (!firstService) {
      json += ",";
    }
    firstService = false;
    json += "\"" + service->UUID.to128().toString() + "\":{";
    bool firstChar = true;
    for (Characteristic* characteristic : service->getCharacteristics()) {
      if (!firstChar) {
        json += ",";
      }
      firstChar = false;
      json += "\"" + characteristic->UUID.to128().toString() + "\":" +
              std::to_string(characteristic->getSubscriptions().size());
    }
    json += "}";
  }
  json += "}}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.size());
  return ESP_OK;
}

static void start_http() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = WEB_SERVER_PORT;
  config.max_uri_handlers = 16;
  config.stack_size = 20480;
  config.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  ESP_ERROR_CHECK(httpd_start(&server, &config));

  auto reg = [&](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t item = {};
    item.uri = uri;
    item.method = method;
    item.handler = handler;
    httpd_register_uri_handler(server, &item);
  };
  reg("/", HTTP_GET, [](httpd_req_t* req) { return send_file(req, web_index_html_start, web_index_html_end, "text/html"); });
  reg("/style.css", HTTP_GET, [](httpd_req_t* req) { return send_file(req, web_style_css_start, web_style_css_end, "text/css"); });
  reg("/favicon.ico", HTTP_GET,
      [](httpd_req_t* req) { return send_file(req, web_favicon_ico_start, web_favicon_ico_end, "image/x-icon"); });
  reg("/settings", HTTP_GET, [](httpd_req_t* req) {
    return send_file(req, web_index_html_start, web_index_html_end, "text/html");
  });
  reg("/settings", HTTP_POST, settings_post);
  reg("/devicesettings", HTTP_GET, send_devices_json);
  reg("/scan", HTTP_GET, scan_get);
  reg("/scan", HTTP_POST, scan_get);
  reg("/status", HTTP_GET, status_get);
  reg("/poweroffset", HTTP_POST, power_offset_post);
  reg("/debug", HTTP_GET, debug_get);
  reg("/wifi", HTTP_GET, wifi_get);
  reg("/wifi", HTTP_POST, wifi_post);
}

static void add_internal_services() {
  if (SettingsManager::isVirtualShiftingEnabled()) {
    Service* zwift = new Service(NimBLEUUID(ZWIFT_CUSTOM_SERVICE_UUID), true, true);
    zwift->addCharacteristic(new Characteristic(NimBLEUUID(ZWIFT_ASYNC_CHARACTERISTIC_UUID), PROP_NOTIFY));
    zwift->addCharacteristic(new Characteristic(NimBLEUUID(ZWIFT_SYNCRX_CHARACTERISTIC_UUID), PROP_WRITE));
    zwift->addCharacteristic(new Characteristic(NimBLEUUID(ZWIFT_SYNCTX_CHARACTERISTIC_UUID), PROP_INDICATE));
    serviceManager.addService(zwift);
  }
  if (SettingsManager::isFTMSEnabled()) {
    Service* ftms = new Service(NimBLEUUID(FITNESS_MACHINE_SERVICE_UUID), true, true);
    ftms->addCharacteristic(new Characteristic(NimBLEUUID(FITNESS_MACHINE_FEATURE_CHARACTERISTIC_UUID), PROP_READ));
    ftms->addCharacteristic(new Characteristic(NimBLEUUID(INDOOR_BIKE_DATA_CHARACTERISTIC_UUID), PROP_NOTIFY));
    ftms->addCharacteristic(new Characteristic(NimBLEUUID(TRAINING_STATUS_CHARACTERISTIC_UUID), PROP_READ | PROP_NOTIFY));
    ftms->addCharacteristic(
        new Characteristic(NimBLEUUID(FITNESS_MACHINE_CONTROL_POINT_CHARACTERISTIC_UUID), PROP_WRITE | PROP_INDICATE));
    ftms->addCharacteristic(new Characteristic(NimBLEUUID(FITNESS_MACHINE_STATUS_CHARACTERISTIC_UUID), PROP_NOTIFY));
    serviceManager.addService(ftms);
  }
}

extern "C" void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  Utils::init();
  SettingsManager::load();
  log_i("%s %s starting", DEVICE_NAME_PREFIX, VERSION);
  log_i("Device name: %s, host name: %s", Utils::getDeviceName().c_str(), Utils::getFQDN().c_str());

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr));

  start_wifi();
  ESP_ERROR_CHECK(mdns_init());
  mdns_hostname_set(Utils::getHostName().c_str());
  mdns_instance_name_set(Utils::getDeviceName().c_str());

  BTDeviceManager::setLocalDeviceName(Utils::getDeviceName());
  BTDeviceManager::setServiceManager(&serviceManager);
  if (!BTDeviceManager::start()) {
    log_e("Unable to start bluetooth device manager");
    esp_restart();
  }
  BTDeviceManager::setRemoteDeviceNameFilter(SettingsManager::getTrainerDeviceName());

  add_internal_services();
  DirConManager::setServiceManager(&serviceManager);
  if (!DirConManager::start()) {
    log_e("Unable to start DirCon manager");
    esp_restart();
  }
  start_http();
  log_i("Startup finished");

  while (true) {
    BTDeviceManager::update();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
