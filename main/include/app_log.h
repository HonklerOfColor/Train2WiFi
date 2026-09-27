#pragma once

#include "esp_log.h"

#define log_e(fmt, ...) ESP_LOGE("Train2WiFi", fmt, ##__VA_ARGS__)
#define log_w(fmt, ...) ESP_LOGW("Train2WiFi", fmt, ##__VA_ARGS__)
#define log_i(fmt, ...) ESP_LOGI("Train2WiFi", fmt, ##__VA_ARGS__)
#define log_d(fmt, ...) ESP_LOGD("Train2WiFi", fmt, ##__VA_ARGS__)
#define log_v(fmt, ...) ESP_LOGV("Train2WiFi", fmt, ##__VA_ARGS__)
