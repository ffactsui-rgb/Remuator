#pragma once
#include <stdint.h>
#include <initializer_list>

enum class TmcModel : uint16_t { Tmc2208 = 2208, Tmc2209 = 2209 };

struct TmcUartSettings {
  TmcModel model = TmcModel::Tmc2208;
  uint8_t address = 0;
  uint32_t baud = 115200;
  uint16_t rmsCurrent = 900; // Безопасный старт: поднимать только по паспорту NEMA17 и охлаждению
  float holdMultiplier = 0.5f;
  uint16_t microsteps = 16;
  uint8_t toff = 5;
  uint8_t blankTime = 24;
  int8_t hysteresisEnd = 0;
  uint8_t hysteresisStart = 4;
  bool stealthChop = false; // spreadCycle: быстрый отклик тока на высоких скоростях
  bool pwmAutoscale = true;
  bool pwmAutograd = false;
  uint8_t pwmOfs = 36;
  uint8_t pwmGrad = 14;
  uint8_t pwmFreq = 1;
  uint8_t pwmReg = 8;
  uint8_t pwmLim = 12;
  uint8_t freewheel = 0;
  bool intpol = true;
  bool pdnDisable = true;
  bool iScaleAnalog = false;
  bool internalRsense = false;
  bool multistepFilter = false;
  uint8_t sendDelay = 2;
};

inline uint16_t tmcMaxRms(TmcModel model) { return model == TmcModel::Tmc2208 ? 1400 : 2000; }

// На общей линии нельзя независимо адресовать двух TMC2208.
// Перемычки TMC2209 задают адрес физически; прошивка лишь обращается к нему.
inline const char *tmcBusError(const TmcUartSettings &y, const TmcUartSettings &z) {
  for (const auto *c : {&y, &z}) {
    if (c->model != TmcModel::Tmc2208 && c->model != TmcModel::Tmc2209)
      return "Выберите TMC2208 или TMC2209";
    if (c->address > 3)
      return "Адрес TMC2209 должен быть 0, 1, 2 или 3";
    if (c->model == TmcModel::Tmc2208 && c->address != 0)
      return "У TMC2208 фиксированный UART-адрес 0";
  }
  if (y.address == z.address)
    return "Конфликт адресов общей UART: нужны разные адреса. Два TMC2208 здесь не поддерживаются";
  return nullptr;
}
