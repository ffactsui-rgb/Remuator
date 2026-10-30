#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <time.h>

// Минимальный драйвер DS3231 (I2C 0x68), без сторонней библиотеки часов.
// Аппаратные часы всегда хранят UTC; часовой пояс TZ применяется только в ESP32.
// Регистры даты: 0x00–0x06; признак остановки генератора OSF — бит 7 регистра 0x0F.
class Ds3231Clock {
public:
  explicit Ds3231Clock(TwoWire &bus) : wire_(bus) {}
  bool present() const { return present_; }

  bool readUtc(time_t &epoch) {
    uint8_t status = 0;
    if (!readRegisters(0x0F, &status, 1) || (status & 0x80U))
      return false;
    uint8_t registers[7] = {};
    if (!readRegisters(0x00, registers, sizeof(registers)))
      return false;
    if (registers[2] & 0x40U) // Поддерживается только 24-часовой режим.
      return false;
    const int second = fromBcd(registers[0] & 0x7F);
    const int minute = fromBcd(registers[1] & 0x7F);
    const int hour = fromBcd(registers[2] & 0x3F);
    const int day = fromBcd(registers[4] & 0x3F);
    const int month = fromBcd(registers[5] & 0x1F);
    const int year = 2000 + fromBcd(registers[6]) + ((registers[5] & 0x80U) ? 100 : 0);
    if (!bcdValid(registers[0] & 0x7F) || !bcdValid(registers[1] & 0x7F) ||
        !bcdValid(registers[2] & 0x3F) || !bcdValid(registers[4] & 0x3F) ||
        !bcdValid(registers[5] & 0x1F) || !bcdValid(registers[6]))
      return false;
    if (year < 2023 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 59)
      return false;
    epoch = static_cast<time_t>(daysFromCivil(year, month, day) * 86400LL +
                                hour * 3600 + minute * 60 + second);
    return true;
  }

  bool writeUtc(time_t epoch) {
    if (epoch < 1700000000LL || epoch > 4102444799LL) return false;
    struct tm parts = {};
    if (!gmtime_r(&epoch, &parts) || parts.tm_year + 1900 < 2023 || parts.tm_year + 1900 > 2099)
      return false;
    const uint8_t values[] = {
        toBcd(parts.tm_sec), toBcd(parts.tm_min), toBcd(parts.tm_hour),
        toBcd(parts.tm_wday + 1), toBcd(parts.tm_mday),
        toBcd(parts.tm_mon + 1), toBcd((parts.tm_year + 1900) - 2000)};
    if (!writeRegisters(0x00, values, sizeof(values))) return false;
    // В DS3231 значение EOSC=1 отключает генератор при питании от батареи.
    // Сбрасываем этот бит, чтобы часы продолжали идти при отключении основного питания.
    uint8_t control = 0;
    if (!readRegisters(0x0E, &control, 1)) return false;
    if (control & 0x80U) {
      control &= ~0x80U;
      if (!writeRegisters(0x0E, &control, 1)) return false;
    }
    // OSF сбрасывается только после полной записи, остальные флаги сохраняются.
    uint8_t status = 0;
    if (!readRegisters(0x0F, &status, 1)) return false;
    status &= ~0x80U;
    if (!writeRegisters(0x0F, &status, 1)) return false;
    time_t verify = 0;
    return readUtc(verify) && (verify >= epoch - 1 && verify <= epoch + 2);
  }

private:
  TwoWire &wire_;
  bool present_ = false;
  static constexpr uint8_t ADDRESS = 0x68;
  static int fromBcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }
  static bool bcdValid(uint8_t value) { return (value & 0x0F) < 10 && (value >> 4) < 10; }
  static uint8_t toBcd(int value) { return (uint8_t)((value / 10) * 16 + value % 10); }
  static int daysInMonth(int y, int m) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) return 29;
    return days[m - 1];
  }
  // Число суток UTC с 01.01.1970, независимо от текущего часового пояса POSIX TZ.
  static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const int mp = static_cast<int>(m) + (m > 2 ? -3 : 9);
    const unsigned doy = (153 * static_cast<unsigned>(mp) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int>(doe) - 719468LL;
  }
  bool readRegisters(uint8_t start, uint8_t *out, size_t count) {
    wire_.beginTransmission(ADDRESS);
    wire_.write(start);
    if (wire_.endTransmission(false) != 0) { present_ = false; return false; }
    const size_t got = wire_.requestFrom(ADDRESS, (uint8_t)count);
    if (got != count) { present_ = false; return false; }
    for (size_t i = 0; i < count; ++i) out[i] = (uint8_t)wire_.read();
    present_ = true;
    return true;
  }
  bool writeRegisters(uint8_t start, const uint8_t *values, size_t count) {
    wire_.beginTransmission(ADDRESS);
    wire_.write(start);
    for (size_t i = 0; i < count; ++i) wire_.write(values[i]);
    const bool ok = wire_.endTransmission() == 0;
    present_ = ok;
    return ok;
  }
};
