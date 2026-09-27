#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bfs {

inline size_t EncodeUleb128(uint64_t value, uint8_t* buffer, size_t capacity) {
  size_t written = 0;
  do {
    if (written >= capacity) {
      return 0;
    }
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value != 0) {
      byte |= 0x80;
    }
    buffer[written++] = byte;
  } while (value != 0);
  return written;
}

inline size_t DecodeUleb128(const uint8_t* buffer, size_t length, uint64_t* value) {
  uint64_t result = 0;
  unsigned shift = 0;
  for (size_t i = 0; i < length && i < 10; i++) {
    uint8_t byte = buffer[i];
    result |= (uint64_t)(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return i + 1;
    }
    shift += 7;
  }
  return 0;
}

}  // namespace bfs
