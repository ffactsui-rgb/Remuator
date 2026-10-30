#pragma once
// Переносимая модель программы, общая для прошивки и регрессионных тестов на компьютере.
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <stdint.h>
#include <string>
#include <vector>

constexpr uint16_t MAX_PROGRAM_STEPS = 80;
constexpr uint32_t MAX_PROGRAM_SECONDS = 31536000; // Один год.
constexpr int32_t BASE_ANGLE_CENTI = 2300;
constexpr int32_t MAX_ANGLE_CENTI = 8500;
constexpr int32_t MAX_ROT_PARTS = 1600; // Не более 100 оборотов за одну команду.

struct ProgramStep {
  uint16_t number;
  int32_t incAngleCenti; // Входное значение: сотые доли градуса.
  int16_t incRot;        // Входное значение: число поворотов по 22,5° со знаком.
  uint32_t pauseSeconds; // Входное значение: интервал между началами шагов, включая последнюю выдержку.
  float angleNaklon;     // Вычисленный абсолютный угол Y.
  float anglePovorot;    // Вычисленное ПРИРАЩЕНИЕ поворота, без накопления.
  int32_t rotationParts; // Накопленный поворот относительно начала программы.
  uint32_t startSecond;  // Сумма предшествующих пауз.
};

struct RemuageProgram {
  char name[120]; // Имя файла без расширения.
  uint16_t count;
  ProgramStep steps[MAX_PROGRAM_STEPS];
};

inline std::string programTrim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos ? std::string()
                                : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline bool programInteger(std::string s, int32_t &out, int32_t low, int32_t high) {
  s = programTrim(s);
  if (s.empty())
    return false;
  size_t pos = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = s[0] == '-';
    pos++;
  }
  if (pos == s.size())
    return false;
  int64_t value = 0;
  for (; pos < s.size(); ++pos) {
    if (s[pos] < '0' || s[pos] > '9')
      return false;
    value = value * 10 + (s[pos] - '0');
    if (value > 2147483648LL)
      return false;
  }
  if (negative)
    value = -value;
  if (value < low || value > high)
    return false;
  out = (int32_t)value;
  return true;
}

inline bool programAngle(std::string s, int32_t &out) {
  s = programTrim(s);
  if (s.empty())
    return false;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = s[0] == '-';
    s.erase(0, 1);
  }
  if (s.empty())
    return false;
  size_t sep = s.find_first_of(".,");
  std::string whole = sep == std::string::npos ? s : s.substr(0, sep);
  std::string fraction = sep == std::string::npos ? "" : s.substr(sep + 1);
  if (whole.empty() || fraction.size() > 2 || (sep != std::string::npos && fraction.empty()))
    return false;
  for (char c : whole)
    if (c < '0' || c > '9')
      return false;
  for (char c : fraction)
    if (c < '0' || c > '9')
      return false;
  int32_t w = 0, f = 0;
  if (!programInteger(whole, w, 0, 360))
    return false;
  if (!fraction.empty() && !programInteger(fraction, f, 0, 99))
    return false;
  if (fraction.size() == 1)
    f *= 10;
  out = (w * 100 + f) * (negative ? -1 : 1);
  return true;
}

// Целое число по-прежнему означает часы; также принимаются ЧЧ:ММ и ЧЧ:ММ:СС.
inline bool programPause(std::string text, uint32_t &seconds) {
  text = programTrim(text);
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i)
    if (i == text.size() || text[i] == ':') {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  if (parts.empty() || parts.size() > 3 || parts[0].empty())
    return false;
  int32_t values[3] = {0, 0, 0};
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i && parts[i].size() != 2)
      return false;
    for (char c : parts[i])
      if (c < '0' || c > '9')
        return false;
    if (!programInteger(parts[i], values[i], 0, i ? 59 : 8760))
      return false;
  }
  seconds = (uint32_t)values[0] * 3600 + (uint32_t)values[1] * 60 + (uint32_t)values[2];
  return seconds <= MAX_PROGRAM_SECONDS;
}

inline std::string programPauseText(uint32_t seconds) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
           (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  return buffer;
}

inline std::string programAngleText(int32_t centi) {
  char b[32];
  const int32_t a = centi < 0 ? -centi : centi;
  if (a % 100 == 0)
    snprintf(b, sizeof(b), "%s%ld", centi < 0 ? "-" : "", (long)(a / 100));
  else
    snprintf(b, sizeof(b), "%s%ld.%02ld", centi < 0 ? "-" : "", (long)(a / 100), (long)(a % 100));
  return b;
}

inline bool recalculateProgram(RemuageProgram &p, std::string &error) {
  error.clear();
  if (!p.count || p.count > MAX_PROGRAM_STEPS) {
    error = "Нужно от 1 до 80 шагов";
    return false;
  }
  int64_t angle = BASE_ANGLE_CENTI, parts = 0;
  uint64_t seconds = 0;
  for (uint16_t i = 0; i < p.count; i++) {
    ProgramStep &s = p.steps[i];
    angle += s.incAngleCenti;
    parts += s.incRot;
    s.number = i + 1;
    s.angleNaklon = angle / 100.0f;
    s.anglePovorot = s.incRot * 22.5f;
    s.rotationParts = (int32_t)parts;
    s.startSecond = (uint32_t)seconds;
    seconds += s.pauseSeconds;
    std::string prefix = "Шаг " + std::to_string(i + 1) + ": ";
    if (angle < BASE_ANGLE_CENTI) {
      error = prefix + "расчётный Y не должен быть ниже 23°";
      return false;
    }
    if (s.incRot < -MAX_ROT_PARTS || s.incRot > MAX_ROT_PARTS) {
      error = prefix + "IncRot должен быть целым от −1600 до 1600";
      return false;
    }
    if (seconds > MAX_PROGRAM_SECONDS) {
      error = "Сумма пауз больше 365 суток";
      return false;
    }
  }
  return true;
}

inline uint32_t programDurationSeconds(const RemuageProgram &p) {
  uint32_t n = 0;
  for (uint16_t i = 0; i < p.count; i++)
    n += p.steps[i].pauseSeconds;
  return n;
}

inline bool programExceedsTiltRange(const RemuageProgram &p) {
  for (uint16_t i = 0; i < p.count; ++i)
    if (p.steps[i].angleNaklon > MAX_ANGLE_CENTI / 100.0f)
      return true;
  return false;
}

inline int32_t programOrientationParts(int32_t parts) { return (parts % 16 + 16) % 16; }

// Округляется НАКОПЛЕННАЯ координата. Независимое округление каждой команды 22,5°
// накапливало бы ошибку, поскольку 40392/16 даёт 2524,5 шага двигателя.
inline int32_t programRotationSteps(int32_t parts, int32_t steps360 = 40392) {
  int64_t n = (int64_t)parts * steps360;
  return (int32_t)(n >= 0 ? (n + 8) / 16 : -((-n + 8) / 16));
}

inline bool programCsvFields(const std::string &line, std::vector<std::string> &fields) {
  fields.clear();
  std::string value;
  bool quoted = false, closed = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        value += '"';
        i++;
      } else if (quoted) {
        quoted = false;
        closed = true;
      } else if (programTrim(value).empty() && !closed) {
        value.clear();
        quoted = true;
      } else
        return false;
    } else if (c == ';' && !quoted) {
      fields.push_back(programTrim(value));
      value.clear();
      closed = false;
    } else {
      if (closed && c != ' ' && c != '\t' && c != '\r')
        return false;
      value += c;
    }
  }
  if (quoted)
    return false;
  fields.push_back(programTrim(value));
  if (fields.size() == 7 && fields.back().empty())
    fields.pop_back();
  return fields.size() == 6;
}

inline bool parseProgramCsv(const std::string &text, RemuageProgram &out, std::string &error) {
  memset(&out, 0, sizeof(out));
  error.clear();
  if (text.size() > 65536) {
    error = "CSV больше 64 КиБ";
    return false;
  }
  size_t pos = 0;
  unsigned lineNo = 0;
  bool header = false;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
      end = text.size();
    std::string line = programTrim(text.substr(pos, end - pos));
    pos = end + 1;
    ++lineNo;
    if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
      line.erase(0, 3);
    if (line.empty())
      continue;
    if (!header && (line == "sep=;" || line == "SEP=;"))
      continue;
    std::vector<std::string> f;
    if (line.size() > 512 || !programCsvFields(line, f)) {
      error = "Строка " + std::to_string(lineNo) +
              ": нужны 6 колонок через ';'. Старый формат утро/вечер не поддерживается";
      return false;
    }
    if (!header) {
      if ((f[0] != "Step" && f[0] != "№") || (f[1] != "incAngle" && f[1] != "incУгол") ||
          (f[2] != "Angle" && f[2] != "Угол") || f[3] != "IncRot" || f[4] != "Rot" ||
          (f[5] != "Pause" && f[5] != "Пауза")) {
        error = "Заголовок: Step;incAngle;Angle;IncRot;Rot;Pause";
        return false;
      }
      header = true;
      continue;
    }
    if (out.count >= MAX_PROGRAM_STEPS) {
      error = "Максимум 80 шагов";
      return false;
    }
    int32_t number = 0, incRot = 0;
    ProgramStep &s = out.steps[out.count];
    std::string prefix = "Строка " + std::to_string(lineNo) + ": ";
    if (!programInteger(f[0], number, 1, 80) || number != out.count + 1) {
      error = prefix + "номера шагов должны идти подряд с 1";
      return false;
    }
    if (!programAngle(f[1], s.incAngleCenti)) {
      error = prefix + "incAngle — число, не более двух знаков после запятой";
      return false;
    }
    if (!programInteger(f[3], incRot, -MAX_ROT_PARTS, MAX_ROT_PARTS)) {
      error = prefix + "IncRot — целое число частей по 22,5°";
      return false;
    }
    if (!programPause(f[5], s.pauseSeconds)) {
      error = prefix + "Pause — часы, Ч:ММ или ЧЧ:ММ:СС (например 6, 0:30 или 00:00:30)";
      return false;
    }
    s.incRot = (int16_t)incRot;
    // Столбцы f[2] и f[4] служат только для отображения: допускается заполненный пример,
    // но значения всегда пересчитываются, а при записи эти столбцы остаются пустыми.
    out.count++;
  }
  if (!header) {
    error = "Нет заголовка CSV";
    return false;
  }
  return recalculateProgram(out, error);
}

inline std::string serializeProgramCsv(const RemuageProgram &p) {
  std::string text = "\xEF\xBB\xBF"
                     "Step;incAngle;Angle;IncRot;Rot;Pause\r\n";
  for (uint16_t i = 0; i < p.count; i++) {
    const ProgramStep &s = p.steps[i];
    text += std::to_string(i + 1) + ";" + programAngleText(s.incAngleCenti) + ";;" +
            std::to_string(s.incRot) + ";;" + programPauseText(s.pauseSeconds) + "\r\n";
  }
  return text;
}

inline uint32_t programHashBytes(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

// Монотонное время без переполнения 32-битного millis и влияния коррекций NTP.
struct ProgramInterval {
  uint64_t deadlineUs = 0, remainingUs = 0;
  bool paused = false;
  void start(uint64_t now, uint64_t duration) {
    deadlineUs = now + duration;
    remainingUs = duration;
    paused = false;
  }
  uint64_t remaining(uint64_t now) const {
    return paused ? remainingUs : (deadlineUs > now ? deadlineUs - now : 0);
  }
  void pause(uint64_t now) {
    remainingUs = remaining(now);
    paused = true;
  }
  void resume(uint64_t now) {
    deadlineUs = now + remainingUs;
    paused = false;
  }
  bool due(uint64_t now) const { return !paused && now >= deadlineUs; }
};
