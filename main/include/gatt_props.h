#pragma once

#include <stdint.h>

/* Internal characteristic flags. Not the on-air GATT property bits. */
enum : uint32_t {
  PROP_READ = 0x01,
  PROP_WRITE = 0x02,
  PROP_NOTIFY = 0x04,
  PROP_INDICATE = 0x08,
};
