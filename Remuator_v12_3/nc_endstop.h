#pragma once
#include <stdint.h>

// NC на землю, INPUT_PULLUP. Размыкание блокирует движение сразу.
// Возврат в LOW должен быть непрерывным 20 мс: дребезг не возобновляет ход.
struct NcEndstop {
  bool active = false;
  bool closing = false;
  uint64_t closedSinceUs = 0;

  bool update(bool open, uint64_t nowUs) {
    if (open) {
      active = true;
      closing = false;
    } else if (active) {
      if (!closing) {
        closing = true;
        closedSinceUs = nowUs;
      } else if (nowUs - closedSinceUs >= 20000ULL) {
        active = false;
        closing = false;
      }
    }
    return active;
  }
};
