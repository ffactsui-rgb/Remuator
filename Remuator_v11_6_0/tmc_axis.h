#pragma once
#include "tmc_config.h"
#include <TMCStepper.h>
#include <memory>

// Типизированные владельцы: у TMCStepper нет виртуального деструктора.
// Общие регистры TMC2208/2209 доступны через базовый класс.
class TmcAxis {
public:
  bool connected = false;
  uint8_t version = 0;
  // True only after IOIN model check + IFCNT counts every written command.
  bool settingsApplied = false;
  uint8_t ifcntBefore = 0, ifcntAfter = 0;
  uint8_t writesSent = 0, writesAccepted = 0;

  void configure(Stream *serial, const TmcUartSettings &settings) {
    driver2208.reset();
    driver2209.reset();
    connected = false;
    version = 0;
    settingsApplied = false;
    ifcntBefore = ifcntAfter = writesSent = writesAccepted = 0;
    if (settings.model == TmcModel::Tmc2208) {
      driver2208.reset(new TMC2208Stepper(serial, 0.11f)); // Только адрес 0.
    } else {
      driver2209.reset(new TMC2209Stepper(serial, 0.11f, settings.address));
    }
    driver().begin();
    if (driver2209) {
      // Z без концевиков и без остановки по StallGuard. Специфические
      // регистры 2209 никогда не отправляются в TMC2208.
      driver2209->TCOOLTHRS(0);
      driver2209->COOLCONF(0);
    }
  }

  TMC2208Stepper &driver() {
    return driver2209 ? static_cast<TMC2208Stepper &>(*driver2209) : *driver2208;
  }

  uint32_t readInputs() {
    return driver2209 ? driver2209->IOIN() : driver2208->IOIN();
  }

  void checkConnection(TmcModel model) {
    version = driver2209 ? driver2209->version() : driver2208->version();
    connected = !driver().CRCerror && version == (model == TmcModel::Tmc2208 ? 0x20 : 0x21);
    if (!connected) settingsApplied = false;
  }

private:
  std::unique_ptr<TMC2208Stepper> driver2208;
  std::unique_ptr<TMC2209Stepper> driver2209;
};
