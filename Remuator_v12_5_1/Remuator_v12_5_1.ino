
/*
  REmuator ESP32-S3 v12.5.1
  ---------------------------------------------
  Перенос REmuator 8.7 с Arduino Nano на ESP32-S3.

  Основные возможности:
    - Два двигателя: Y (наклон) и Z (поворот), TMC2208/TMC2209 на общей UART;
    - NC-концевики Y: GPIO14 (23°), GPIO13 (85°), внешняя подтяжка 10 кОм к 3,3 В;
    - Шина I2C GPIO2 (SDA) / GPIO1 (SCL): OLED + DS3231, резервное питание RTC;
    - DS3231 в UTC; при старте система получает время RTC без Wi-Fi;
    - NTP при наличии STA Wi-Fi, период/серверы и часовой пояс в веб-настройках;
    - Журнал восстановления LittleFS с проверкой CRC и календарными сроками;
    - Движение Y по сохранённой карте шагов; ADXL345 используется для
      калибровки, восстановления координаты после питания и индикации угла;
    - Кнопки сохранены в коде, но полностью отключены до новой распиновки.

  ОБЯЗАТЕЛЬНО ДЛЯ ПОДКЛЮЧЕНИЯ:
    Y (наклон): DIR=GPIO4, STEP=GPIO5, EN=GPIO6 (LOW включает);
    Z (поворот): DIR=GPIO42, STEP=GPIO41, EN=GPIO40 (LOW включает);
    TMC: TX=GPIO21 через резистор 1 кОм на общую PDN_UART;
         RX=GPIO47 прямо на PDN_UART.
    Подтянуть каждый EN к 3,3 В отдельным 10 кОм.
    Концевики: NC на GND, 10 кОм от входа к +3,3 В;
      LOW=цепь замкнута, HIGH=концевик сработал / провод оборван.
    DS3231: VCC=3,3 В, GND=GND, SDA=GPIO2, SCL=GPIO1;
      отдельные подтяжки I2C — только к 3,3 В. Резервная батарея обязательна
      для отсчёта календаря при выключенном питании.
    Проверьте механические направления без нагрузки перед первым запуском.

*/

// Arduino IDE вставляет прототипы функций перед первым объявлением в скетче.
// Все используемые в этих прототипах типы должны быть доступны из ПЕРВОГО подключения.
// В частности, здесь определены WiFiEvent_t, WiFiEventInfo_t и TmcAxis,
// а также TmcUartSettings, TMC2208Stepper, AsyncWebServerRequest и RemuageProgram.
#include "RemuatorCompileDeps.h"
#include <atomic>
#include "user_guide_page.h"

// Строка версии для диагностики и веб-интерфейса API.
#define REMUATOR_VERSION "12.5.1"

// Arduino-ESP32 3.x: размер стека loopTask в байтах; ранее было 8192 байта.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Для функций со сложными типами параметров прототипы объявлены вручную.
// Это не позволяет препроцессору Arduino поместить прототипы раньше определений типов.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void reportTmcConnection(TmcAxis &axis, const char *name, const TmcUartSettings &settings);
void loadTmcSettings(TmcUartSettings &c, const char *x);
void saveTmcSettings(const TmcUartSettings &c, const char *x);
bool persistTmcFile(const TmcUartSettings &y,const TmcUartSettings &z,uint32_t ya,uint32_t ys,uint32_t za,uint32_t zs);
void applyTmcSettings(TMC2208Stepper &d, const TmcUartSettings &c);
bool applyTmcSettingsVerified(TmcAxis &axis, const TmcUartSettings &c);
bool fmAuthenticated(AsyncWebServerRequest *r);
String fmArg(AsyncWebServerRequest *r, const char *key, bool post = false);
void copyProgramNameUtf8(char *dst, size_t dstSize, const String &src);

// Отключённый пример подключения: #include <gpio_viewer.h>
// Отключённый пример объявления: GPIOViewer gpio_viewer;

// ============================================================
//                 НАСТРОЙКИ UART / TMC
// ============================================================
TmcUartSettings uartNaklon{TmcModel::Tmc2208, 0};
TmcUartSettings uartPovorot{TmcModel::Tmc2209, 1};

// ============================================================
//                    РАСПИНОВКА ESP32-S3
// ============================================================

constexpr uint8_t Y_DIR_PIN = 4;
constexpr uint8_t Y_STEP_PIN = 5;
constexpr uint8_t Y_ENABLE_PIN = 6; // EN Y, активный LOW

constexpr uint8_t Z_DIR_PIN = 42;
constexpr uint8_t Z_STEP_PIN = 41;
constexpr uint8_t Z_ENABLE_PIN = 40; // EN Z, активный LOW

// NC концевик -> GND; отдельный РЕЗИСТОР 10 кОм -> +3,3 В.
// LOW = нормально замкнуто, HIGH = предел / обрыв цепи.
// Используем INPUT, не внутреннюю подтяжку.
constexpr uint8_t Y_END_PIN = 14;       // 23°
constexpr uint8_t Y_UPPER_END_PIN = 13; // 85°
constexpr int32_t Y_HOME_TRAVEL_STEPS = 108410; // Резервное значение до калибровки ADXL345.
constexpr int32_t Y_ENDSTOP_SEARCH_STEPS = 200000000; // Условная дальняя цель поиска, а не предел калибровочного хода.
constexpr uint32_t Y_LIMIT_SEARCH_TIMEOUT_SEC = 180; // Предельный защитный тайм-аут: 3 минуты.
constexpr uint64_t RECOVERY_DELAY_US = 2000000ULL;

// Все физические кнопки ВРЕМЕННО ОТКЛЮЧЕНЫ. Их функции оставлены в скетче.
// Прежние значения остаются лишь для последующего переназначения: GPIO5/6
// теперь управляют STEP/EN двигателя Y и НЕ могут использоваться как кнопки.
constexpr bool HARDWARE_BUTTONS_ENABLED = false;
constexpr uint8_t BTN_RIGHT = 5;
constexpr uint8_t BTN_AUTO = 6;
constexpr uint8_t BTN_MENU = 7;
constexpr uint8_t BTN_LEFT = 17;
constexpr uint8_t BTN_POWER = 18;
constexpr uint8_t BTN_SUPPLY_PIN = 4; // занят Y_DIR, НЕ включать питание кнопок!

// Общая I2C: OLED SSD1306 + DS3231 (адрес 0x68).
constexpr uint8_t I2C_SDA_PIN = 2;
constexpr uint8_t I2C_SCL_PIN = 1;

// Общая однопроводная полудуплексная UART.
constexpr uint8_t TMC_UART_RX = 47;
constexpr uint8_t TMC_UART_TX = 21;
constexpr uint32_t TMC_DEFAULT_BAUD = 115200;

// Не допускаем повторного назначения одного GPIO разным сигналам.
constexpr uint8_t REMUATOR_GPIO_PINS[] = {
    Y_STEP_PIN, Y_DIR_PIN, Y_ENABLE_PIN, Z_STEP_PIN, Z_DIR_PIN, Z_ENABLE_PIN,
    Y_END_PIN, Y_UPPER_END_PIN, I2C_SDA_PIN, I2C_SCL_PIN, TMC_UART_RX, TMC_UART_TX};
// Неактивные кнопки намеренно не включены в проверку: они НЕ настраиваются
// через pinMode и НЕ опрашиваются, их старые номера конфликтуют с Z.
constexpr bool remuatorPinsUnique();

// ============================================================
//                       НАСТРОЙКИ
// ============================================================

#define SERIAL_DEBUG 1
// В NVS включено по умолчанию; выключатель влияет на все DBG/DBGLN.
volatile bool serialOutputEnabled = true;
#define DEBUG_BAUD 115200

// Период обслуживания Web-запросов.
// Это отдельная переменная, как требовалось.
// Важно: WebServer.handleClient() всё равно вызывается регулярно,
// а переменная задаёт минимальный интервал вызова.
uint32_t WEB_REQUEST_PERIOD_MS = 20;
uint32_t WEB_PAGE_REFRESH_PERIOD_MS = 60000;

// Механика
uint32_t NAKLON_ACCELERATION = 10000;
uint32_t NAKLON_MAX_SPEED = 6000;
uint32_t POVOROT_ACCELERATION = 10000;
uint32_t POVOROT_MAX_SPEED = 6000;

// Паузы исходного скетча
uint32_t PAUSE_AFTER_NAKLON_MS = 500;

// Максимальное время движения одного действия
uint32_t MAX_ACTION_TIME_SEC = 3600;

// До первой калибровки это лишь значение для индикации, запуск блокируется.
constexpr int32_t DEFAULT_Z_STEPS_360 = 40392;
int32_t zCalSteps360 = DEFAULT_Z_STEPS_360;
uint16_t zCalMicrosteps = 0;
bool zCalStored = false;
uint32_t zCalSavedHash = 0;
inline uint32_t zCalibrationDigest(int32_t steps, uint16_t micro) {
  uint32_t x=0x5A364331UL; // Постоянная сигнатура схемы Z6C1.
  x^=(uint32_t)steps; x=(x<<13)|(x>>19);
  x^=(uint32_t)micro*2654435761UL;
  return x;
}
bool zCalStarted = false;
bool zCalJogging = false;
int8_t zCalDirection = 0;
uint32_t zCalJogAt = 0;
uint32_t zCalHeartbeat = 0;
std::atomic<int32_t> zCalStepSnapshot{0};
constexpr int32_t Z_CAL_MAX_ABS_STEPS = 1000000;
constexpr uint32_t Z_CAL_DEADMAN_MS = 2200;
inline bool zCalibrationValid() {
  return zCalStored && zCalSteps360 >= 100 && zCalSteps360 <= Z_CAL_MAX_ABS_STEPS &&
         zCalMicrosteps == uartPovorot.microsteps &&
         zCalSavedHash == zCalibrationDigest(zCalSteps360,zCalMicrosteps);
}
inline int32_t currentZSteps360() {
  return zCalibrationValid() ? zCalSteps360 : DEFAULT_Z_STEPS_360;
}
void zCalibrationInvalidate();
void zCalibrationStopMotion();
String overallCalibrationReason();
bool overallCalibrationReady();

// NTP/Wi-Fi: можно изменять через Настройки, значения сохраняются в NVS.
String NTP_SERVER_1 = "pool.ntp.org";
String NTP_SERVER_2 = "time.nist.gov";
uint32_t NTP_SYNC_PERIOD_SEC = 86400UL; // интервал между подтверждёнными NTP, секунды

// Часовой пояс по умолчанию: UTC+2 зимой/UTC+3 летом для Европы.
// POSIX TZ поддерживает переход на летнее время.
String TZ_INFO = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// Точка доступа Wi-Fi.
String apSSID = "RemuageVRN";
String apPassword = "43214321";

// Ограничения
// Постоянная MAX_PROGRAM_STEPS объявлена в program_model.h.

// Встроенный пароль OTA, заданный пользователем. Инструкция: OTA_RU.md.
#define REMUATOR_OTA_PASSWORD "123456789A+-"

// ============================================================
//                      БИБЛИОТЕКИ/ОБЪЕКТЫ
// ============================================================

GStepper<STEPPER2WIRE> motor_Naklon(200, Y_STEP_PIN, Y_DIR_PIN);
GStepper<STEPPER2WIRE> motor_Povorot(200, Z_STEP_PIN, Z_DIR_PIN);

GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;

Preferences prefs;
void zCalibrationInvalidate() {
  zCalStored = false;
  prefs.putBool("zCalValid", false);
}
Ds3231Clock rtcClock(Wire);
bool rtcPresent = false;
bool rtcTimeValid = false;
uint32_t lastRtcConfirmedEpoch = 0;
bool rtcWritePending = false;
uint32_t lastRtcReadMs = 0;
uint32_t lastRtcWriteAttemptMs = 0;
constexpr uint32_t RTC_CHECK_PERIOD_MS = 300000UL;

// Основной интерфейс использует ESPAsyncWebServer на порту 80. Слой совместимости сохраняет
// существующие обработчики при переходе к асинхронным обработчикам запросов.
// Асинхронный файловый менеджер LittleFS слушает порт 8080; синхронный WebServer не нужен.
SemaphoreHandle_t controlMutex = nullptr;
// Запросы отклоняются ДО захвата мьютекса, пока выдаются импульсы STEP.
bool stepMotionPending();
bool calibrationBusy();
bool controllerRestartPending();
void handleRoot();
void handleBootstrapSetup();
void handleBootstrapWifiSave();
void handleNetworkSave();
String bootstrapStatusJson();
bool beginZHome();
std::atomic<bool> otaInProgress{false};
String otaStatusJson();
class ControlGuard {
  bool acquired=false;
public:
  ControlGuard() {
    acquired=!controlMutex || xSemaphoreTakeRecursive(controlMutex,portMAX_DELAY)==pdTRUE;
  }
  // Сетевой обработчик не ждёт бесконечно: OTA удерживает управление на всю передачу.
  explicit ControlGuard(AsyncWebServerRequest *request,bool reply=true) {
    if(!otaInProgress.load())
      acquired=!controlMutex || xSemaphoreTakeRecursive(controlMutex,pdMS_TO_TICKS(25))==pdTRUE;
    // Закрываем окно между первой проверкой OTA и получением мьютекса.
    if(acquired && otaInProgress.load()) {
      if(controlMutex) xSemaphoreGiveRecursive(controlMutex);
      acquired=false;
    }
    if(!acquired && reply) request->send(503,"text/plain; charset=utf-8",
      otaInProgress.load() ? "Идёт обновление OTA. Дождитесь перезагрузки контроллера" :
      "Контроллер занят. Повторите запрос после завершения операции");
  }
  explicit operator bool() const {return acquired;}
  ~ControlGuard() {
    if(acquired && controlMutex) xSemaphoreGiveRecursive(controlMutex);
  }
};
class AsyncCompatServer {
public:
  explicit AsyncCompatServer(uint16_t port) : asyncServer(port) {}
  void on(const char *uri, HTTPMethod method, void (*handler)()) {
    asyncServer.on(AsyncURIMatcher::exact(uri), (WebRequestMethodComposite)method,
                   [this, handler](AsyncWebServerRequest *request) {
                     const String path=request->url();
                     // Чтение таблиц CSV, формирование HTML и операции LittleFS
                     // не должны удерживать мьютекс управления во время GyverStepper.tick.
                     if (stepMotionPending() && path != "/api/calibration/z" && path != "/api/manual" &&
                         path != "/api/calibration/y" && path != "/api/live" && path != "/api/power" && path != "/program/stop" && path != "/api/reboot" && path != "/api/motor-test") {
                       request->send(423,"text/plain; charset=utf-8", "Двигатели движутся; обновите страницу после остановки");
                       return;
                     }
                     ControlGuard guard(request);if(!guard)return;
                     currentRequest = request;
                     responseHeaders.clear();
                     if (controllerRestartPending() && path != "/api/reboot")
                       request->send(409,"text/plain; charset=utf-8","Контроллер перезагружается; дождитесь запуска");
                     else handler();
                     currentRequest = nullptr;
                   });
  }
  void on(const char *uri, HTTPMethod method, void (*handler)(), void (*uploadHandler)()) {
    asyncServer.on(
        AsyncURIMatcher::exact(uri), (WebRequestMethodComposite)method,
        [this, handler, uploadHandler](AsyncWebServerRequest *request) {
          if(stepMotionPending() && request->url()!="/api/program" &&
             request->url()!="/api/calibration/y") {
            request->send(423,"text/plain; charset=utf-8","Нельзя сохранять файлы при движении");return;
          }
          ControlGuard guard(request);if(!guard)return;
          if(compatUploadFailed) {
            currentRequest=request;compatUpload.status=UPLOAD_FILE_ABORTED;uploadHandler();currentRequest=nullptr;
            compatUploadFailed=false;request->send(503,"text/plain; charset=utf-8","Загрузка прервана занятостью контроллера; повторите файл целиком");return;
          }
          currentRequest = request;
          responseHeaders.clear();
          if(controllerRestartPending()) request->send(409,"text/plain; charset=utf-8","Контроллер перезагружается; дождитесь запуска");
          else handler();
          currentRequest = nullptr;
        },
        [this, uploadHandler](AsyncWebServerRequest *request, String filename, size_t index,
                              uint8_t *data, size_t len, bool final) {
          if(index==0) compatUploadFailed=false;
          ControlGuard guard(request,false);
          if(!guard) {compatUploadFailed=true;return;}
          if(compatUploadFailed) return;
          currentRequest = request;
          compatUpload.filename = filename;
          compatUpload.name = "file";
          compatUpload.totalSize = index + len;

          // Буфер HTTPUpload::buf — массив на 1436 байт, а не указатель.
          // Сначала отдельно передаётся START, затем каждый полученный фрагмент копируется в буфер.
          if (index == 0) {
            compatUpload.currentSize = 0;
            compatUpload.status = UPLOAD_FILE_START;
            uploadHandler();
          }
          size_t offset = 0;
          const size_t capacity = sizeof(compatUpload.buf);
          while (offset < len) {
            size_t chunk = len - offset;
            if (chunk > capacity)
              chunk = capacity;
            memcpy(compatUpload.buf, data + offset, chunk);
            compatUpload.currentSize = chunk;
            compatUpload.status = UPLOAD_FILE_WRITE;
            uploadHandler();
            offset += chunk;
          }
          if (final) {
            compatUpload.currentSize = 0;
            compatUpload.status = UPLOAD_FILE_END;
            uploadHandler();
          }
          currentRequest = nullptr;
        });
  }
  void onNotFound(void (*handler)()) {
    asyncServer.onNotFound([this, handler](AsyncWebServerRequest *request) {
      if(stepMotionPending()){request->send(423,"text/plain; charset=utf-8","Движение: чтение LittleFS отложено");return;}
      ControlGuard guard(request);if(!guard)return;
      currentRequest = request;
      responseHeaders.clear();
      handler();
      currentRequest = nullptr;
    });
  }
  void onMemoryPage(const char *uri,const char *page) {
    asyncServer.on(AsyncURIMatcher::exact(uri),(WebRequestMethodComposite)HTTP_GET,
      [page](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response=request->beginResponse(200,"text/html; charset=utf-8",page);
        response->addHeader("Cache-Control","no-cache");request->send(response);
      });
  }
  void enableOtaStatus() {
    // Только атомарные счётчики и метаданные разделов: мьютекс управления и LittleFS не нужны.
    asyncServer.on(AsyncURIMatcher::exact("/api/ota"),(WebRequestMethodComposite)HTTP_GET,
      [](AsyncWebServerRequest *request){request->send(200,"application/json; charset=utf-8",otaStatusJson());});
  }
  void begin() { asyncServer.begin(); }
  bool hasArg(const String &name) {
    if (!currentRequest)
      return false;
    return currentRequest->hasParam(name, true) || currentRequest->hasParam(name, false);
  }
  String arg(const String &name) {
    if (!currentRequest)
      return String();
    const AsyncWebParameter *p = currentRequest->getParam(name, true);
    if (!p)
      p = currentRequest->getParam(name, false);
    return p ? p->value() : String();
  }
  String uri() { return currentRequest ? currentRequest->url() : String("/"); }
  HTTPUpload &upload() { return compatUpload; }
  void sendHeader(const String &name, const String &value) {
    responseHeaders.push_back({name, value});
  }
  void send(int code) { send(code, "text/plain; charset=utf-8", String()); }
  void send(int code, const char *contentType, const String &body) {
    if (!currentRequest)
      return;
    // Изменения через AJAX не должны перенаправлять или перезагружать документ. Ответ 303 старых
    // обработчиков для fetch преобразуется в небольшое подтверждение JSON.
    bool ajax = currentRequest->hasHeader("X-Requested-With") &&
                currentRequest->getHeader("X-Requested-With")->value() == "XMLHttpRequest";
    int actualCode = code;
    String actualType = contentType ? String(contentType) : String("text/plain");
    String actualBody = body;
    if (ajax && code >= 300 && code < 400) {
      actualCode = 200;
      actualType = "application/json";
      actualBody = "{\"ok\":true,\"message\":\"saved\"}";
    }
    AsyncWebServerResponse *response =
        currentRequest->beginResponse(actualCode, actualType.c_str(), actualBody);
    for (auto &h : responseHeaders) {
      if (ajax && h.first.equalsIgnoreCase("Location"))
        continue;
      response->addHeader(h.first.c_str(), h.second.c_str());
    }
    // Страницы читаются из LittleFS: браузер не должен повторно использовать устаревшие HTML и данные API.
    bool hasCacheControl = false;
    for (const auto &h : responseHeaders)
      if (h.first.equalsIgnoreCase("Cache-Control"))
        hasCacheControl = true;
    if (!hasCacheControl)
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    responseHeaders.clear();
    currentRequest->send(response);
  }
  void send(int code, const char *contentType, const char *body) {
    send(code, contentType, String(body ? body : ""));
  }
  // Двоичные ресурсы, особенно .ico, нельзя читать в Arduino String:
  // нулевые байты обрывают текстовый ответ и повреждают содержимое PNG/ICO.
  void streamFile(File &file, const String &contentType) {
    if (!file || !currentRequest) {
      send(404, "text/plain", "FILE_OPEN_ERROR");
      return;
    }
    String path = String(file.path());
    AsyncWebServerResponse *response =
        currentRequest->beginResponse(LittleFS, path, contentType, false);
    bool hasCacheControl = false;
    for (auto &h : responseHeaders) {
      if (h.first.equalsIgnoreCase("Cache-Control"))
        hasCacheControl = true;
      response->addHeader(h.first.c_str(), h.second.c_str());
    }
    if (!hasCacheControl)
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    responseHeaders.clear();
    currentRequest->send(response);
  }

private:
  AsyncWebServer asyncServer;
  AsyncWebServerRequest *currentRequest = nullptr;
  HTTPUpload compatUpload;
  bool compatUploadFailed=false;
  std::vector<std::pair<String, String>> responseHeaders;
};
AsyncCompatServer server(80);
AsyncWebServer filesServer(8080);
bool filesServerReady = false;

std::atomic<bool> otaServiceEnabled{false};
std::atomic<uint32_t> otaTransferred{0},otaTotal{0};
std::atomic<int> otaLastError{-1},otaFlashError{0};
std::atomic<bool> otaTransferComplete{false};
wifi_ps_type_t otaSavedSleep=WIFI_PS_NONE;
bool otaSleepChanged=false;

// Сетевые настройки и файл обмена программами. Объявлены заранее,
// поскольку используются функциями CSV выше сетевого раздела.
String savedSSID;
String savedPassword;
bool homeWiFiEnabled = true;
uint32_t lastStaReconnectMs = 0;
bool saveOneProgramFile(size_t p);
void refreshPrograms(bool force = false);
void markProgramsDirty(const String &path);
bool mountProgramFS();
void setupAsyncFileManager();
bool pauseProgram();
bool resumeProgram();

HardwareSerial TmcSerial(1);

// Оба объекта используют один UART. Адреса должны совпадать с MS1/MS2.
// 0.11 Ом сохранено из исходного скетча; проверьте Rsense ваших модулей.
TmcAxis tmcNaklon;
TmcAxis tmcPovorot;

// Явные прототипы: тип настроек должен быть известен Arduino-препроцессору.
void normalizeTmcBusSettings(uint32_t baud);
void applyTmcSettings(TMC2208Stepper &drv, const TmcUartSettings &settings);

// ============================================================
//                   ТАБЛИЦА УГОЛ -> ШАГИ
// ============================================================

const int32_t ANGLE_STEPS[][2] = {{23, 0},     {30, 15334},  {35, 25000}, {40, 34674}, {45, 45000},
                                  {50, 55118}, {55, 65000},  {60, 74710}, {65, 82000}, {70, 90542},
                                  {75, 96000}, {80, 104111}, {85, 108410}};
const uint8_t ANGLE_COUNT = sizeof(ANGLE_STEPS) / sizeof(ANGLE_STEPS[0]);

// ============================================================
//                  СТРУКТУРА ЭТАПА ПРОГРАММЫ
// ============================================================

// Типы ProgramStep и RemuageProgram объявлены в program_model.h.

// Явные прототипы важны для генератора прототипов Arduino IDE.
void startProgram(uint16_t requestedStep = 1);
void createDefaultProgram(RemuageProgram &target, const String &name = "7 дней");
bool writeProgramCsv(const String &path, const RemuageProgram &program);
bool readProgramCsv(const String &path, RemuageProgram &loaded, String *error = nullptr);

RemuageProgram runProgram = {};
RemuageProgram draftProgram = {};
String draftPath;
bool draftLoaded = false, draftDirty = false;
uint32_t draftRevision = 1, draftSourceHash = 0;
String executionMessage;
bool resumeRecoveredProgram();
void finishProgram();
bool openProgramDraft(const String &path, String &error);
bool recoveryRequired = false, recoveryMotionUncertain = false;
std::atomic<bool> programTiltSamplingActive{false};
bool sensorFallbackY=false, yBootReferencePending=true, xProtectionTripped=false;
uint32_t bootSensorWaitMs=0;
constexpr uint32_t SENSOR_RECOVERY_TIMEOUT_MS=5000;
uint32_t controllerBootId=0, controllerCycle=0;
bool recoveryCountdownStarted=false;
ProgramInterval stepInterval, settleInterval;
uint64_t actionStartedUs = 0;
uint32_t executionHash = 0;
std::vector<RemuageProgram> programs;
std::vector<String> programPaths;
String selectedProgramPath;
bool programsDirty = true;
std::vector<String> invalidProgramFiles;
std::vector<String> invalidProgramErrors;
uint32_t lastProgramScanMs = 0;

// ============================================================
//                    СОСТОЯНИЕ ВЫПОЛНЕНИЯ
// ============================================================

enum ProgramState {
  IDLE = 0,
  MOVING_NAKLON,
  WAITING_AFTER_NAKLON,
  MOVING_POVOROT,
  WAITING_BETWEEN_STEPS
};

struct ExecutionState {
  uint32_t magic;
  uint32_t checksum;
  uint32_t programHash;
  uint16_t lastCompletedStep; // В версиях 11.0/11.1 это было неиспользуемое поле programIndex.
  uint16_t currentStep;
  uint8_t state;
  bool running;
  bool paused;
  bool naklonCompleted;
  bool povorotCompleted;
  bool motionUncertain;
  int32_t naklonPosition;
  int32_t povorotPosition;
  int64_t lastCompletedEpoch; // Начало шага уже хранится в schedule[currentStep].
  int64_t savedEpoch;
  uint64_t remainingUs;
  uint64_t settleRemainingUs;
  time_t schedule[MAX_PROGRAM_STEPS + 1];
};
const uint32_t LEGACY_EXEC_MAGIC = 0x524D563BUL;
const uint32_t V112_EXEC_MAGIC = 0x524D563CUL;
const uint32_t EXEC_MAGIC = 0x524D563DUL;
ExecutionState execState = {};
struct LegacyExecutionRecord {
  ExecutionState execution;
  RemuageProgram program;
};
struct RunMetadata {
  int64_t startedEpoch;
  int64_t finishedEpoch;
  int64_t outageEpoch;
  int64_t powerOnEpoch;
  bool noticeVisible;
  bool upperReached;
  char logPath[80];
  char name[120];
};
RunMetadata runMeta = {};
struct ExecutionRecord {
  ExecutionState execution;
  RemuageProgram program;
  RunMetadata metadata;
};
#include "execution_store.h"
#include "tmc_settings_file.h"
struct TmcFileStorage : ExecutionFileStorage {
  bool ensureDirectory() {return LittleFS.exists("/settings") || LittleFS.mkdir("/settings");}
} tmcFileStorage;
TmcSettingsFile<TmcFileStorage> tmcSettingsFile(tmcFileStorage);
bool persistTmcFile(const TmcUartSettings &y,const TmcUartSettings &z,uint32_t ya,uint32_t ys,uint32_t za,uint32_t zs) {
  TmcFileValues v;v.y=y;v.z=z;v.yAcceleration=ya;v.ySpeed=ys;v.zAcceleration=za;v.zSpeed=zs;
  return tmcSettingsFile.save(v);
}

bool executionReadError = false;
bool executionJournalDegraded = false;

size_t selectedProgram = 0;
bool programPaused = false;
bool pauseStopPending = false;
uint32_t programPausedAtMillis = 0;
ProgramState programState = IDLE;
bool programRunning = false;
uint16_t currentProgramStep = 0;

uint32_t actionCompleteMillis = 0;
uint32_t lastWebHandle = 0;
uint32_t lastNtpSyncEpoch = 0;
uint32_t lastBrowserSyncEpoch = 0;
uint8_t clockSource = 0; // Источник времени: 0 — неизвестен, 1 — NTP, 2 — браузер, 3 — DS3231.
uint32_t lastNtpAttemptMs = 0;
bool ntpRequested = false;
std::atomic<uint32_t> receivedNtpEpoch{0};
uint32_t lastOledRefresh = 0;

bool oledDirty = true;
int32_t oledLastMinuteKey = -1;
uint32_t oledLastNetworkIcon = 0;
uint8_t oledNetworkAnim = 0;
uint32_t pauseStartMillis = 0;
uint32_t pauseDurationMillis = 0;
time_t scheduledEpoch[MAX_PROGRAM_STEPS + 1] = {};
bool scheduleValid = false;

bool serialMoveNaklonActive = false;
bool serialMovePovorotActive = false;
bool motorNaklonMoving = false;
bool motorPovorotMoving = false;
int moveDirectionNaklon = 0;
int moveDirectionPovorot = 0;

bool endstopYActive = false;
bool endstopUpperActive = false;
bool yEndstopFault = false;
NcEndstop lowerEndstop, upperEndstop;
bool homingActive = false;
bool homingZPhase = false;
bool homePowerConsumed = false;
uint64_t homingStartedUs = 0;
bool recoveryAutoPending = false;
uint64_t recoveryReadyUs = 0;
bool recoveryPowerLogged = false;
uint64_t lastCheckpointUs = 0;
String logError;

String serialInput;

// Кнопка питания.
bool powerButtonPressed = false;
uint32_t powerButtonPressTime = 0;
const uint32_t LONG_PRESS_TIME = 1500;

// ============================================================
// ОТЛАДКА
// ============================================================
bool calibrationBusy();

// Вывод в USB Serial никогда не выполняется во время выдачи STEP.
// Проверяем также незавершённые target, а не только getState(): после
// setTarget() tick() ещё может не успеть изменить внутренний флаг движения.
// Отбрасываем диагностические сообщения во время движения: даже запись
// в кольцевой буфер Serial/USB и построение String создают задержки.
inline bool stepMotionPending() {
  return calibrationBusy() || motor_Naklon.getCurrent() != motor_Naklon.getTarget() ||
         motor_Povorot.getCurrent() != motor_Povorot.getTarget() ||
         motor_Naklon.getState() || motor_Povorot.getState() ||
         homingActive || pauseStopPending ||
         (!xProtectionTripped && (serialMoveNaklonActive || serialMovePovorotActive));
}
char startupSerial[24577] = {};
size_t startupSerialLength = 0;
std::atomic<bool> captureStartupSerial{true};
portMUX_TYPE startupSerialMux = portMUX_INITIALIZER_UNLOCKED;
#if SERIAL_DEBUG
template<typename T> void captureStartup(const T &value) {
  if (!captureStartupSerial.load()) return;
  String part(value);
  portENTER_CRITICAL(&startupSerialMux);
  if (captureStartupSerial.load() && startupSerialLength + part.length() <= sizeof(startupSerial)-1) {
    memcpy(startupSerial+startupSerialLength,part.c_str(),part.length());
    startupSerialLength += part.length();startupSerial[startupSerialLength]=0;
  }
  portEXIT_CRITICAL(&startupSerialMux);
}
#define DBG(x) do { if (!stepMotionPending()) { captureStartup(x); if (serialOutputEnabled) Serial.print(x); } } while (0)
#define DBGLN(x) do { if (!stepMotionPending()) { captureStartup(x); captureStartup("\n"); if (serialOutputEnabled) Serial.println(x); } } while (0)
// Минимальный свободный стек loopTask с момента запуска; ESP-IDF выдаёт значение в байтах.
void debugLoopStack(const char *stage) {
  if (stepMotionPending()) return;
  char line[160];
  snprintf(line,sizeof(line),"[stack] %s: low-water free=%u / %u bytes\n", stage,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned)getArduinoLoopTaskStackSize());
  DBG(line);
}
#else
#define DBG(x)
#define DBGLN(x)
void debugLoopStack(const char *) {}
#endif

// Диагностика Wi-Fi событий: причина отключения AP-клиента будет видна
// в Serial Monitor. Это позволит точно отличить проблему AP от HTTP/NVS.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_AP_START:
    DBGLN("WiFi EVT: AP START");
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    DBGLN("WiFi EVT: AP CLIENT CONNECTED");
    break;
  case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
    DBGLN(String("WiFi EVT: AP CLIENT IP ") + WiFi.softAPIP().toString());
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    DBGLN(String("WiFi EVT: AP CLIENT DISCONNECTED reason=") +
          String(info.wifi_ap_stadisconnected.reason));
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    DBGLN(String("WiFi EVT: HOME GOT IP ") + WiFi.localIP().toString());
    DBGLN(String("WiFi EVT: AP is still ") + WiFi.softAPIP().toString());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    DBGLN(String("WiFi EVT: HOME DISCONNECTED, AP remains ") + WiFi.softAPIP().toString());
    break;
  default:
    break;
  }
}

// ============================================================
//                    ВСТРОЕННЫЙ RGB LED
// ============================================================
// Проверка размещена после типов программы, чтобы сохранить порядок
// автопрототипов Arduino для функций с RemuageProgram/ProgramStep.
constexpr bool remuatorPinsUnique() {
  for (size_t i = 0; i < sizeof(REMUATOR_GPIO_PINS); ++i)
    for (size_t j = i + 1; j < sizeof(REMUATOR_GPIO_PINS); ++j)
      if (REMUATOR_GPIO_PINS[i] == REMUATOR_GPIO_PINS[j])
        return false;
  return true;
}
static_assert(remuatorPinsUnique(), "Remuator GPIO conflict: check pin assignments");

// На ESP32-S3 UNO встроенный адресный RGB LED доступен через
// RGB_BUILTIN (для этой платы ранее определялся как GPIO97).
// Только зелёный = STA подключилась к домашнему роутеру.
// Синий = есть клиент AP, жёлтый = AP запущена, но клиентов нет.
// Красный = нет даже точки доступа. AP работает одновременно со STA.
void setConnectionLED() {
  const bool homeWiFi = (WiFi.status() == WL_CONNECTED);
  const bool apRunning =
      ((WiFi.getMode() & WIFI_AP) != 0) && (WiFi.softAPIP() != IPAddress(0, 0, 0, 0));
  const bool apClient = apRunning && (WiFi.softAPgetStationNum() > 0);
  if (homeWiFi)
    rgbLedWrite(RGB_BUILTIN, 0, 100, 0);
  else if (apClient)
    rgbLedWrite(RGB_BUILTIN, 0, 0, 100);
  else if (apRunning)
    rgbLedWrite(RGB_BUILTIN, 80, 45, 0);
  else
    rgbLedWrite(RGB_BUILTIN, 100, 0, 0);
}

// ============================================================
//                         УТИЛИТЫ
// ============================================================

bool normalDriverPower = false;
uint32_t rebootAt = 0;
bool controllerRestartPending() {return rebootAt != 0;}
bool motorTestActive = false;
uint32_t motorTestHeartbeat = 0, motorTestStarted = 0;
void releaseDrivers() {
  if(yEndstopFault || recoveryMotionUncertain || rebootAt || xProtectionTripped) {
    normalDriverPower=false;
    digitalWrite(Y_ENABLE_PIN,HIGH);digitalWrite(Z_ENABLE_PIN,HIGH);return;
  }
  normalDriverPower = true;
  digitalWrite(Y_ENABLE_PIN, (calibrationBusy() || (programTiltSamplingActive.load() && programState==MOVING_NAKLON) || uartNaklon.holdEnabled || motor_Naklon.getState()) ? LOW : HIGH);
  digitalWrite(Z_ENABLE_PIN, (uartPovorot.holdEnabled || motor_Povorot.getState()) ? LOW : HIGH);
}
void enableDrivers() {
  if(xProtectionTripped) {disableDrivers();return;}
  normalDriverPower = true;
  digitalWrite(Y_ENABLE_PIN, LOW);
  digitalWrite(Z_ENABLE_PIN, LOW);
}

void disableDrivers() {
  normalDriverPower = false;
  digitalWrite(Y_ENABLE_PIN, HIGH);
  digitalWrite(Z_ENABLE_PIN, HIGH);
}

void setupDriverEnablePins() {
  pinMode(Y_ENABLE_PIN, OUTPUT);
  digitalWrite(Y_ENABLE_PIN, HIGH);
  pinMode(Z_ENABLE_PIN, OUTPUT);
  digitalWrite(Z_ENABLE_PIN, HIGH);
}

String htmlEscape(const String &s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  r.replace("'", "&#39;");
  return r;
}

String urlDecode(String s) {
  s.replace("+", " ");
  for (int i = 0; i < (int)s.length() - 2; i++) {
    if (s[i] == '%' && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) {
      String hex = s.substring(i + 1, i + 3);
      char c = (char)strtol(hex.c_str(), nullptr, 16);
      s = s.substring(0, i) + String(c) + s.substring(i + 3);
    }
  }
  return s;
}

// Веб-интерфейс v10.7-async-fs: оболочка, CSS и JS загружаются из LittleFS.
String contentTypeForPath(const String &path) {
  if (path.endsWith(".html"))
    return "text/html; charset=utf-8";
  if (path.endsWith(".css"))
    return "text/css; charset=utf-8";
  if (path.endsWith(".js"))
    return "application/javascript; charset=utf-8";
  if (path.endsWith(".ico"))
    return "image/x-icon";
  if (path.endsWith(".csv"))
    return "text/csv; charset=utf-8";
  if (path.endsWith(".json"))
    return "application/json; charset=utf-8";
  if (path.endsWith(".txt"))
    return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

#include "bootstrap_policy.h"
#include "bootstrap_pages.h"

// Общедоступные веб-файлы находятся непосредственно в корне LittleFS.
void sendLittleFSFile(String path) {
  if (!path.startsWith("/") || path.indexOf('/', 1) >= 0 || path.indexOf("..") >= 0 ||
      !(path.endsWith(".html") || path.endsWith(".css") || path.endsWith(".js") ||
        path.endsWith(".ico"))) {
    server.send(404, "text/plain; charset=utf-8", "Not found");
    return;
  }
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory()) {
    if(path.endsWith(".html")) {
      server.sendHeader("Location","/setup");server.send(303);return;
    }
    server.send(404, "text/plain; charset=utf-8",
                "Нет веб-файла " + path +
                    ". Загрузите файлы из data в корень LittleFS через менеджер :8080 или "
                    "загрузчик LittleFS.");
    return;
  }
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.streamFile(file, contentTypeForPath(path));
  file.close();
}

// Неизвестные адреса могут соответствовать только общедоступным веб-файлам в корне.
void handleStaticFile() { sendLittleFSFile(server.uri()); }
void handleStaticIndex() { handleRoot(); }
void handleStaticCSS() { sendLittleFSFile("/style.css"); }
void handleStaticJS() { sendLittleFSFile("/app.js"); }
void handleStaticIcon() { sendLittleFSFile("/favicon.ico"); }
void handleReadme() {
  // Файл README_RU.md в LittleFS доступен только для чтения; загружается вместе с прошивкой.
  if (stepMotionPending()) { server.send(409,"text/plain; charset=utf-8","Двигатели работают: прочитайте после остановки");return; }
  File f=LittleFS.open("/README_RU.md","r");
  if (!f) {server.send(404,"text/plain; charset=utf-8","README_RU.md отсутствует в LittleFS");return;}
  server.sendHeader("Cache-Control","no-store");
  server.streamFile(f,"text/markdown; charset=utf-8"); f.close();
}

// ============================================================
//                         ВРЕМЯ
// ============================================================

bool timeValid() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

String dateTimeString(time_t t) {
  if (t < 1000000000)
    return "--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tmv);
  return String(buf);
}

String dateTimeShort(time_t t) {
  if (t < 1000000000)
    return "--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmv);
  return String(buf);
}

time_t nowEpoch() { return time(nullptr); }

void applyTimezone() {
  setenv("TZ", TZ_INFO.c_str(), 1);
  tzset();
}

// DS3231 хранит только UTC. Часовой пояс применяется на стороне ESP32/OLED.
// Не принимаем время RTC, если осциллятор останавливался (бит OSF=1).
void restoreTimeFromRtc() {
  time_t rtcEpoch = 0;
  rtcTimeValid = rtcClock.readUtc(rtcEpoch);
  rtcPresent = rtcClock.present();
  lastRtcReadMs = millis();
  if (!rtcTimeValid) {
    DBGLN(rtcPresent ? "RTC DS3231: время не подтверждено (OSF/BCD), нужен NTP или браузер"
                     : "RTC DS3231: не обнаружен на I2C 0x68");
    return;
  }
  // При каждой загрузке RTC — опорный календарь, даже при программном RESET,
  // когда системные часы могли остаться от предыдущего запуска.
  struct timeval value = {};
  value.tv_sec = rtcEpoch;
  if (settimeofday(&value, nullptr) == 0) {
    lastRtcConfirmedEpoch = (uint32_t)rtcEpoch;
    clockSource = 3;
    DBGLN(String("RTC DS3231 UTC: ") + String((long long)rtcEpoch));
    oledMarkDirty();
  }
}

void serviceRtcClock() {
  // Не делаем I2C-запросы во время выдачи STEP: они блокируют loop().
  if (motor_Naklon.getState() || motor_Povorot.getState() || homingActive ||
      serialMoveNaklonActive || serialMovePovorotActive)
    return;
  const uint32_t nowMs = millis();
  if (rtcWritePending && (!lastRtcWriteAttemptMs || nowMs - lastRtcWriteAttemptMs >= 60000UL)) {
    lastRtcWriteAttemptMs = nowMs;
    if (timeValid() && rtcClock.writeUtc(nowEpoch())) {
      rtcPresent = rtcTimeValid = true;
      lastRtcConfirmedEpoch = (uint32_t)nowEpoch();
      rtcWritePending = false;
      lastRtcReadMs = nowMs;
      DBGLN("RTC DS3231: время обновлено и прочитано обратно");
    } else {
      rtcPresent = rtcClock.present();
      rtcTimeValid = false;
      DBGLN("RTC DS3231: ошибка записи (повтор не ранее 60 секунд)");
    }
  }
  if (!rtcWritePending && (!lastRtcReadMs || nowMs - lastRtcReadMs >= RTC_CHECK_PERIOD_MS)) {
    time_t value = 0;
    rtcTimeValid = rtcClock.readUtc(value);
    rtcPresent = rtcClock.present();
    if (rtcTimeValid) lastRtcConfirmedEpoch = (uint32_t)value;
    lastRtcReadMs = nowMs;
    // Не корректируем часы автоматически по RTC при выполнении программы.
    // Бесшовный источник коррекции — SNTP, только при известном UTC.
  }
}

void onNtpTimeReceived(struct timeval *value) {
  // Обработчик SNTP вызывается из другой задачи. Работа с интерфейсом и NVS отложена до основного цикла.
  receivedNtpEpoch.store((uint32_t)value->tv_sec);
}

void pollTimeSync() {
  uint32_t received = receivedNtpEpoch.exchange(0);
  if (received) {
    lastNtpSyncEpoch = received;
    clockSource = 1;
    ntpRequested = false;
    oledMarkDirty();
    prefs.putULong("lastNtp", lastNtpSyncEpoch);
    rtcWritePending = true; // после подтверждённого NTP обновить автономные часы
    lastRtcWriteAttemptMs = 0;
  }
}

void syncTimeNTP(bool force = false) {
  if (WiFi.status() != WL_CONNECTED)
    return;
  uint32_t now = millis();
  if (!force && lastNtpAttemptMs && now - lastNtpAttemptMs < 60000UL)
    return;
  if (!force && lastNtpSyncEpoch && nowEpoch() >= lastNtpSyncEpoch &&
      nowEpoch() - lastNtpSyncEpoch < NTP_SYNC_PERIOD_SEC)
    return;
  lastNtpAttemptMs = now;
  ntpRequested = true;
  sntp_set_time_sync_notification_cb(onNtpTimeReceived);
  configTzTime(TZ_INFO.c_str(), NTP_SERVER_1.c_str(), NTP_SERVER_2.c_str());
  // При запуске можно кратко подождать первичную установку времени; последующие запросы асинхронны.
  uint32_t started = millis();
  while (!timeValid() && millis() - started < 5000UL) {
    delay(25);
    pollTimeSync();
    serviceRecovery();
  }
  pollTimeSync();
}

#include "calibration_y.h"
String overallCalibrationReason() {
  if (zCalStarted) return "Калибровка Z не завершена: сохраните результат или отмените";
  String reason = calibrationRunReason();
  if (reason.length()) return reason;
  if (!zCalibrationValid()) return "Требуется калибровка Z: Настройки → Калибровка Z";
  return "";
}
bool overallCalibrationReady() { return overallCalibrationReason().isEmpty(); }
// Целевая координата Z в GyverStepper имеет тип int32_t; программы с возможным
// переполнением после изменения STEP/360 отклоняются до начала движения.
bool programZTargetsSafe(const RemuageProgram &p) {
  const int32_t steps=currentZSteps360();
  for (uint16_t i=0;i<p.count;++i) {
    int64_t numerator=(int64_t)p.steps[i].rotationParts*steps;
    int64_t absolute=numerator<0?-numerator:numerator;
    if ((absolute+8)/16>1500000000LL) return false;
  }
  return true;
}



// ============================================================
//                   УГЛЫ И ПАУЗЫ
// ============================================================

int findNearestAngle(int angle) {
  if (angle <= ANGLE_STEPS[0][0])
    return ANGLE_STEPS[0][0];
  if (angle >= ANGLE_STEPS[ANGLE_COUNT - 1][0])
    return ANGLE_STEPS[ANGLE_COUNT - 1][0];

  for (uint8_t i = 0; i < ANGLE_COUNT - 1; i++) {
    if (angle >= ANGLE_STEPS[i][0] && angle <= ANGLE_STEPS[i + 1][0]) {
      int dl = angle - ANGLE_STEPS[i][0];
      int du = ANGLE_STEPS[i + 1][0] - angle;
      return (dl <= du) ? ANGLE_STEPS[i][0] : ANGLE_STEPS[i + 1][0];
    }
  }
  return ANGLE_STEPS[0][0];
}

int32_t angleToStepsNaklon(float angle) {
  if (calibrationValid()) return calibratedAngleToSteps(angle);
  if (angle <= ANGLE_STEPS[0][0])
    return ANGLE_STEPS[0][1];
  for (uint8_t i = 1; i < ANGLE_COUNT; i++)
    if (angle <= ANGLE_STEPS[i][0]) {
      const float f = (angle - ANGLE_STEPS[i - 1][0]) / (ANGLE_STEPS[i][0] - ANGLE_STEPS[i - 1][0]);
      return lroundf(ANGLE_STEPS[i - 1][1] + f * (ANGLE_STEPS[i][1] - ANGLE_STEPS[i - 1][1]));
    }
  return ANGLE_STEPS[ANGLE_COUNT - 1][1];
}

float stepsToAngleNaklon(int32_t steps) {
  if (calibrationValid()) return calibratedStepsToAngle(steps);
  if (steps <= ANGLE_STEPS[0][1])
    return ANGLE_STEPS[0][0];
  for (uint8_t i = 1; i < ANGLE_COUNT; ++i)
    if (steps <= ANGLE_STEPS[i][1]) {
      double fraction =
          (double)(steps - ANGLE_STEPS[i - 1][1]) / (ANGLE_STEPS[i][1] - ANGLE_STEPS[i - 1][1]);
      return ANGLE_STEPS[i - 1][0] + fraction * (ANGLE_STEPS[i][0] - ANGLE_STEPS[i - 1][0]);
    }
  return ANGLE_STEPS[ANGLE_COUNT - 1][0];
}

int32_t angleToStepsPovorot(float angle) {
  return (int32_t)llround((double)angle * currentZSteps360() / 360.0);
}

time_t scheduledTimeForStep(uint16_t index) {
  return programRunning && scheduleValid && index <= runProgram.count ? scheduledEpoch[index] : 0;
}

void rebuildRemainingSchedule() {
  if (!programRunning || currentProgramStep >= runProgram.count)
    return;
  const uint64_t remaining = stepInterval.remaining((uint64_t)esp_timer_get_time());
  scheduledEpoch[currentProgramStep + 1] = nowEpoch() + (time_t)((remaining + 999999) / 1000000);
  for (uint16_t i = currentProgramStep + 1; i < runProgram.count; i++)
    scheduledEpoch[i + 1] = scheduledEpoch[i] + runProgram.steps[i].pauseSeconds;
  scheduleValid = true;
}

void beginStepInterval() {
  const uint64_t now = (uint64_t)esp_timer_get_time();
  stepInterval.start(now, (uint64_t)runProgram.steps[currentProgramStep].pauseSeconds * 1000000ULL);
  scheduledEpoch[currentProgramStep] = nowEpoch();
  rebuildRemainingSchedule();
}

// ============================================================
//                  ЖУРНАЛ ВЫПОЛНЕНИЯ
// ============================================================

// ============================================================
//       LittleFS: состояние выполнения; NVS: настройки
// ============================================================
#include "run_history.h"

// Снимок состояния, фаза, сообщение о завершении и имя журнала фиксируются одной записью.
bool saveExecution() {
  execState.magic = EXEC_MAGIC;
  execState.programHash = executionHash;
  execState.currentStep = currentProgramStep;
  execState.state = (uint8_t)programState;
  execState.running = programRunning;
  execState.paused = recoveryRequired ? !recoveryAutoPending : programPaused;
  execState.motionUncertain = programState==MOVING_POVOROT && serialMovePovorotActive; // Записанный признак выдачи Z до первого STEP.
  execState.naklonPosition = motor_Naklon.getCurrent();
  execState.povorotPosition = motor_Povorot.getCurrent();
  if (timeValid())
    execState.savedEpoch = nowEpoch();
  uint64_t now = (uint64_t)esp_timer_get_time();
  execState.remainingUs = stepInterval.remaining(now);
  execState.settleRemainingUs = settleInterval.remaining(now);
  memcpy(execState.schedule, scheduledEpoch, sizeof(scheduledEpoch));
  execState.checksum = 0;
  execState.checksum = programHashBytes(&execState, sizeof(execState));
  std::unique_ptr<ExecutionRecord> record(new ExecutionRecord{});
  record->execution = execState;
  record->metadata = runMeta;
  if (programRunning)
    record->program = runProgram;
  if (executionJournal.save(*record)) {
    // Удаляются только старые записи после проверки и фиксации нового журнала.
    if (prefs.isKey("exec"))
      prefs.remove("exec");
    if (prefs.isKey("runProgram"))
      prefs.remove("runProgram");
    executionReadError = false;
    lastCheckpointUs = now;
    return true;
  }
  motor_Naklon.brake();
  motor_Povorot.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  motor_Povorot.setTarget(motor_Povorot.getCurrent());
  programTiltSamplingActive=false;
  disableDrivers();
  programPaused = true;
  recoveryAutoPending = false;
  recoveryRequired = true;
  recoveryMotionUncertain = true;
  stepInterval.pause((uint64_t)esp_timer_get_time());
  settleInterval.pause((uint64_t)esp_timer_get_time());
  executionMessage = "Не удалось сохранить состояние в LittleFS. Движение заблокировано; "
                     "остановите программу и проверьте свободное место и файловую систему.";
  DBGLN(executionMessage);
  return false;
}

bool loadExecution() {
  std::unique_ptr<ExecutionRecord> record(new ExecutionRecord{});
  bool fromJournal = executionJournal.load(*record, executionJournalDegraded);
  if (fromJournal) {
    execState = record->execution;
    runProgram = record->program;
    runMeta = record->metadata;
  } else {
    // Записи версий 11.0–11.2 имеют меньший размер. При переносе сохраняется номер последовательности.
    CheckedJournal<ExecutionFileStorage, LegacyExecutionRecord> legacy(executionStorage);
    std::unique_ptr<LegacyExecutionRecord> old(new LegacyExecutionRecord{});
    if (legacy.load(*old, executionJournalDegraded)) {
      execState = old->execution;
      runProgram = old->program;
      runMeta = {};
      executionJournal.continueAfter(legacy.lastSequence(), legacy.lastSlot());
      fromJournal = true;
    }
  }
  if (!fromJournal) {
    if (executionJournal.hasRecords()) {
      executionReadError = true;
      executionMessage = "Журнал LittleFS повреждён. Выполнение не восстановлено. Проверьте "
                         "положение осей перед новым запуском.";
      return false;
    }
    if (prefs.getBytesLength("exec") != sizeof(execState) ||
        prefs.getBytes("exec", &execState, sizeof(execState)) != sizeof(execState))
      return false;
    if (execState.running &&
        (prefs.getBytesLength("runProgram") != sizeof(runProgram) ||
         prefs.getBytes("runProgram", &runProgram, sizeof(runProgram)) != sizeof(runProgram)))
      return false;
  }
  const uint32_t checksum = execState.checksum;
  execState.checksum = 0;
  if ((execState.magic != EXEC_MAGIC && execState.magic != V112_EXEC_MAGIC &&
       execState.magic != LEGACY_EXEC_MAGIC) ||
      programHashBytes(&execState, sizeof(execState)) != checksum) {
    executionReadError = true;
    executionMessage = "Неверная контрольная сумма состояния. Проверьте положение и обнулите "
                       "координаты перед запуском.";
    return false;
  }
  execState.checksum = checksum;
  // В версиях 11.0/11.1 минуты хранились в той же структуре ProgramStep. Сначала проверяются исходные байты,
  // затем значения однократно преобразуются по сигнатуре версии, без эвристик.
  if (execState.magic == LEGACY_EXEC_MAGIC) {
    if (execState.running) {
      if (programHashBytes(&runProgram, sizeof(runProgram)) != execState.programHash ||
          runProgram.count == 0 || runProgram.count > MAX_PROGRAM_STEPS) {
        executionReadError = true;
        executionMessage = "Повреждён прежний снимок программы";
        return false;
      }
      for (uint16_t i = 0; i < runProgram.count; ++i) {
        if (runProgram.steps[i].pauseSeconds > MAX_PROGRAM_SECONDS / 60) {
          executionReadError = true;
          executionMessage = "Некорректная пауза прежнего снимка";
          return false;
        }
        runProgram.steps[i].pauseSeconds *= 60;
      }
      std::string error;
      if (!recalculateProgram(runProgram, error)) {
        executionReadError = true;
        executionMessage = error.c_str();
        return false;
      }
      execState.programHash = programHashBytes(&runProgram, sizeof(runProgram));
    }
    execState.lastCompletedStep =
        execState.running ? execState.currentStep +
                                (execState.naklonCompleted && execState.povorotCompleted ? 1 : 0)
                          : 0;
    execState.lastCompletedEpoch = 0; // В версии 11.1 время завершения действий не записывалось.
    execState.magic = EXEC_MAGIC;
  }
  if (!execState.running) {
    motor_Naklon.setCurrent(execState.naklonPosition);
    motor_Naklon.setTarget(execState.naklonPosition);
    motor_Povorot.setCurrent(execState.povorotPosition);
    motor_Povorot.setTarget(execState.povorotPosition);
    return false;
  }
  if (programHashBytes(&runProgram, sizeof(runProgram)) != execState.programHash) {
    executionReadError = true;
    executionMessage = "Повреждён снимок выполнения";
    return false;
  }
  std::string error;
  if (!recalculateProgram(runProgram, error) || execState.currentStep >= runProgram.count ||
      execState.state < MOVING_NAKLON || execState.state > WAITING_BETWEEN_STEPS) {
    executionReadError = true;
    executionMessage =
        "Некорректное состояние выполнения. Проверьте положение и обнулите координаты.";
    return false;
  }
  if (!runMeta.startedEpoch)
    runMeta.startedEpoch = execState.schedule[0];
  if (!runMeta.name[0])
    copyProgramNameUtf8(runMeta.name, sizeof(runMeta.name), runProgram.name);
  executionHash = execState.programHash;
  currentProgramStep = execState.currentStep;
  programState = (ProgramState)execState.state;
  programRunning = true;
  programPaused = true;
  motor_Naklon.setCurrent(execState.naklonPosition);
  motor_Povorot.setCurrent(execState.povorotPosition);
  motor_Naklon.setTarget(execState.naklonPosition);
  motor_Naklon.brake();
  motor_Povorot.setTarget(execState.povorotPosition);
  motor_Povorot.brake();
  memcpy(scheduledEpoch, execState.schedule, sizeof(scheduledEpoch));
  scheduleValid = true;
  return true;
}

void resetExecution() {
  xProtectionTripped=false;
  sensorFallbackY=false;bootSensorWaitMs=0;
  memset(&execState, 0, sizeof(execState));
  programRunning = false;
  programPaused = false;
  pauseStopPending = false;
  recoveryAutoPending = false;
  recoveryRequired = false;
  recoveryMotionUncertain = false;
  currentProgramStep = 0;
  programState = IDLE;
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  stepInterval = ProgramInterval{};
  settleInterval = ProgramInterval{};
  scheduleValid = false;
  saveExecution();
}

// В прошивке только ОДИН исходный шаблон «7 дней».
void createDefaultProgram(RemuageProgram &target, const String &name) {
  static const char csv[] = "Step;incAngle;Angle;IncRot;Rot;Pause\n"
                            "1;0;;2;;6\n2;7;;-6;;3\n3;5;;4;;3\n4;5;;-4;;3\n"
                            "5;5;;4;;3\n6;5;;-4;;3\n7;5;;4;;3\n8;5;;-6;;3\n"
                            "9;5;;-4;;2\n10;5;;-4;;2\n11;5;;-4;;2\n12;5;;-4;;2\n"
                            "13;5;;-4;;2\n14;0;;-8;;2\n15;0;;8;;2\n";
  std::string error;
  parseProgramCsv(csv, target, error);
  copyProgramNameUtf8(target.name, sizeof(target.name), name);
}

void restoreTmcFile() {
  TmcFileValues v;
  if(tmcSettingsFile.load(v)) {
    uartNaklon=v.y;uartPovorot=v.z;NAKLON_ACCELERATION=v.yAcceleration;NAKLON_MAX_SPEED=v.ySpeed;
    POVOROT_ACCELERATION=v.zAcceleration;POVOROT_MAX_SPEED=v.zSpeed;
    DBGLN("UART Y/Z: настройки восстановлены из проверенного файла");
  } else {
    // Однократная миграция прежних настроек NVS; повреждённый файл не применяется к драйверам.
    const bool saved=persistTmcFile(uartNaklon,uartPovorot,NAKLON_ACCELERATION,NAKLON_MAX_SPEED,POVOROT_ACCELERATION,POVOROT_MAX_SPEED);
    DBGLN(saved ? "UART Y/Z: создан файл из настроек NVS" : "UART Y/Z: файл недоступен, использованы настройки NVS");
  }
}

void initializePrograms() {
  // Старые ключи p0...p3 оставляем нетронутыми для ручной миграции,
  // но НЕ используем как источник: CSV, отредактированный менеджером,
  // больше не может быть затёрт состоянием NVS.
  serialOutputEnabled = prefs.getBool("serialOn", true);
  WEB_REQUEST_PERIOD_MS = prefs.getUInt("webPeriod", WEB_REQUEST_PERIOD_MS);
  WEB_PAGE_REFRESH_PERIOD_MS = prefs.getUInt("webRefresh", WEB_PAGE_REFRESH_PERIOD_MS);
  PAUSE_AFTER_NAKLON_MS = prefs.getUInt("pauseNak", PAUSE_AFTER_NAKLON_MS);
  NAKLON_ACCELERATION = prefs.getUInt("nakAcc", NAKLON_ACCELERATION);
  NAKLON_MAX_SPEED = prefs.getUInt("nakSpd", NAKLON_MAX_SPEED);
  POVOROT_ACCELERATION = prefs.getUInt("povAcc", POVOROT_ACCELERATION);
  POVOROT_MAX_SPEED = prefs.getUInt("povSpd", POVOROT_MAX_SPEED);
  MAX_ACTION_TIME_SEC = prefs.getUInt("maxAct", MAX_ACTION_TIME_SEC);

  NTP_SYNC_PERIOD_SEC = constrain(prefs.getUInt("ntpPeriod", NTP_SYNC_PERIOD_SEC), 60UL, 604800UL);
  NTP_SERVER_1 = prefs.getString("ntpServer1", NTP_SERVER_1);
  NTP_SERVER_2 = prefs.getString("ntpServer2", NTP_SERVER_2);
  if (!NTP_SERVER_1.length()) NTP_SERVER_1 = "pool.ntp.org";
  if (!NTP_SERVER_2.length()) NTP_SERVER_2 = "time.nist.gov";
  lastNtpSyncEpoch = prefs.getULong("lastNtp", 0);
  String tz = prefs.getString("tz", TZ_INFO);
  if (tz.length())
    TZ_INFO = tz;
  applyTimezone();
  loadTmcSettings(uartNaklon, "tn");
  loadTmcSettings(uartPovorot, "tp");
  // При отсутствии настроек: Y=TMC2208@0, Z=TMC2209@1.
  // Модели и выбранные адреса сохраняются независимо от скорости шины.
  normalizeTmcBusSettings(prefs.getUInt("tmcBusBaud", uartNaklon.baud));
  restoreTmcFile();
}

// ============================================================
//                  TMC2208 / TMC2209 / ОБЩАЯ UART
// ============================================================

void normalizeTmcBusSettings(uint32_t baud) {
  if (baud < 9600 || baud > 500000)
    baud = TMC_DEFAULT_BAUD;
  uartNaklon.baud = uartPovorot.baud = baud;
  uartNaklon.pdnDisable = uartPovorot.pdnDisable = true;
  // На общей шине оба узла должны выдерживать задержку ответа >= 2.
  uartNaklon.sendDelay = constrain(uartNaklon.sendDelay, 2, 15);
  uartPovorot.sendDelay = constrain(uartPovorot.sendDelay, 2, 15);
}

void reportTmcConnection(TmcAxis &axis, const char *name, const TmcUartSettings &settings) {
  axis.checkConnection(settings.model);
  DBGLN(String("TMC ") + name + " model=" + String((uint16_t)settings.model) +
        " addr=" + String(settings.address) +
        (axis.connected ? ": UART OK"
                        : ": UART ERROR; check model, MS1/MS2, PDN_UART, VIO and VMOT"));
}

// Возвращает true только если драйвер ответил правильной моделью (IOIN.VERSION)
// и счётчик IFCNT подтвердил ВСЕ UART-записи настройки (по модулю 256).
// Измерять ТОЛЬКО в состоянии покоя: чтения UART блокируют поток STEP.
bool applyTmcSettingsVerified(TmcAxis &axis, const TmcUartSettings &c) {
  axis.settingsApplied = false;
  axis.writesSent = axis.writesAccepted = 0;
  axis.checkConnection(c.model);
  if (!axis.connected) return false;
  TMC2208Stepper &drv = axis.driver();
  const uint8_t before = drv.IFCNT();
  if (drv.CRCerror) { axis.connected = false; return false; }
  const uint16_t bytesBefore = drv.bytesWritten;
  applyTmcSettings(drv, c);
  const uint16_t bytesAfter = drv.bytesWritten;
  const uint8_t after = drv.IFCNT();
  axis.ifcntBefore = before;
  axis.ifcntAfter = after;
  const uint16_t sent = uint16_t(bytesAfter - bytesBefore) / 8U;
  axis.writesSent = sent <= 255 ? uint8_t(sent) : 0;
  axis.writesAccepted = uint8_t(after - before); // аппаратный счётчик циклический
  axis.settingsApplied = !drv.CRCerror && sent > 0 && sent < 256 &&
                         uint16_t(bytesAfter - bytesBefore) == sent * 8 &&
                         axis.writesAccepted == sent;
  if (!axis.settingsApplied) {
    DBGLN(String("TMC CONFIG NOT VERIFIED model=") + String(uint16_t(c.model)) +
          " addr=" + String(c.address) + " IFCNT accepted=" + String(axis.writesAccepted) +
          " / sent=" + String(axis.writesSent));
  }
  return axis.settingsApplied;
}

void configureTmcDrivers() {
  tmcNaklon.configure(&TmcSerial, uartNaklon);
  tmcPovorot.configure(&TmcSerial, uartPovorot);
  const bool yOK = applyTmcSettingsVerified(tmcNaklon, uartNaklon);
  const bool zOK = applyTmcSettingsVerified(tmcPovorot, uartPovorot);
  if (!yOK || !zOK)
    DBGLN("WARNING: TMC UART not confirmed. Do not start motion until wiring/address is checked.");
  reportTmcConnection(tmcNaklon, "Y/NAKLON", uartNaklon);
  reportTmcConnection(tmcPovorot, "Z/POVOROT", uartPovorot);
}

void initTMC() {
  normalizeTmcBusSettings(uartNaklon.baud);
  if (tmcBusError(uartNaklon, uartPovorot)) {
    // Повреждённые/устаревшие настройки: известная установленная пара.
    uartNaklon.model = TmcModel::Tmc2208;
    uartNaklon.address = 0;
    uartPovorot.model = TmcModel::Tmc2209;
    uartPovorot.address = 1;
    DBGLN("Invalid saved UART topology; using Y=TMC2208@0, Z=TMC2209@1");
  }
  uartNaklon.rmsCurrent = std::min(uartNaklon.rmsCurrent, tmcMaxRms(uartNaklon.model));
  uartPovorot.rmsCurrent = std::min(uartPovorot.rmsCurrent, tmcMaxRms(uartPovorot.model));
  // Arduino begin() принимает пины в порядке RX, TX.
  TmcSerial.begin(uartNaklon.baud, SERIAL_8N1, TMC_UART_RX, TMC_UART_TX);
  delay(20);
  configureTmcDrivers();
}

// ============================================================
//                     ДВИГАТЕЛИ / EN
// ============================================================

void setupMotors() {
  motor_Naklon.setRunMode(FOLLOW_POS);
  motor_Naklon.setAcceleration(NAKLON_ACCELERATION);
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  motor_Naklon.setTarget(motor_Naklon.getCurrent());

  motor_Povorot.reverse(true);
  motor_Povorot.setRunMode(FOLLOW_POS);
  motor_Povorot.setAcceleration(POVOROT_ACCELERATION);
  motor_Povorot.setMaxSpeed(POVOROT_MAX_SPEED);
  motor_Povorot.setTarget(motor_Povorot.getCurrent());

  disableDrivers(); // Безопасное состояние при старте.
}

// ============================================================
//                         КОНЦЕВИКИ
// ============================================================

// Блокировка только движения В СТОРОНУ разомкнутого концевика.
// Читаем также сырые входы: команда HTTP/Serial не обходит защиту между опросами.
bool canMoveY(int direction) {
  const bool lower = endstopYActive || digitalRead(Y_END_PIN) == HIGH;
  const bool upper = endstopUpperActive || digitalRead(Y_UPPER_END_PIN) == HIGH;
  if (lower && upper)
    return false;
  return (direction >= 0 || !lower) && (direction <= 0 || !upper);
}

void checkEndstops() {
  const uint64_t now = (uint64_t)esp_timer_get_time();
  const bool y = lowerEndstop.update(digitalRead(Y_END_PIN) == HIGH, now);
  const bool upper = upperEndstop.update(digitalRead(Y_UPPER_END_PIN) == HIGH, now);
  const bool movingDown = motor_Naklon.getTarget() < motor_Naklon.getCurrent();
  const bool movingUp = motor_Naklon.getTarget() > motor_Naklon.getCurrent();
  const bool wasLower = endstopYActive, wasUpper = endstopUpperActive;
  endstopYActive = y;
  endstopUpperActive = upper;
  const bool wasFault = yEndstopFault;
  yEndstopFault = y && upper;
  if (yEndstopFault) {
    if(homingZPhase) {
      motor_Povorot.brake();motor_Povorot.setTarget(motor_Povorot.getCurrent());
      motorPovorotMoving=false;homingZPhase=false;moveDirectionPovorot=0;
      disableDrivers();
    }
    if (calibrationBusy()) calStopWithError("Авария: два концевика Y разомкнуты");
    // Два противоположных концевика одновременно: не назначаем фиктивный ноль.
    motor_Naklon.brake();
    motor_Naklon.setTarget(motor_Naklon.getCurrent());
    motorNaklonMoving = serialMoveNaklonActive = homingActive = false;
    moveDirectionNaklon = 0;
    if (!wasFault) {
      executionMessage = "Оба концевика Y разомкнуты: проверьте GPIO14/GPIO13 и проводку";
      recoveryAutoPending = false;
      if (programRunning && !programPaused)
        pauseProgram();
      if (programRunning)
        appendRunLog("ERROR", executionMessage);
      if (!motor_Povorot.getState())
        disableDrivers();
      oledMarkDirty();
    }
    return;
  }
  if (movingDown && !upper)
    runMeta.upperReached = false;
  if (y && (((!wasLower || wasFault) && !movingUp) || movingDown || (homingActive && !homingZPhase))) {
    const int32_t beforeLowerReset = motor_Naklon.getCurrent();
    calibrationOnLower(beforeLowerReset);
    motor_Naklon.brake();
    motor_Naklon.setCurrent(0);
    motor_Naklon.setTarget(0);
    motorNaklonMoving = false;
    moveDirectionNaklon = 0;
    runMeta.upperReached = false;
    if (homingActive && !homingZPhase) {
      serialMoveNaklonActive = false;
      beginZHome();
    }
    if (!programRunning)
      serialMoveNaklonActive = false;
    if (!motor_Povorot.getState()) {
      if(!recoveryMotionUncertain && !executionReadError) releaseDrivers();
      if (!programRunning && !calibrationBusy())
        saveExecution();
    }
    oledMarkDirty();
  }
  if (upper && (movingUp || ((!wasUpper || wasFault) && !movingDown))) {
    motor_Naklon.brake();
    calibrationOnUpper();
    motor_Naklon.setCurrent(calibratedUpperSteps());
    motor_Naklon.setTarget(calibratedUpperSteps());
    runMeta.upperReached = true;
    motorNaklonMoving = false;
    moveDirectionNaklon = 0;
    if (!programRunning)
      serialMoveNaklonActive = false;
    if (!motor_Povorot.getState()) {
      releaseDrivers();
      if (!programRunning && !calibrationBusy())
        saveExecution();
    }
    if (programRunning)
      appendRunLog("UPPER_LIMIT", "Наклон остановлен верхним концевиком GPIO13, 85°");
    oledMarkDirty();
  }
}

bool beginZHome() {
  homingZPhase=true;
  serialMoveNaklonActive=false;
  if(!zCalibrationValid() || recoveryMotionUncertain || executionReadError) {
    homingActive=homingZPhase=false;
    disableDrivers();
    executionMessage="Y в нуле. Возврат Z заблокирован: проверьте калибровку и исходное положение Z";
    saveExecution();return false;
  }
  const int64_t position=motor_Povorot.getCurrent(), turn=currentZSteps360();
  int64_t delta=-(position % turn);
  if(delta > turn/2) delta-=turn;
  if(delta < -turn/2) delta+=turn;
  int64_t target=position+delta;
  if(target>INT32_MAX) target-=turn;
  if(target<INT32_MIN) target+=turn;
  if(!saveExecution()) {homingActive=homingZPhase=false;return false;}
  if(target==position) {
    motor_Povorot.setCurrent(0);motor_Povorot.setTarget(0);
    homingActive=homingZPhase=false;
    executionMessage="В начале: Y — 23°, Z — запомненный ноль";
    releaseDrivers();return saveExecution();
  }
  homingActive=true;homingStartedUs=(uint64_t)esp_timer_get_time();
  motor_Povorot.setAcceleration(POVOROT_ACCELERATION);
  motor_Povorot.setMaxSpeed(POVOROT_MAX_SPEED);
  motorPovorotMoving=true;serialMovePovorotActive=false;
  moveDirectionPovorot=target>position?1:-1;
  motor_Povorot.setTarget((int32_t)target);enableDrivers();
  executionMessage="Y в нуле: возврат Z к запомненному нулю";
  return true;
}

bool beginHoming() {
  if (zCalStarted || programRunning || calibrationBusy() || motor_Naklon.getState() || motor_Povorot.getState()) {
    executionMessage = "Сначала остановите программу и ручное движение";
    return false;
  }
  if (digitalRead(Y_END_PIN) == HIGH && digitalRead(Y_UPPER_END_PIN) == HIGH) {
    executionMessage = "Оба концевика Y разомкнуты: возврат невозможен";
    return false;
  }
  if (digitalRead(Y_END_PIN) == HIGH) {
    motor_Naklon.setCurrent(0);
    motor_Naklon.setTarget(0);
    runMeta.upperReached = false;
    executionMessage = "В начале: концевик Y уже нажат, 23°";
    return beginZHome();
  }
  if (!canMoveY(-1)) {
    executionMessage = "Дождитесь замыкания нижнего концевика Y";
    return false;
  }
  if (!saveExecution())
    return false;
  homingZPhase = false;
  homingActive = true;
  homingStartedUs = (uint64_t)esp_timer_get_time();
  serialMoveNaklonActive = true;
  moveDirectionNaklon = -1;
  executionMessage = "Возврат к 23°: максимальная заданная скорость, до NC-концевика";
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  motor_Naklon.setTarget(motor_Naklon.getCurrent() - Y_ENDSTOP_SEARCH_STEPS);
  enableDrivers();
  return true;
}

void serviceHoming() {
  if (!homingActive)
    return;
  if(homingZPhase) {
    const bool finished=motor_Povorot.getCurrent()==motor_Povorot.getTarget();
    const bool timeout=(uint64_t)esp_timer_get_time()-homingStartedUs >
      (uint64_t)std::min<uint32_t>(MAX_ACTION_TIME_SEC,Y_LIMIT_SEARCH_TIMEOUT_SEC)*1000000ULL;
    if(!finished && !timeout) return;
    motor_Povorot.brake();motor_Povorot.setTarget(motor_Povorot.getCurrent());
    motorPovorotMoving=false;moveDirectionPovorot=0;
    homingActive=homingZPhase=false;
    if(timeout && !finished) {
      disableDrivers();executionMessage="Таймаут возврата Z: проверьте механику и исходное положение";
    } else {
      motor_Povorot.setCurrent(0);motor_Povorot.setTarget(0);releaseDrivers();
      executionMessage="В начале: Y — 23°, Z — запомненный ноль";
    }
    saveExecution();oledMarkDirty();return;
  }
  // Поиск не ограничен 108410 шагами: остановка — NC-концевик или время.
  const uint64_t elapsed = (uint64_t)esp_timer_get_time() - homingStartedUs;
  const bool timeout = elapsed > (uint64_t)std::min<uint32_t>(MAX_ACTION_TIME_SEC, Y_LIMIT_SEARCH_TIMEOUT_SEC) * 1000000ULL;
  const bool finished = motor_Naklon.getCurrent() == motor_Naklon.getTarget();
  if (!finished && !timeout) return;
  motor_Naklon.brake();
  homingActive = false;
  serialMoveNaklonActive = false;
  moveDirectionNaklon = 0;
  executionMessage = "Авария: концевик 23° не найден до тайм-аута; проверьте механику";
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  disableDrivers();
  saveExecution();
  oledMarkDirty();
}

// ============================================================
//                    СТАРТ / ОСТАНОВКА
// ============================================================

void startProgram(uint16_t requestedStep) {
  if (!overallCalibrationReady()) { executionMessage=overallCalibrationReason(); return; }
  if (digitalRead(Y_END_PIN) == HIGH && digitalRead(Y_UPPER_END_PIN) == HIGH) {
    executionMessage = "Оба концевика Y разомкнуты: проверьте проводку";
    return;
  }
  if (programRunning)
    return;
  if (executionReadError) {
    executionMessage = "Сначала проверьте исходное положение и обнулите координаты: журнал "
                       "восстановления повреждён.";
    return;
  }
  executionMessage = "";
  if (!timeValid()) {
    executionMessage = "Время не установлено. Откройте главную страницу и запустите из браузера "
                       "либо синхронизируйте часы на странице Сеть.";
    return;
  }
  if (motorNaklonMoving || motorPovorotMoving || motor_Naklon.getState() ||
      motor_Povorot.getState()) {
    executionMessage = "Сначала остановите ручное движение";
    return;
  }
  calSample(true);
  if(!calXWithinTolerance()) {executionMessage="X вне допуска выравнивания или датчик недоступен: проверьте настройки X";return;}
  xProtectionTripped=false;
  if(yBootReferencePending) {synchronizeYAfterPower();yBootReferencePending=false;}
  refreshPrograms(true);
  if (programs.empty() || selectedProgram >= programs.size()) {
    executionMessage = "Нет корректной программы";
    return;
  }
  runProgram = (draftLoaded && draftDirty && draftPath == selectedProgramPath)
                   ? draftProgram
                   : programs[selectedProgram];
  std::string error;
  if (!recalculateProgram(runProgram, error)) {
    executionMessage = error.c_str();
    return;
  }
  if (!programZTargetsSafe(runProgram)) {
    executionMessage = "Слишком большое число шагов Z: переполнение координат. Уменьшите IncRot или перекалибруйте Z";
    return;
  }
  if (requestedStep < 1 || requestedStep > runProgram.count) {
    executionMessage = "Номер стартового шага за пределами программы";
    return;
  }
  runMeta = {};
  runMeta.startedEpoch = nowEpoch();
  copyProgramNameUtf8(runMeta.name, sizeof(runMeta.name), runProgram.name);
  if (!ensureRunLog()) {
    executionMessage = logError;
    return;
  }
  executionHash = programHashBytes(&runProgram, sizeof(runProgram));
  // Если стартуем с шага N, принимаем текущее физическое положение Z
  // за положение ПОСЛЕ шага N-1. Это исключает прокрутку пропущенной истории.
  const int32_t zBaseline = requestedStep > 1
      ? programRotationSteps(runProgram.steps[requestedStep - 2].rotationParts, currentZSteps360())
      : 0;
  motor_Povorot.setCurrent(zBaseline);
  motor_Povorot.setTarget(zBaseline);
  memset(&execState, 0, sizeof(execState));
  memset(scheduledEpoch, 0, sizeof(scheduledEpoch));
  programRunning = true;
  programPaused = false;
  pauseStopPending = false;
  recoveryAutoPending = false;
  recoveryRequired = false;
  recoveryMotionUncertain = false;
  currentProgramStep = requestedStep - 1;
  programState = MOVING_NAKLON;
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  beginStepInterval();
  if (!saveExecution())
    return;
  appendRunLog("START", String("Программа: ") + runProgram.name +
                          ", с шага " + String(requestedStep));
  if (programExceedsTiltRange(runProgram))
    appendRunLog("WARNING", tiltRangeWarning(runProgram));
  // Первое движение начинается сразу; его пауза задаёт интервал до второго шага.
  beginNaklonAction();
  if (!programRunning || programPaused)
    return;
  DBGLN(String("ПРОГРАММА: ") + runProgram.name);
  oledMarkDirty();
}

void stopProgram() {
  sensorFallbackY=false;
  programTiltSamplingActive=false;
  if (programRunning && !runMeta.finishedEpoch)
    appendRunLog("STOP", "Программа остановлена");
  recoveryAutoPending = false;
  homingActive = homingZPhase = false;
  motor_Naklon.brake();
  motor_Povorot.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  motor_Povorot.setTarget(motor_Povorot.getCurrent());
  motorNaklonMoving = false;
  motorPovorotMoving = false;
  releaseDrivers();
  resetExecution();
  programsDirty = true;
  DBGLN("ПРОГРАММА ОСТАНОВЛЕНА");
  oledMarkDirty();
}

// ============================================================
//                     ВЫПОЛНЕНИЕ ПРОГРАММЫ
// ============================================================

void serviceXProtection() {
  if(!programRunning || xProtectionTripped || !calXStored || !calXFresh() || calXWithinRunTolerance()) return;
  xProtectionTripped=true;
  recoveryAutoPending=false;
  const uint64_t now=(uint64_t)esp_timer_get_time();
  stepInterval.pause(now);settleInterval.pause(now);
  programPaused=true;pauseStopPending=false;homingActive=homingZPhase=false;
  motor_Naklon.brake();motor_Povorot.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());motor_Povorot.setTarget(motor_Povorot.getCurrent());
  motorNaklonMoving=motorPovorotMoving=false;
  // Признак выданной команды Z сохраняется для прежнего правила восстановления после питания.
  programTiltSamplingActive=false;
  disableDrivers();
  executionMessage=String("Защита X: отклонение ")+String(fabsf(calCurrentX-calRefX),1)+
    "° превышает ±"+String(calRunToleranceX,1)+"°. Устраните причину и нажмите Продолжить";
  if(saveExecution()) appendRunLog("X_PROTECTION",executionMessage);
  oledMarkDirty();
}
bool freshMeasuredY() {
  return calAdxlPresent && calLastOkMs && (uint32_t)(millis()-calLastOkMs)<1200 && isfinite(calAngle);
}
bool synchronizeYAfterPower() {
  if(!yBootReferencePending || !calibrationValid() || calibrationBusy() ||
     motor_Naklon.getState() || motor_Povorot.getState()) return false;
  calSample(true);
  if(!freshMeasuredY()) return false;
  if(digitalRead(Y_END_PIN)==HIGH && digitalRead(Y_UPPER_END_PIN)==HIGH) return false;
  const int32_t position=digitalRead(Y_END_PIN)==HIGH ? 0 :
    digitalRead(Y_UPPER_END_PIN)==HIGH ? calibratedUpperSteps() : calibratedAngleToSteps(calAngle);
  motor_Naklon.setCurrent(position);
  motor_Naklon.setTarget(position);
  yBootReferencePending=false;
  return true;
}
void beginNaklonAction() {
  // Обычное выполнение использует только координату STEP и сохранённую карту.
  // ADXL345 не корректирует счётчик перед каждой строкой программы.
  const ProgramStep &s = runProgram.steps[currentProgramStep];
  int32_t target = angleToStepsNaklon(s.angleNaklon);
  // Логический угол 0° означает ФИЗИЧЕСКИЙ возврат к нижнему NC-концевику, а не просто 0 STEP.
  if(s.angleNaklon<=0.01f) {
    target = (endstopYActive || digitalRead(Y_END_PIN)==HIGH) ? 0 :
             motor_Naklon.getCurrent()-Y_ENDSTOP_SEARCH_STEPS;
    motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  }
  if (endstopUpperActive && !endstopYActive)
    runMeta.upperReached = true;
  if (s.angleNaklon > 85 && !runMeta.upperReached)
    target = motor_Naklon.getCurrent() + calibratedUpperSteps();
  if (s.angleNaklon > 85 && runMeta.upperReached)
    target = calibratedUpperSteps();
  const int direction = (target > motor_Naklon.getCurrent()) - (target < motor_Naklon.getCurrent());
  if (!canMoveY(direction)) {
    executionMessage = "Наклон заблокирован концевиком";
    stopProgram();
    return;
  }
  // Сохраняем состояние до включения оси; восстановление использует сохранённые отметки.
  if (!saveExecution())
    return;
  programTiltSamplingActive=true;
  calMoveFromRest(target);
  serialMoveNaklonActive = true;
  actionStartedUs = (uint64_t)esp_timer_get_time();
  enableDrivers();
}

void beginPovorotAction() {
  programTiltSamplingActive=true; // Свежие измерения Y продолжаются во время вращения Z.
  serialMovePovorotActive=true; // Признак выдачи сохраняется до включения двигателя Z.
  int32_t target =
      programRotationSteps(runProgram.steps[currentProgramStep].rotationParts, currentZSteps360());
  if (!saveExecution())
    return;
  if(!appendRunLog("ROTATION_ISSUED","Команда Z записана; при сбое повтор не выполняется")) {executionMessage=logError;pauseProgram();return;}
  motor_Povorot.setTarget(target);
  serialMovePovorotActive = true;
  actionStartedUs = (uint64_t)esp_timer_get_time();
  enableDrivers();
}

// Пауза сохраняет текущий шаг программы и координаты по счётчикам двигателей.
// Энкодеров нет: это координаты по командам, а не измеренные положения.
bool pauseProgram() {
  if (recoveryAutoPending && programRunning) {
    recoveryAutoPending = false;
    executionMessage = "Автоматическое продолжение отменено кнопкой Пауза";
    saveExecution();
    return true;
  }
  if (!programRunning || programPaused)
    return false;
  uint64_t now = (uint64_t)esp_timer_get_time();
  stepInterval.pause(now);
  settleInterval.pause(now);
  programPaused = true;
  programTiltSamplingActive=false;
  pauseStopPending = true;
  motor_Naklon.stop();
  motor_Povorot.stop();
  if (!motor_Naklon.getState() && !motor_Povorot.getState()) {
    pauseStopPending = false;
    releaseDrivers();
  }
  saveExecution();
  oledMarkDirty();
  return true;
}

bool resumeProgram() {
  if (!overallCalibrationReady()) {executionMessage=overallCalibrationReason();return false;}
  if (!programZTargetsSafe(runProgram)) {executionMessage="Координаты Z выходят за допустимый диапазон";return false;}
  if (yEndstopFault)
    return false;
  if (!programRunning || !programPaused || pauseStopPending || recoveryMotionUncertain)
    return false;
  if (motor_Naklon.getState() || motor_Povorot.getState())
    return false;
  if(xProtectionTripped && !calXWithinRunTolerance()) {executionMessage="Продолжение заблокировано: нужны свежие показания X в рабочем допуске";return false;}
  xProtectionTripped=false;
  if (recoveryRequired)
    return resumeRecoveredProgram();
  uint64_t now = (uint64_t)esp_timer_get_time();
  stepInterval.resume(now);
  settleInterval.resume(now);
  programPaused = false;
  recoveryRequired = false;
  executionMessage = "";
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  rebuildRemainingSchedule();
  if (!saveExecution())
    return false;
  oledMarkDirty();
  return true;
}

void executeProgram() {
  if (!programRunning)
    return;
  serviceXProtection();
  if (programPaused) {
    if (pauseStopPending && !motor_Naklon.getState() && !motor_Povorot.getState()) {
      pauseStopPending = false;
      releaseDrivers();
      saveExecution();
    }
    return;
  }
  if (currentProgramStep >= runProgram.count) {
    stopProgram();
    return;
  }
  uint64_t now = (uint64_t)esp_timer_get_time();
  if ((serialMoveNaklonActive || serialMovePovorotActive) &&
      now - actionStartedUs > (uint64_t)(programState == MOVING_NAKLON &&
                                         runProgram.steps[currentProgramStep].angleNaklon <= 0.01f
                                      ? std::min<uint32_t>(MAX_ACTION_TIME_SEC, Y_LIMIT_SEARCH_TIMEOUT_SEC)
                                      : MAX_ACTION_TIME_SEC) * 1000000ULL) {
    executionMessage = "Превышено максимальное время движения";
    stopProgram();
    return;
  }
  switch (programState) {
  case MOVING_NAKLON:
    if (!serialMoveNaklonActive)
      beginNaklonAction();
    if (!programRunning || programPaused)
      return;
    if (motor_Naklon.getCurrent() == motor_Naklon.getTarget()) {
      programTiltSamplingActive=false;
      if (runProgram.steps[currentProgramStep].angleNaklon > 85 && !runMeta.upperReached) {
        executionMessage = "Верхний концевик Y не найден за 108410 шагов; программа остановлена";
        appendRunLog("ERROR", executionMessage);
        stopProgram();
        return;
      }
      serialMoveNaklonActive = false;
      motorNaklonMoving = false;
      releaseDrivers();
      execState.naklonCompleted = true;
      programState = execState.povorotCompleted ? WAITING_BETWEEN_STEPS : WAITING_AFTER_NAKLON;
      if (execState.povorotCompleted) {
        execState.lastCompletedStep = currentProgramStep + 1;
        execState.lastCompletedEpoch = nowEpoch();
      }
      settleInterval.start(now, (uint64_t)PAUSE_AFTER_NAKLON_MS * 1000ULL);
      if (saveExecution())
        appendRunLog("TILT_DONE_BY_STEPS", String("Y выполнен по карте шагов: ")+
          String(calibratedStepsToAngle(motor_Naklon.getCurrent()),2)+" deg; ADXL345="+
          (freshMeasuredY()?String(calAngle,0)+" deg":String("нет данных")));
      oledMarkDirty();
    }
    break;
  case WAITING_AFTER_NAKLON:
    if (settleInterval.due(now)) {
      programState = MOVING_POVOROT;
      saveExecution();
    }
    break;
  case MOVING_POVOROT:
    if (!serialMovePovorotActive)
      beginPovorotAction();
    if (!programRunning || programPaused)
      return;
    // Логическая отметка завершения ставится через шесть секунд после команды, независимо от STEP.
    if(now-actionStartedUs>=6000000ULL) execState.povorotCompleted=true;
    if (motor_Povorot.getCurrent() == motor_Povorot.getTarget() && execState.povorotCompleted) {
      programTiltSamplingActive=false;
      serialMovePovorotActive = false;
      motorPovorotMoving = false;
      releaseDrivers();
      // Накопленная координата сохраняется: продолжение не должно повторно прибавлять IncRot.
      execState.povorotCompleted = true;
      execState.lastCompletedStep = currentProgramStep + 1;
      execState.lastCompletedEpoch = nowEpoch();
      programState = WAITING_BETWEEN_STEPS;
      if (saveExecution())
        appendRunLog("ROTATION_DONE", "Команда Z принята выполненной через 6 секунд; выдержка шага продолжается");
      oledMarkDirty();
    }
    break;
  case WAITING_BETWEEN_STEPS:
    if (stepInterval.due(now)) {
      if (currentProgramStep + 1 >= runProgram.count) {
        finishProgram();
      } else {
        currentProgramStep++;
        programState = MOVING_NAKLON;
        execState.naklonCompleted = false;
        execState.povorotCompleted = false;
        // Расписание привязывается к ФАКТИЧЕСКОМУ началу. Просроченные шаги не выполняются подряд без выдержки.
        beginStepInterval();
        if (saveExecution())
          appendRunLog("STEP_START", "Начало следующего шага");
        oledMarkDirty();
      }
    }
    break;
  case IDLE:
    break;
  }
}

// ============================================================
//                         КНОПКИ
// ============================================================

void resetBothPositions() {
  if (programRunning)
    stopProgram();

  motor_Naklon.stop();
  motor_Povorot.stop();

  motor_Naklon.reset();
  motor_Povorot.reset();

  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  motor_Povorot.setTarget(motor_Povorot.getCurrent());

  motorNaklonMoving = false;
  motorPovorotMoving = false;
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  moveDirectionNaklon = 0;
  moveDirectionPovorot = 0;

  disableDrivers();
  resetExecution();
  DBGLN("Позиции обоих моторов сброшены в 0");
  oledMarkDirty();
}

void handleButtons() {
  if (homePowerConsumed) {
    if (digitalRead(BTN_POWER) == LOW)
      homePowerConsumed = false;
    return; // Нажатие, остановившее возврат, не должно также обнулять обе оси при отпускании кнопки.
  }
  if (homingActive) {
    if (digitalRead(BTN_POWER) == HIGH) {
      homePowerConsumed = true;
      powerButtonPressed = false;
      homingActive = false;
      serialMoveNaklonActive = false;
      motor_Naklon.brake();
      motor_Naklon.setTarget(motor_Naklon.getCurrent());
      releaseDrivers();
      executionMessage = "Возврат в начало остановлен";
    }
    return;
  }
  bool power = digitalRead(BTN_POWER) == HIGH;

  if (power && !powerButtonPressed) {
    powerButtonPressed = true;
    powerButtonPressTime = millis();
  }

  if (!power && powerButtonPressed) {
    uint32_t duration = millis() - powerButtonPressTime;
    powerButtonPressed = false;

    if (duration < LONG_PRESS_TIME) {
      resetBothPositions();
    } else {
      if (programRunning)
        stopProgram();
      else
        startProgram();
    }
  }

  // Во время программы остальные кнопки не управляют моторами.
  if (programRunning)
    return;

  bool left = digitalRead(BTN_LEFT) == HIGH;
  bool right = digitalRead(BTN_RIGHT) == HIGH;
  bool menu = digitalRead(BTN_MENU) == HIGH;
  bool autoB = digitalRead(BTN_AUTO) == HIGH;

  // --- Наклон ---
  if (autoB && menu) {
    if (motorNaklonMoving) {
      motor_Naklon.setTarget(motor_Naklon.getCurrent());
      motorNaklonMoving = false;
      moveDirectionNaklon = 0;
      releaseDrivers();
    }
  } else if (autoB) {
    if (canMoveY(1) && moveDirectionNaklon != 1) {
      moveDirectionNaklon = 1;
      motorNaklonMoving = true;
      serialMoveNaklonActive = false;
      enableDrivers();
      motor_Naklon.setTarget(1000000);
    }
  } else if (menu) {
    if (canMoveY(-1) && moveDirectionNaklon != -1) {
      moveDirectionNaklon = -1;
      motorNaklonMoving = true;
      serialMoveNaklonActive = false;
      enableDrivers();
      motor_Naklon.setTarget(-1000000);
    }
  }

  if (!autoB && !menu && motorNaklonMoving) {
    motor_Naklon.setTarget(motor_Naklon.getCurrent());
    motorNaklonMoving = false;
    moveDirectionNaklon = 0;
    releaseDrivers();
  }

  // --- Поворот ---
  if (left && right) {
    if (motorPovorotMoving) {
      motor_Povorot.setTarget(motor_Povorot.getCurrent());
      motorPovorotMoving = false;
      moveDirectionPovorot = 0;
      releaseDrivers();
    }
  } else if (left) {
    if (moveDirectionPovorot != -1) {
      moveDirectionPovorot = -1;
      motorPovorotMoving = true;
      serialMovePovorotActive = false;
      enableDrivers();
      motor_Povorot.setTarget(-1000000);
    }
  } else if (right) {
    if (moveDirectionPovorot != 1) {
      moveDirectionPovorot = 1;
      motorPovorotMoving = true;
      serialMovePovorotActive = false;
      enableDrivers();
      motor_Povorot.setTarget(1000000);
    }
  }

  if (!left && !right && motorPovorotMoving) {
    motor_Povorot.setTarget(motor_Povorot.getCurrent());
    motorPovorotMoving = false;
    moveDirectionPovorot = 0;
    releaseDrivers();
  }
}

// ============================================================
// ПОСЛЕДОВАТЕЛЬНЫЙ ПОРТ
// ============================================================

void handleSerial() {
#if SERIAL_DEBUG
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (!serialInput.length())
        continue;

      char cmd = serialInput.charAt(0);
      String value = serialInput.substring(1);

      if ((cmd == 'N' || cmd == 'n') && !programRunning) {
        int angle = value.toInt();
        int32_t target = angleToStepsNaklon(angle);

        if (!homingActive &&
            canMoveY((target > motor_Naklon.getCurrent()) - (target < motor_Naklon.getCurrent()))) {
          motor_Naklon.setTarget(target);
          serialMoveNaklonActive = true;
          enableDrivers();
          DBG("Serial N -> ");
          DBGLN(target);
        }
      } else if ((cmd == 'P' || cmd == 'p') && !programRunning && !homingActive) {
        int angle = value.toInt();
        int32_t target = angleToStepsPovorot(angle);

        motor_Povorot.setTarget(target);
        serialMovePovorotActive = true;
        enableDrivers();
        DBG("Serial P -> ");
        DBGLN(target);
      } else if ((cmd == 'T' || cmd == 't') && value.length() >= 14) {
        int year = value.substring(0, 4).toInt();
        int mon = value.substring(4, 6).toInt();
        int day = value.substring(6, 8).toInt();
        int hour = value.substring(8, 10).toInt();
        int min = value.substring(10, 12).toInt();
        int sec = value.substring(12, 14).toInt();

        struct tm tmv = {};
        tmv.tm_year = year - 1900;
        tmv.tm_mon = mon - 1;
        tmv.tm_mday = day;
        tmv.tm_hour = hour;
        tmv.tm_min = min;
        tmv.tm_sec = sec;
        time_t t = mktime(&tmv);

        struct timeval tv = {t, 0};
        settimeofday(&tv, nullptr);
        oledMarkDirty();
        DBGLN("Время установлено из Serial");
      } else if (cmd == 'S' || cmd == 's') {
        startProgram();
      } else if (cmd == 'X' || cmd == 'x') {
        stopProgram();
      } else if (cmd == 'R' || cmd == 'r') {
        resetBothPositions();
      } else {
        long target = serialInput.toInt();
        if (!programRunning && !homingActive &&
            canMoveY((target > motor_Naklon.getCurrent()) - (target < motor_Naklon.getCurrent()))) {
          motor_Naklon.setTarget(target);
          serialMoveNaklonActive = true;
          enableDrivers();
        }
      }

      serialInput = "";
    } else {
      if (serialInput.length() < 40)
        serialInput += c;
    }
  }
#endif
}

// ============================================================
// ЭКРАН OLED
// ============================================================

const uint8_t icon_no_check_16x16[] PROGMEM = {
    0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xFF,
    0xFF, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFF};

const uint8_t icon_check_16x16[] PROGMEM = {
    0xFF, 0x01, 0x01, 0x31, 0x71, 0xE1, 0x81, 0x01, 0x01, 0x81, 0xC1, 0xF0, 0x38, 0x1E, 0x07, 0xE3,
    0xFF, 0x80, 0x80, 0x80, 0x80, 0x81, 0x83, 0x8E, 0x8E, 0x83, 0x81, 0x80, 0x80, 0x80, 0x80, 0xFF};

const uint8_t icon_gradus_7x7[] PROGMEM = {0x1C, 0x3E, 0x63, 0x63, 0x63, 0x3E, 0x1C};
const uint8_t po_chasovoy_16x16[] PROGMEM = {
    0xC0, 0xF0, 0x3C, 0x0E, 0x06, 0x03, 0x03, 0x03, 0x03, 0x87, 0x8E, 0x9C, 0xF8, 0xE0, 0xFC, 0xFC,
    0x03, 0x0F, 0x3C, 0x70, 0x60, 0xC0, 0xC0, 0x80, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
const uint8_t protiv_chasovoy_16x16[] PROGMEM = {
    0xFC, 0xFC, 0xE0, 0xF8, 0x9C, 0x8E, 0x87, 0x03, 0x03, 0x03, 0x03, 0x06, 0x0E, 0x3C, 0xF0, 0xC0,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x80, 0xC0, 0xC0, 0x60, 0x70, 0x3C, 0x0F, 0x03};
const uint8_t ugol_8x16[] PROGMEM = {0x00, 0x00, 0x00, 0xC0, 0xF0, 0x3C, 0x0E, 0x02,
                                     0x38, 0x3C, 0x37, 0x33, 0x30, 0x30, 0x30, 0x30};
const uint8_t dot_8x8[] PROGMEM = {0x07, 0x07, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t duble_dot_8x8[] PROGMEM = {0xE7, 0xE7, 0xE7, 0x00, 0x00, 0x00, 0x00, 0x00};

void oledMarkDirty() { oledDirty = true; }

void oledShowStartup() {
  oled.clear();
  oled.autoPrintln(true);
  oled.home();
  oled.setScale(2);
  oled.print(F("Автомат"));
  oled.setScale(3);
  oled.setCursorXY(0, 19);
  oled.print(F("РЕМЮАЖА"));
  oled.setScale(2);
  oled.setCursorXY(30, 43);
  oled.print("v");
  oled.print(REMUATOR_VERSION);
  oled.update();
  delay(2000);
}

void oled_time(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  oled.setScale(2);
  oled.setCursorXY(0, 0);
  oled.print(tmv.tm_mday);
  oled.drawBitmap(24, 12, dot_8x8, 8, 8);
  oled.setCursorXY(29, 0);
  oled.print(tmv.tm_mon + 1);
  oled.setCursorXY(69, 0);
  oled.print(tmv.tm_hour);
  oled.drawBitmap(94, 4, duble_dot_8x8, 8, 8);
  oled.setCursorXY(100, 0);
  oled.print(tmv.tm_min);
}

void oled_next_time(time_t next) {
  oled.rect(0, 48, 128, 64, OLED_CLEAR);
  oled.update(0, 48, 128, 64);

  if (currentProgramStep + 1 < runProgram.count) {
    ProgramStep &n = runProgram.steps[currentProgramStep + 1];
    oled.setScale(2);
    oled.setCursorXY(0, 48);
    oled.print(n.angleNaklon);
    oled.drawBitmap(25, 48, n.anglePovorot > 0 ? po_chasovoy_16x16 : protiv_chasovoy_16x16, 16, 16);
    oled.setCursorXY(40, 48);
    oled.print(abs(n.anglePovorot));
  }

  struct tm tmv;
  localtime_r(&next, &tmv);
  oled.setCursorXY(75, 48);
  oled.print(tmv.tm_hour);
  oled.drawBitmap(99, 52, duble_dot_8x8, 8, 8);
  oled.setCursorXY(102, 48);
  oled.print(tmv.tm_min);
  oled.update(0, 48, 128, 64);
}

void oled_check(uint8_t row) {
  uint8_t y = row == 2 ? 16 : 32;
  oled.rect(112, y, 128, y + 15, OLED_CLEAR);
  oled.drawBitmap(112, y, icon_check_16x16, 16, 16);
  oled.update(112, y, 128, y + 15);
}

void oled_no_check() {
  oled.rect(112, 16, 128, 46, OLED_CLEAR);
  oled.drawBitmap(112, 16, icon_no_check_16x16, 16, 16);
  oled.drawBitmap(112, 32, icon_no_check_16x16, 16, 16);
  oled.update(112, 16, 128, 46);
}

void oled_2_ugol_naclona(uint8_t angle) {
  oled.setScale(2);
  oled.setCursorXY(68, 16);
  oled.print(angle);
  oled.drawBitmap(92, 16, icon_gradus_7x7, 7, 7);
  oled.update(68, 16, 99, 31);
}

void oled_3_ugol_povorota(int16_t angle) {
  oled.rect(52, 32, 111, 46, OLED_CLEAR);
  oled.update(52, 32, 111, 46);
  oled.drawBitmap(52, 32, angle > 0 ? po_chasovoy_16x16 : protiv_chasovoy_16x16, 16, 16);
  oled.setScale(2);
  oled.setCursorXY(68, 32);
  oled.print(abs(angle));
  oled.drawBitmap(abs(angle) > 100 ? 104 : 92, 32, icon_gradus_7x7, 7, 7);
  oled.update(52, 32, 111, 46);
}

void oled_2_step(uint8_t step) {
  if (step < 10) {
    oled.setScale(3);
    oled.setCursorXY(12, 20);
    oled.print(step);
    oled.update(12, 16, 51, 31);
  } else {
    oled.setScale(3);
    oled.setCursorXY(0, 20);
    oled.print(step);
    oled.update(0, 16, 51, 31);
  }
}

void printCurrentTimeOLED() {
  time_t now = nowEpoch();
  oled.clear();
  oled.autoPrintln(true);
  oled.home();
  oled.setScale(3);
  struct tm tmv;
  localtime_r(&now, &tmv);
  if (tmv.tm_mday < 10)
    oled.print(F("0"));
  oled.print(tmv.tm_mday);
  oled.setScale(1);
  oled.print(F("."));
  oled.setScale(3);
  if (tmv.tm_mon + 1 < 10)
    oled.print(F("0"));
  oled.print(tmv.tm_mon + 1);
  oled.setScale(1);
  oled.print(F("."));
  oled.setScale(3);
  oled.print((tmv.tm_year + 1900) - 2000);
  oled.print(F(" "));
  if (tmv.tm_hour < 10)
    oled.print(F("0"));
  oled.print(tmv.tm_hour);
  oled.print(F(":"));
  if (tmv.tm_min < 10)
    oled.print(F("0"));
  oled.print(tmv.tm_min);
  oled.update();
}

void oledRenderProgram() {
  if (!programRunning || currentProgramStep >= runProgram.count)
    return;
  time_t displayStepStart = scheduledEpoch[currentProgramStep] > 1000000000UL
                                ? scheduledEpoch[currentProgramStep]
                                : nowEpoch();
  ProgramStep &s = runProgram.steps[currentProgramStep];
  oled.clear();
  oled_time(displayStepStart);
  oled_2_step(currentProgramStep + 1);
  oled.drawBitmap(52, 16, ugol_8x16, 8, 16);
  oled_2_ugol_naclona(s.angleNaklon);
  oled.drawBitmap(112, 16, execState.naklonCompleted ? icon_check_16x16 : icon_no_check_16x16, 16,
                  16);
  oled.drawBitmap(52, 32, s.anglePovorot > 0 ? po_chasovoy_16x16 : protiv_chasovoy_16x16, 16, 16);
  oled.setScale(2);
  oled.setCursorXY(68, 32);
  oled.print(abs(s.anglePovorot));
  oled.drawBitmap(abs(s.anglePovorot) > 100 ? 104 : 92, 32, icon_gradus_7x7, 7, 7);
  oled.drawBitmap(112, 32, execState.povorotCompleted ? icon_check_16x16 : icon_no_check_16x16, 16,
                  16);
  oled.setScale(1);
  oled.setCursorXY(0, 47);
  oled.print(F("Пауза "));
  oled.print(programPauseText(s.pauseSeconds).c_str());
  if (programPaused)
    oled.print(F(" ПАУЗА"));

  if (currentProgramStep + 1 < runProgram.count) {
    time_t next = scheduledTimeForStep(currentProgramStep + 1);
    oled_next_time(next);
  } else {
    oled.rect(0, 48, 128, 64, OLED_CLEAR);
    oled.update(0, 48, 128, 64);
  }
  oled.update();
}

// ============================================================
//                  ИКОНКИ WI-FI / AP НА OLED
// ============================================================
// Две отдельные иконки 16x16: слева HOME Wi-Fi, справа AP.
// Они могут отображаться одновременно.
const uint8_t wifiIcon16_0[] PROGMEM = {
    0x00, 0x00, 0x03, 0xC0, 0x0F, 0xF0, 0x1C, 0x38, 0x38, 0x1C, 0x60, 0x06, 0x71, 0x8E, 0x03, 0xC0,
    0x07, 0xE0, 0x01, 0x80, 0x03, 0xC0, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00};
const uint8_t wifiIcon16_1[] PROGMEM = {
    0x00, 0x00, 0x03, 0xC0, 0x0F, 0xF0, 0x1C, 0x38, 0x38, 0x1C, 0x60, 0x06, 0x71, 0x8E, 0x03, 0xC0,
    0x07, 0xE0, 0x01, 0x80, 0x03, 0xC0, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00};
const uint8_t wifiIcon16_2[] PROGMEM = {
    0x00, 0x00, 0x03, 0xC0, 0x0F, 0xF0, 0x1C, 0x38, 0x38, 0x1C, 0x60, 0x06, 0x71, 0x8E, 0x03, 0xC0,
    0x07, 0xE0, 0x01, 0x80, 0x03, 0xC0, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00};
// AP — отдельная иконка антенны/точки доступа, не Wi-Fi-дуги.
const uint8_t apIcon16_0[] PROGMEM = {
    0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0F, 0xF0, 0x09, 0x90,
    0x09, 0x90, 0x09, 0x90, 0x09, 0x90, 0x0F, 0xF0, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x18, 0x18};
const uint8_t apIcon16_1[] PROGMEM = {
    0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0F, 0xF0, 0x0F, 0xF0,
    0x0F, 0xF0, 0x09, 0x90, 0x09, 0x90, 0x0F, 0xF0, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x18, 0x18};
const uint8_t iconDisabled16[] PROGMEM = {
    0x80, 0x01, 0xC0, 0x03, 0x60, 0x06, 0x30, 0x0C, 0x18, 0x18, 0x0C, 0x30, 0x06, 0x60, 0x03, 0xC0,
    0x01, 0x80, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x18, 0x18, 0x30, 0x0C, 0x60, 0x06, 0xC0, 0x03};

void oledDrawNetworkIcon(bool force = false) {
  uint32_t nowMs = millis();
  if (!force && nowMs - oledLastNetworkIcon < 300)
    return;
  oledLastNetworkIcon = nowMs;
  oledNetworkAnim = (oledNetworkAnim + 1) % 3;
  bool homeWiFi = (WiFi.status() == WL_CONNECTED);
  bool apClient = (WiFi.softAPgetStationNum() > 0);

  oled.rect(96, 48, 127, 63, OLED_CLEAR);
  if (homeWiFi) {
    const uint8_t *f = (oledNetworkAnim == 0)   ? wifiIcon16_0
                       : (oledNetworkAnim == 1) ? wifiIcon16_1
                                                : wifiIcon16_2;
    oled.drawBitmap(96, 48, f, 16, 16);
  } else {
    oled.drawBitmap(96, 48, iconDisabled16, 16, 16);
  }
  if (apClient) {
    const uint8_t *f = (oledNetworkAnim == 1) ? apIcon16_1 : apIcon16_0;
    oled.drawBitmap(112, 48, f, 16, 16);
  } else {
    oled.drawBitmap(112, 48, iconDisabled16, 16, 16);
  }
}
void oledRenderIdle() {
  time_t now = nowEpoch();
  struct tm tmv;
  localtime_r(&now, &tmv);
  oled.clear();
  oled.setScale(2);
  oled.setCursorXY(0, 0);
  if (tmv.tm_mday < 10)
    oled.print('0');
  oled.print(tmv.tm_mday);
  oled.print('.');
  if (tmv.tm_mon + 1 < 10)
    oled.print('0');
  oled.print(tmv.tm_mon + 1);
  oled.setCursorXY(70, 0);
  if (tmv.tm_hour < 10)
    oled.print('0');
  oled.print(tmv.tm_hour);
  oled.print(':');
  if (tmv.tm_min < 10)
    oled.print('0');
  oled.print(tmv.tm_min);
  oled.setScale(1);
  oled.setCursorXY(0, 25);
  oled.print(programs.empty() ? "Нет CSV" : programs[selectedProgram].name);
  oled.setCursorXY(0, 42);
  oled.print(F("POWER=Start"));
  oledDrawNetworkIcon(true);
  oled.update();
}

void oledShowStatus() {
  time_t now = nowEpoch();
  struct tm tmv;
  localtime_r(&now, &tmv);
  int32_t minuteKey = (int32_t)(tmv.tm_year + 1900) * 525600L + (int32_t)(tmv.tm_yday) * 1440L +
                      tmv.tm_hour * 60L + tmv.tm_min;
  bool minuteChanged = minuteKey != oledLastMinuteKey;

  if (!oledDirty && !minuteChanged) {
    if (!programRunning && millis() - oledLastNetworkIcon >= 300) {
      oledDrawNetworkIcon();
      oled.update(118, 56, 127, 63);
    }
    return;
  }
  oledLastMinuteKey = minuteKey;
  oledDirty = false;

  if (programRunning)
    oledRenderProgram();
  else
    oledRenderIdle();
}

// ============================================================
//                      ВЕБ-СТРАНИЦЫ
// ============================================================

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  if(!bootstrapWebReady()) {server.sendHeader("Location","/setup");server.send(303);return;}
  sendLittleFSFile("/index.html");
}
void handleBootstrapSetup() {
  server.send(200,"text/html; charset=utf-8",BOOTSTRAP_WIFI_HTML);
}
void handleApiHome();

bool acceptBrowserClock(bool force);
bool resumeRecoveredProgram();

void handleApiProgram() {
  String action = server.hasArg("action") ? server.arg("action") : "";
  if (action == "start") {
    if (!acceptBrowserClock(false))
      return;
    int32_t selectedStep = 1;
    if (server.hasArg("step") &&
        !programInteger(server.arg("step").c_str(), selectedStep, 1, MAX_PROGRAM_STEPS)) {
      server.send(422, "text/plain; charset=utf-8", "Неверный номер стартового шага");
      return;
    }
    startProgram((uint16_t)selectedStep);
    if (!programRunning || recoveryMotionUncertain) {
      server.send(409, "text/plain; charset=utf-8", executionMessage);
      return;
    }
  } else if (action == "stop")
    stopProgram();
  else if (action == "pause") {
    if (!pauseProgram()) {
      server.send(409, "text/plain", "NOT_RUNNING_OR_ALREADY_PAUSED");
      return;
    }
  } else if (action == "resume") {
    if (!resumeProgram()) {
      server.send(409, "text/plain", "NOT_PAUSED_OR_MOTORS_STOPPING");
      return;
    }
  } else if (action == "dismiss") {
    runMeta.noticeVisible = false;
    if (!saveExecution()) {
      server.send(507, "text/plain; charset=utf-8", executionMessage);
      return;
    }
  } else if (action == "reset") {
    if (zCalStarted) {
      server.send(409, "text/plain; charset=utf-8", "Остановите или отмените калибровку Z");
      return;
    }
    resetBothPositions();
  }
  else {
    server.send(400, "text/plain", "UNKNOWN_ACTION");
    return;
  }
  if (recoveryMotionUncertain && !programRunning) {
    server.send(507, "text/plain; charset=utf-8", executionMessage);
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleApiPower() {
  String action = server.hasArg("action") ? server.arg("action") : "short";
  if (zCalStarted) {
    // Не даём кнопке Power изменить счётчик STEP/360 во время измерения.
    zCalibrationStopMotion();
    server.send(409,"text/plain; charset=utf-8", "Идёт калибровка Z. Для завершения используйте Стоп или Отмена.");
    return;
  }
  if (action == "short")
    resetBothPositions();
  else if (action == "long") {
    if (programRunning)
      stopProgram();
    else
      startProgram();
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleApiManual() {
  if (zCalStarted) {
    // Путь ручного перемещения не должен подменять координату, измеряемую для 360°.
    if (server.hasArg("action") && server.arg("action") == "stop") {
      zCalibrationStopMotion();
      server.send(200,"text/plain; charset=utf-8","OK");
    } else server.send(409,"text/plain; charset=utf-8","Сначала завершите калибровку Z");
    return;
  }
  if (programRunning) {
    server.send(409, "text/plain", "PROGRAM_RUNNING");
    return;
  }
  String action = server.hasArg("action") ? server.arg("action") : "stop";
  if (action == "home") {
    if (!beginHoming()) {
      server.send(409, "text/plain; charset=utf-8", executionMessage);
      return;
    }
  } else if (homingActive && action != "stop") {
    server.send(409, "text/plain; charset=utf-8", "Выполняется возврат в начало");
    return;
  } else if (action == "tilt_up") {
    if (!canMoveY(1)) {
      server.send(409, "text/plain; charset=utf-8", "Ход к 85° заблокирован концевиком Y");
      return;
    }
    {
      moveDirectionNaklon = 1;
      motorNaklonMoving = true;
      serialMoveNaklonActive = false;
      enableDrivers();
      motor_Naklon.setTarget(1000000);
    }
  } else if (action == "tilt_down") {
    if (!canMoveY(-1)) {
      server.send(409, "text/plain; charset=utf-8", "Ход к 23° заблокирован концевиком Y");
      return;
    }
    {
      moveDirectionNaklon = -1;
      motorNaklonMoving = true;
      serialMoveNaklonActive = false;
      enableDrivers();
      motor_Naklon.setTarget(-1000000);
    }
  } else if (action == "rotate_left") {
    moveDirectionPovorot = -1;
    motorPovorotMoving = true;
    serialMovePovorotActive = false;
    enableDrivers();
    motor_Povorot.setTarget(-1000000);
  } else if (action == "rotate_right") {
    moveDirectionPovorot = 1;
    motorPovorotMoving = true;
    serialMovePovorotActive = false;
    enableDrivers();
    motor_Povorot.setTarget(1000000);
  } else {
    if (homingActive)
      executionMessage = "Возврат в начало остановлен";
    homingActive = homingZPhase = false;
    serialMoveNaklonActive = serialMovePovorotActive = false;
    motorNaklonMoving = motorPovorotMoving = false;
    moveDirectionNaklon = moveDirectionPovorot = 0;
    motor_Naklon.brake();
    motor_Povorot.brake();
    motor_Naklon.setTarget(motor_Naklon.getCurrent());
    motor_Povorot.setTarget(motor_Povorot.getCurrent());
    releaseDrivers();
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "OK");
}

String urlEncodePath(const String &raw) {
  const char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(raw.length() * 3);
  for (size_t i = 0; i < raw.length(); ++i) {
    uint8_t c = (uint8_t)raw[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      result += (char)c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

void handlePrograms() { sendLittleFSFile("/programs.html"); }

void handleProgramSelect() {
  if (programRunning) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите программу");
    return;
  }
  refreshPrograms(true);
  String path = server.arg("path");
  if (!path.length() && server.hasArg("p")) {
    int p = server.arg("p").toInt();
    if (p >= 0 && (size_t)p < programPaths.size())
      path = programPaths[p];
  }
  for (size_t i = 0; i < programPaths.size(); i++)
    if (programPaths[i] == path) {
      String error;
      if (!openProgramDraft(path, error)) {
        server.send(409, "text/plain; charset=utf-8", error);
        return;
      }
      selectedProgram = i;
      selectedProgramPath = path;
      prefs.putString("selectedPath", path);
      oledMarkDirty();
      redirectHome();
      return;
    }
  server.send(404, "text/plain; charset=utf-8", "Программа не найдена");
}

void handleProgramRestore() {
  if (programRunning) {
    server.send(409, "text/plain", "PROGRAM_RUNNING");
    return;
  }
  refreshPrograms(true);
  if (draftLoaded && draftDirty) {
    server.send(409, "text/plain; charset=utf-8",
                "Сначала сохраните или отмените изменения черновика");
    return;
  }
  draftLoaded = false;
  if (server.hasArg("path")) {
    const String path = server.arg("path");
    bool knownInvalid = false;
    for (const String &item : programPaths)
      if (item == path) {
        knownInvalid = true;
        break;
      }
    for (const String &item : invalidProgramFiles)
      if (item == path) {
        knownInvalid = true;
        break;
      }
    if (!knownInvalid || !isProgramCsv(path) || !LittleFS.exists(path)) {
      server.send(400, "text/plain", "INVALID_CSV_PATH");
      return;
    }
    // Структуру программы размером около 2,4 КиБ нельзя размещать на стеке асинхронной задачи HTTP.
    std::unique_ptr<RemuageProgram> def(new RemuageProgram{});
    String programName = path.substring(path.lastIndexOf('/') + 1, path.lastIndexOf('.'));
    createDefaultProgram(*def, programName);
    if (!writeProgramCsv(path, *def)) {
      server.send(500, "text/plain", "CSV_WRITE_ERROR");
      return;
    }
    programsDirty = true;
    refreshPrograms(true);
    oledMarkDirty();
    server.sendHeader("Location", "/programs");
    server.send(303);
    return;
  }
  if (!server.hasArg("p"))
    return redirectHome();
  int p = server.arg("p").toInt();
  if (p >= 0 && (size_t)p < programs.size()) {
    std::unique_ptr<RemuageProgram> backup(new RemuageProgram(programs[p]));
    createDefaultProgram(programs[p], String(backup->name));
    if (!saveOneProgramFile((size_t)p)) {
      programs[p] = *backup;
      server.send(500, "text/plain", "CSV_WRITE_ERROR");
      return;
    }
    oledMarkDirty();
  }
  server.sendHeader("Location", "/programs");
  server.send(303);
}

void handleProgramStop() {
  stopProgram();
  redirectHome();
}

void handleProgramEdit();
void handleEditorGet();
void handleEditorUpdate();
void handleEditorSave();
void handleEditorDiscard();
bool openProgramDraft(const String &path, String &error);

// Копирование UTF-8 имени без обрезания посередине многобайтного символа.
// Имя хранится в UTF-8; копирование не обрезает многобайтный символ.
void copyProgramNameUtf8(char *dst, size_t dstSize, const String &src) {
  if (dstSize == 0)
    return;
  size_t maxBytes = dstSize - 1;
  size_t n = 0;
  const uint8_t *p = (const uint8_t *)src.c_str();
  while (*p && n < maxBytes) {
    uint8_t len = 1;
    if ((*p & 0x80) == 0x00)
      len = 1;
    else if ((*p & 0xE0) == 0xC0)
      len = 2;
    else if ((*p & 0xF0) == 0xE0)
      len = 3;
    else if ((*p & 0xF8) == 0xF0)
      len = 4;
    else {
      p++;
      continue;
    }
    if (n + len > maxBytes)
      break;
    bool valid = true;
    for (uint8_t i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      p++;
      continue;
    }
    for (uint8_t i = 0; i < len; i++)
      dst[n++] = (char)p[i];
    p += len;
  }
  dst[n] = '\0';
}

void copyProgramNameUtf8(char *dst, size_t dstSize, const String &src);

// ============================================================
//              ФАЙЛОВОЕ ХРАНЕНИЕ ПРОГРАММ / CSV
// ============================================================
// Формат файла совместим с Excel:
// Формат столбцов: Step;incAngle;Angle;IncRot;Rot;Pause.
// UTF-8 с BOM. Разделитель ';', текстовые поля заключаются в кавычки.

// ============================================================
//          Каталог /prog — единственный источник программ
// ============================================================
// CSV UTF-8 BOM (необязательный): Step;incAngle;Angle;IncRot;Rot;Pause
// Имя файла определяет программу: удаление и переименование CSV учитываются при повторном поиске.

String csvEscape(const String &in) {
  String v = in;
  v.replace("\"", "\"\"");
  return String("\"") + v + "\"";
}

bool csvParseLine(const String &line, String fields[], uint8_t maxFields) {
  // Поддерживаются точки с запятой внутри кавычек и экранированные двойные кавычки.
  // Необязательный последний пустой столбец после завершающей точки с запятой Excel игнорируется.
  uint8_t field = 0;
  String cur;
  bool quoted = false;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.length() && line[i + 1] == '"') {
        cur += '"';
        i++;
      } else
        quoted = !quoted;
    } else if (c == ';' && !quoted) {
      if (field >= maxFields)
        return false;
      fields[field++] = cur;
      cur = "";
    } else
      cur += c;
  }
  if (quoted)
    return false;
  if (field == maxFields && cur.length() == 0)
    return true; // Завершающая точка с запятой.
  if (field != maxFields - 1)
    return false;
  fields[field] = cur;
  return true;
}

// Excel может сохранять CSV с кириллицей в Windows-1251 вместо UTF-8.
// Поддержка старого импорта сохраняется; чтение само по себе не должно перезаписывать исходный файл.
bool csvValidUtf8(const String &s) {
  const uint8_t *b = (const uint8_t *)s.c_str();
  size_t i = 0, n = s.length();
  while (i < n) {
    const uint8_t c = b[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    if (c >= 0xC2 && c <= 0xDF) {
      if (i + 1 >= n || (b[i + 1] & 0xC0) != 0x80)
        return false;
      i += 2;
      continue;
    }
    if (c >= 0xE0 && c <= 0xEF) {
      if (i + 2 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80)
        return false;
      if ((c == 0xE0 && b[i + 1] < 0xA0) || (c == 0xED && b[i + 1] >= 0xA0))
        return false;
      i += 3;
      continue;
    }
    if (c >= 0xF0 && c <= 0xF4) {
      if (i + 3 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80 ||
          (b[i + 3] & 0xC0) != 0x80)
        return false;
      if ((c == 0xF0 && b[i + 1] < 0x90) || (c == 0xF4 && b[i + 1] >= 0x90))
        return false;
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

void csvAppendUtf8(String &out, uint16_t cp) {
  if (cp < 0x80)
    out += (char)cp;
  else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

String csvWindows1251ToUtf8(const String &src) {
  static const uint16_t map80bf[64] = {
      0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409,
      0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0,
      0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB,
      0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6,
      0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};
  String out;
  out.reserve(src.length() * 2);
  const uint8_t *b = (const uint8_t *)src.c_str();
  for (size_t i = 0; i < src.length(); i++) {
    const uint8_t c = b[i];
    if (c < 0x80)
      out += (char)c;
    else if (c < 0xC0)
      csvAppendUtf8(out, map80bf[c - 0x80]);
    else
      csvAppendUtf8(out, 0x0410 + (c - 0xC0));
  }
  return out;
}

bool isProgramCsv(const String &path) {
  String lower = path;
  lower.toLowerCase();
  return lower.startsWith("/prog/") && lower.endsWith(".csv") && path.indexOf("..") < 0;
}

String programFilePath(size_t p) { return p < programPaths.size() ? programPaths[p] : String(); }

bool writeProgramCsv(const String &path, const RemuageProgram &program) {
  if (!isProgramCsv(path) || !program.count)
    return false;
  const std::string text = serializeProgramCsv(program);
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f)
    return false;
  size_t n = f.write((const uint8_t *)text.data(), text.size());
  f.flush();
  f.close();
  if (n != text.size()) {
    LittleFS.remove(tmp);
    return false;
  }
  File check = LittleFS.open(tmp, "r");
  bool ok = check && check.size() == text.size();
  if (check)
    check.close();
  if (!ok) {
    LittleFS.remove(tmp);
    return false;
  }
  // Переименование LittleFS атомарно заменяет существующий файл без промежуточного удаления.
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

bool saveOneProgramFile(size_t p) {
  if (p >= programs.size() || p >= programPaths.size())
    return false;
  bool ok = writeProgramCsv(programPaths[p], programs[p]);
  if (ok)
    programsDirty = true;
  return ok;
}

bool readProgramCsv(const String &path, RemuageProgram &loaded, String *error) {
  if (error)
    *error = "";
  if (!isProgramCsv(path)) {
    if (error)
      *error = "Недопустимое имя CSV";
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f || f.isDirectory() || f.size() > 65536) {
    if (f)
      f.close();
    if (error)
      *error = "Не удалось открыть CSV (максимум 64 КиБ)";
    return false;
  }
  String text = f.readString();
  f.close();
  if (!csvValidUtf8(text))
    text = csvWindows1251ToUtf8(text);
  std::string reason;
  bool ok = parseProgramCsv(text.c_str(), loaded, reason);
  if (!ok) {
    if (error)
      *error = reason.c_str();
    return false;
  }
  String name = path.substring(path.lastIndexOf('/') + 1, path.lastIndexOf('.'));
  if (name.length() >= sizeof(loaded.name)) {
    if (error)
      *error = "Слишком длинное имя файла";
    return false;
  }
  copyProgramNameUtf8(loaded.name, sizeof(loaded.name), name);
  return true;
}

// Не создавать четыре прежние программы по умолчанию и не восстанавливать устаревшую NVS в пустую LittleFS.
void ensureTemplateWhenEmpty() {
  if (!LittleFS.exists("/prog"))
    LittleFS.mkdir("/prog");
  File dir = LittleFS.open("/prog", "r");
  bool anyCsv = false;
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory() && isProgramCsv(String(f.path()))) {
        anyCsv = true;
        break;
      }
      f = dir.openNextFile();
    }
    dir.close();
  }
  // Повреждённый CSV считается пользовательскими данными и НЕ перезаписывается.
  // Встроенный шаблон создаётся только при ПОЛНОМ отсутствии файлов *.csv.
  if (!anyCsv) {
    // Эта функция вызывается из refreshPrograms() в задаче loopTask. Модель CSV,
    // созданная конструктором по умолчанию, слишком велика для вложенного стекового кадра.
    std::unique_ptr<RemuageProgram> def(new RemuageProgram{});
    createDefaultProgram(*def);
    const String path = "/prog/7 дней.csv";
    if (writeProgramCsv(path, *def))
      DBGLN("LittleFS: создан шаблон /prog/7 дней.csv");
  }
}

// Текущий выбор сохраняется по ПУТИ, а не по изменяющемуся индексу каталога.
// Движение использует неизменяемый снимок в памяти; внешние изменения CSV
// применяются только после остановки, чтобы не менять цели во время движения.
void refreshPrograms(bool force) {
  if (programRunning)
    return;
  uint32_t now = millis();
  if (!force && !programsDirty && (uint32_t)(now - lastProgramScanMs) < 3000)
    return;
  lastProgramScanMs = now;
  if (!LittleFS.exists("/prog"))
    LittleFS.mkdir("/prog");
  ensureTemplateWhenEmpty();
  std::vector<String> paths;
  File dir = LittleFS.open("/prog", "r");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      String path = String(f.path());
      if (!f.isDirectory() && isProgramCsv(path))
        paths.push_back(path);
      f = dir.openNextFile();
    }
    dir.close();
  }
  std::sort(paths.begin(), paths.end(),
            [](const String &a, const String &b) { return a.compareTo(b) < 0; });
  std::vector<RemuageProgram> nextPrograms;
  std::vector<String> nextPaths;
  std::vector<String> invalidPaths;
  std::vector<String> invalidErrors;
  // Входные модели CSV размещаются в куче вектора, а не на стеке loopTask.
  // В исходном скетче эта большая локальная модель и модель ensureTemplateWhenEmpty()
  // одновременно находились во вложенных стековых кадрах при первом запуске.
  nextPrograms.reserve(paths.size());
  for (const auto &path : paths) {
    nextPrograms.emplace_back();
    String reason;
    if (readProgramCsv(path, nextPrograms.back(), &reason)) {
      nextPaths.push_back(path);
    } else {
      nextPrograms.pop_back();
      invalidPaths.push_back(path);
      invalidErrors.push_back(reason);
      DBGLN(String("CSV требует исправления: ") + path + " | " + reason);
    }
  }

  if (nextPrograms.empty()) {
    invalidProgramFiles.swap(invalidPaths);
    invalidProgramErrors.swap(invalidErrors);
    programs.clear();
    programPaths.clear();
    selectedProgram = 0;
    selectedProgramPath = "";
    programsDirty = false;
    DBGLN("CSV: нет корректных программ; исходные файлы НЕ удалены");
    return;
  }
  String wanted = selectedProgramPath;
  if (!wanted.length())
    wanted = prefs.getString("selectedPath", "");
  size_t nextIndex = 0;
  for (size_t i = 0; i < nextPaths.size(); ++i)
    if (nextPaths[i] == wanted) {
      nextIndex = i;
      break;
    }
  programs.swap(nextPrograms);
  programPaths.swap(nextPaths);
  invalidProgramFiles.swap(invalidPaths);
  invalidProgramErrors.swap(invalidErrors);
  selectedProgram = nextIndex;
  selectedProgramPath = programPaths[selectedProgram];
  programsDirty = false;
  oledMarkDirty();
}

void markProgramsDirty(const String &path) {
  if (path == "/prog" || path.startsWith("/prog/"))
    programsDirty = true;
}

void ensureProgramsFile() {
  if (!mountProgramFS())
    return;
  if (!LittleFS.exists("/prog"))
    LittleFS.mkdir("/prog");
  // Прежний файл /program_N.csv переносится один раз; новые слоты не создаются.
  for (uint8_t p = 1; p <= 4; p++) {
    String oldPath = String("/program_") + p + ".csv";
    String newPath = String("/prog/program_") + p + ".csv";
    if (LittleFS.exists(oldPath) && !LittleFS.exists(newPath))
      LittleFS.rename(oldPath, newPath);
  }
  refreshPrograms(true);
}

bool mountProgramFS() {
  // Ошибка монтирования не должна стирать веб-файлы пользователя, программы или журнал восстановления.
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) {
    bootstrapFsMounted=false;
    DBGLN("LittleFS ERROR: partition label 'spiffs' missing; check partitions.csv.");
    return false;
  }
  DBGLN(String("LittleFS partition: address=0x") + String(part->address, HEX) +
        " size=" + String(part->size) + " bytes (" + String(part->size / 1024) + " KiB)");
  bool mounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");

  if(!mounted && bootstrapPartitionErased(part->size,[part](size_t offset,uint8_t *bytes,size_t count) {
      return esp_partition_read(part,offset,bytes,count)==ESP_OK;
    })) {
    DBGLN("LittleFS: erased partition detected; creating initial filesystem");
    mounted=LittleFS.begin(true,"/littlefs",10,"spiffs");
  }
  bootstrapFsMounted=mounted;
  if (!mounted) {
    DBGLN("LittleFS ERROR: mount failed; no format performed. Check flash/partition table.");
    return false;
  }
  DBGLN("LittleFS mount OK");
  const esp_partition_t *dumpPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (dumpPart)
    DBGLN(String("Core dump partition: 0x") + String(dumpPart->address, HEX) +
          " size=" + String(dumpPart->size / 1024) + " KiB");
  else
    DBGLN("Core dump partition missing: keep the existing partition layout when flashing via USB");
  DBGLN(String("LittleFS web files: index=") +
        String(LittleFS.exists("/index.html") || LittleFS.exists("/data/index.html")) +
        " css=" + String(LittleFS.exists("/style.css") || LittleFS.exists("/data/style.css")) +
        " js=" + String(LittleFS.exists("/app.js") || LittleFS.exists("/data/app.js")) +
        " favicon=" +
        String(LittleFS.exists("/favicon.ico") || LittleFS.exists("/data/favicon.ico")));
  return true;
}

#include "program_editor_backend.h"

// Настройки драйверов.

void loadTmcSettings(TmcUartSettings &c, const char *x) {
  String p(x);
  c.model = (TmcModel)prefs.getUShort((p + "model").c_str(), (uint16_t)c.model);
  c.baud = prefs.getUInt((p + "baud").c_str(), c.baud);
  c.address = c.model == TmcModel::Tmc2208 ? 0 : prefs.getUChar((p + "addr").c_str(), c.address);
  c.rmsCurrent = prefs.getUInt((p + "rms").c_str(), c.rmsCurrent);
  c.holdEnabled = prefs.getBool((p + "holdOn").c_str(), false);
  c.holdMultiplier = prefs.getFloat((p + "hold").c_str(), c.holdMultiplier);
  c.microsteps = prefs.getUInt((p + "micro").c_str(), c.microsteps);
  c.toff = prefs.getUChar((p + "toff").c_str(), c.toff);
  c.blankTime = prefs.getUChar((p + "blank").c_str(), c.blankTime);
  c.hysteresisEnd = prefs.getChar((p + "hend").c_str(), c.hysteresisEnd);
  c.hysteresisStart = prefs.getUChar((p + "hstart").c_str(), c.hysteresisStart);
  c.stealthChop = prefs.getBool((p + "stealth").c_str(), c.stealthChop);
  c.pwmAutoscale = prefs.getBool((p + "autoscale").c_str(), c.pwmAutoscale);
  c.pwmAutograd = prefs.getBool((p + "autograd").c_str(), c.pwmAutograd);
  c.pwmOfs = prefs.getUChar((p + "pwmo").c_str(), c.pwmOfs);
  c.pwmGrad = prefs.getUChar((p + "pwmgrad").c_str(), c.pwmGrad);
  c.pwmFreq = prefs.getUChar((p + "pwmfreq").c_str(), c.pwmFreq);
  c.pwmReg = prefs.getUChar((p + "pwmreg").c_str(), c.pwmReg);
  c.pwmLim = prefs.getUChar((p + "pwmlim").c_str(), c.pwmLim);
  c.freewheel = prefs.getUChar((p + "freewheel").c_str(), c.freewheel);
  c.intpol = prefs.getBool((p + "intpol").c_str(), c.intpol);
  c.pdnDisable = prefs.getBool((p + "pdn").c_str(), c.pdnDisable);
  c.iScaleAnalog = prefs.getBool((p + "iscale").c_str(), c.iScaleAnalog);
  c.internalRsense = prefs.getBool((p + "irsense").c_str(), c.internalRsense);
  c.multistepFilter = prefs.getBool((p + "msfilter").c_str(), c.multistepFilter);
  c.sendDelay = prefs.getUChar((p + "delay").c_str(), c.sendDelay);
}
void saveTmcSettings(const TmcUartSettings &c, const char *x) {
  String p(x);
  prefs.putUShort((p + "model").c_str(), (uint16_t)c.model);
  prefs.putUInt((p + "baud").c_str(), c.baud);
  prefs.putUChar((p + "addr").c_str(), c.address);
  prefs.putUInt((p + "rms").c_str(), c.rmsCurrent);
  prefs.putBool((p + "holdOn").c_str(), c.holdEnabled);
  prefs.putFloat((p + "hold").c_str(), c.holdMultiplier);
  prefs.putUInt((p + "micro").c_str(), c.microsteps);
  prefs.putUChar((p + "toff").c_str(), c.toff);
  prefs.putUChar((p + "blank").c_str(), c.blankTime);
  prefs.putChar((p + "hend").c_str(), c.hysteresisEnd);
  prefs.putUChar((p + "hstart").c_str(), c.hysteresisStart);
  prefs.putBool((p + "stealth").c_str(), c.stealthChop);
  prefs.putBool((p + "autoscale").c_str(), c.pwmAutoscale);
  prefs.putBool((p + "autograd").c_str(), c.pwmAutograd);
  prefs.putUChar((p + "pwmo").c_str(), c.pwmOfs);
  prefs.putUChar((p + "pwmgrad").c_str(), c.pwmGrad);
  prefs.putUChar((p + "pwmfreq").c_str(), c.pwmFreq);
  prefs.putUChar((p + "pwmreg").c_str(), c.pwmReg);
  prefs.putUChar((p + "pwmlim").c_str(), c.pwmLim);
  prefs.putUChar((p + "freewheel").c_str(), c.freewheel);
  prefs.putBool((p + "intpol").c_str(), c.intpol);
  prefs.putBool((p + "pdn").c_str(), c.pdnDisable);
  prefs.putBool((p + "iscale").c_str(), c.iScaleAnalog);
  prefs.putBool((p + "irsense").c_str(), c.internalRsense);
  prefs.putBool((p + "msfilter").c_str(), c.multistepFilter);
  prefs.putUChar((p + "delay").c_str(), c.sendDelay);
}
void applyTmcSettings(TMC2208Stepper &d, const TmcUartSettings &c) {
  d.senddelay(c.sendDelay);
  d.pdn_disable(true);
  d.mstep_reg_select(true); // MS1/MS2 задают адрес; микрошаг берём из регистра.
  d.I_scale_analog(c.iScaleAnalog);
  d.internal_Rsense(c.internalRsense);
  d.hold_multiplier(c.holdMultiplier);
  d.rms_current(c.rmsCurrent);
  d.microsteps(c.microsteps);
  d.toff(c.toff);
  d.blank_time(c.blankTime);
  d.hysteresis_end(c.hysteresisEnd);
  d.hysteresis_start(c.hysteresisStart);
  d.en_spreadCycle(!c.stealthChop);
  d.pwm_autoscale(c.pwmAutoscale);
  d.pwm_autograd(c.pwmAutograd);
  d.pwm_ofs(c.pwmOfs);
  d.pwm_grad(c.pwmGrad);
  d.pwm_freq(c.pwmFreq);
  d.pwm_reg(c.pwmReg);
  d.pwm_lim(c.pwmLim);
  d.freewheel(c.freewheel);
  d.intpol(c.intpol);
  d.multistep_filt(c.multistepFilter);
  d.VACTUAL(0); // Движение задаётся внешними STEP/DIR.
}

void handleUartSettings() { sendLittleFSFile("/uart.html"); }

void handleUartSettingsSave() {
  if (zCalStarted || calibrationBusy()) {
    server.send(409,"text/plain; charset=utf-8","Калибровка не завершена");
    return;
  }
  if (programRunning || motorNaklonMoving || motorPovorotMoving || motor_Naklon.getState() ||
      motor_Povorot.getState()) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите движение");
    return;
  }
  if (!server.hasArg("motor"))
    return redirectHome();
  const String motor = server.arg("motor");
  if (motor == "bus") {
    int32_t baud, yModel, zModel, yAddress = 0, zAddress = 0;
    if (!programInteger(server.arg("baud").c_str(), baud, 9600, 500000) ||
        !programInteger(server.arg("nakModel").c_str(), yModel, 2208, 2209) ||
        !programInteger(server.arg("povModel").c_str(), zModel, 2208, 2209) ||
        (yModel == 2209 && !server.hasArg("nakAddr")) ||
        (zModel == 2209 && !server.hasArg("povAddr")) ||
        (server.hasArg("nakAddr") &&
         !programInteger(server.arg("nakAddr").c_str(), yAddress, 0, 3)) ||
        (server.hasArg("povAddr") &&
         !programInteger(server.arg("povAddr").c_str(), zAddress, 0, 3))) {
      server.send(400, "text/plain; charset=utf-8",
                  "Проверьте модель, адрес 0..3 и скорость UART 9600..500000");
      return;
    }
    TmcUartSettings nextY = uartNaklon, nextZ = uartPovorot;
    nextY.model = (TmcModel)yModel;
    nextZ.model = (TmcModel)zModel;
    nextY.address = (uint8_t)yAddress;
    nextZ.address = (uint8_t)zAddress;
    if (const char *error = tmcBusError(nextY, nextZ)) {
      server.send(400, "text/plain; charset=utf-8", error);
      return;
    }
    if (nextY.rmsCurrent > tmcMaxRms(nextY.model) || nextZ.rmsCurrent > tmcMaxRms(nextZ.model)) {
      server.send(400, "text/plain; charset=utf-8",
                  "Сначала уменьшите ток: максимум TMC2208 — 1400 мА RMS, TMC2209 — 2000 мА RMS");
      return;
    }
    // Проверяем всю пару ДО изменения UART, чтобы сменить оба адреса вместе.
    disableDrivers();
    TmcUartSettings oldY = uartNaklon, oldZ = uartPovorot;
    TmcSerial.flush();
    uartNaklon = nextY;
    uartPovorot = nextZ;
    normalizeTmcBusSettings((uint32_t)baud);
    TmcSerial.updateBaudRate(uartNaklon.baud);
    delay(20);
    configureTmcDrivers();
    if (!tmcNaklon.settingsApplied || !tmcPovorot.settingsApplied) {
      const String fail = String("UART: настройки не подтверждены. Y=") +
        (tmcNaklon.settingsApplied ? "OK" : "ERROR") + " Z=" +
        (tmcPovorot.settingsApplied ? "OK" : "ERROR") +
        ". Проверьте VIO/VMOT, PDN_UART и MS1/MS2 (адрес Z должен совпадать с перемычками).";
      uartNaklon = oldY;
      uartPovorot = oldZ;
      TmcSerial.updateBaudRate(oldY.baud);
      delay(20);
      configureTmcDrivers(); // восстановление прежних параметров (best effort)
      server.send(503, "text/plain; charset=utf-8", fail);
      return;
    }
    if(!persistTmcFile(uartNaklon,uartPovorot,NAKLON_ACCELERATION,NAKLON_MAX_SPEED,POVOROT_ACCELERATION,POVOROT_MAX_SPEED)) {
      uartNaklon=oldY;uartPovorot=oldZ;TmcSerial.updateBaudRate(oldY.baud);configureTmcDrivers();releaseDrivers();
      server.send(507,"text/plain; charset=utf-8","Не удалось записать файл UART; прежние настройки восстановлены");return;
    }
    prefs.putUInt("tmcBusBaud", uartNaklon.baud);
    saveTmcSettings(uartNaklon, "tn");
    saveTmcSettings(uartPovorot, "tp");
    if (oldZ.model != uartPovorot.model || oldZ.address != uartPovorot.address)
      zCalibrationInvalidate();
    return redirectHome();
  }
  if (motor != "nak" && motor != "pov") {
    server.send(400, "text/plain", "UNKNOWN_MOTOR");
    return;
  }

  const bool pov = (motor == "pov");
  normalizeTmcBusSettings(uartNaklon.baud);
  TmcUartSettings c = pov ? uartPovorot : uartNaklon;
  int32_t rms, micro, blank, hend, hstart;
  if (!programInteger(server.arg("rms").c_str(), rms, 50, tmcMaxRms(c.model)) ||
      !programInteger(server.arg("micro").c_str(), micro, 1, 256) || (micro & (micro - 1)) ||
      !programInteger(server.arg("blank").c_str(), blank, 16, 54) ||
      (blank != 16 && blank != 24 && blank != 36 && blank != 54) ||
      !programInteger(server.arg("hend").c_str(), hend, -3, 12) ||
      !programInteger(server.arg("hstart").c_str(), hstart, 1, 8)) {
    server.send(400, "text/plain; charset=utf-8",
                "Неверные параметры драйвера: ток, микрошаги, blank time или гистерезис");
    return;
  }
  c.rmsCurrent = (uint16_t)rms;
  c.holdEnabled = server.hasArg("holdEnabled");
  if (server.hasArg("holdPercent"))
    c.holdMultiplier = constrain(server.arg("holdPercent").toFloat() / 100.0f, 0.1f, 1.0f);
  else if (server.hasArg("hold")) c.holdMultiplier = constrain(server.arg("hold").toFloat(), 0.1f, 1.0f);
  int32_t acceleration = pov ? POVOROT_ACCELERATION : NAKLON_ACCELERATION;
  int32_t speed = pov ? POVOROT_MAX_SPEED : NAKLON_MAX_SPEED;
  if ((server.hasArg(pov ? "pa" : "na") && !programInteger(server.arg(pov ? "pa" : "na").c_str(), acceleration, 1, 2000000000)) ||
      (server.hasArg(pov ? "ps" : "ns") && !programInteger(server.arg(pov ? "ps" : "ns").c_str(), speed, 1, 2000000000))) {
    server.send(422,"text/plain; charset=utf-8","Скорость и ускорение должны быть положительными целыми числами"); return;
  }
  c.microsteps = (uint16_t)micro;
  c.toff = constrain(server.arg("toff").toInt(), 1, 15); // 0 отключает выходы драйвера!
  c.blankTime = (uint8_t)blank;
  c.hysteresisEnd = (int8_t)hend;
  c.hysteresisStart = (uint8_t)hstart;
  c.pwmOfs = constrain(server.arg("pwmo").toInt(), 0, 255);
  c.pwmGrad = constrain(server.arg("pwmgrad").toInt(), 0, 255);
  c.pwmFreq = constrain(server.arg("pwmfreq").toInt(), 0, 3);
  c.pwmReg = constrain(server.arg("pwmreg").toInt(), 1, 15);
  c.pwmLim = constrain(server.arg("pwmlim").toInt(), 0, 15);
  c.freewheel = constrain(server.arg("freewheel").toInt(), 0, 3);
  c.sendDelay = constrain(server.arg("delay").toInt(), 2, 15);
  c.stealthChop = server.hasArg("stealth");
  c.pwmAutoscale = server.hasArg("autoscale");
  c.pwmAutograd = server.hasArg("autograd");
  c.intpol = server.hasArg("intpol");
  c.pdnDisable = true;
  c.iScaleAnalog = server.hasArg("iscale");
  c.internalRsense = server.hasArg("irsense");
  c.multistepFilter = server.hasArg("msfilter");
  disableDrivers();
  TmcAxis &axis = pov ? tmcPovorot : tmcNaklon;
  if (!applyTmcSettingsVerified(axis, c)) {
    // Нельзя сохранять в NVS значения, которые не достигли драйвера!
    applyTmcSettingsVerified(axis, pov ? uartPovorot : uartNaklon);
    server.send(503, "text/plain; charset=utf-8",
      String("Настройки не применены: TMC") + String(uint16_t(c.model)) +
      " оси " + (pov ? "Z" : "Y") + " не подтвердил UART-записи (IOIN/IFCNT). "
      "Проверьте физический UART-адрес, VIO/VMOT, GPIO21/47, 1 кОм на TX. "
      "При общей шине с TMC2208@0 адрес TMC2209 должен быть 1, 2 или 3.");
    return;
  }
  if(!persistTmcFile(pov?uartNaklon:c,pov?c:uartPovorot,pov?NAKLON_ACCELERATION:acceleration,
      pov?NAKLON_MAX_SPEED:speed,pov?acceleration:POVOROT_ACCELERATION,pov?speed:POVOROT_MAX_SPEED)) {
    applyTmcSettingsVerified(axis,pov?uartPovorot:uartNaklon);releaseDrivers();
    server.send(507,"text/plain; charset=utf-8","Файл UART не сохранён; прежние параметры восстановлены");return;
  }
  prefs.putUInt("tmcBusBaud", uartNaklon.baud);
  saveTmcSettings(c, pov ? "tp" : "tn");
  if (pov && c.microsteps != uartPovorot.microsteps) zCalibrationInvalidate();
  (pov ? uartPovorot : uartNaklon) = c;
  if (pov) {
    POVOROT_ACCELERATION=acceleration; POVOROT_MAX_SPEED=speed;
    prefs.putUInt("povAcc",acceleration); prefs.putUInt("povSpd",speed);
    motor_Povorot.setAcceleration(acceleration); motor_Povorot.setMaxSpeed(speed);
  } else {
    NAKLON_ACCELERATION=acceleration; NAKLON_MAX_SPEED=speed;
    prefs.putUInt("nakAcc",acceleration); prefs.putUInt("nakSpd",speed);
    motor_Naklon.setAcceleration(acceleration); motor_Naklon.setMaxSpeed(speed);
  }
  releaseDrivers();
  reportTmcConnection(axis, pov ? "Z/POVOROT" : "Y/NAKLON", c);
  redirectHome();
}

// ============================================================
//                         НАСТРОЙКИ
// ============================================================

bool saveSerialOutput(bool enabled) {
  if(prefs.putBool("serialOn",enabled)!=sizeof(bool)) return false;
  serialOutputEnabled=enabled;
  return true;
}
void handleSerialSettings() {
  server.send(200,"application/json; charset=utf-8",String("{\"ok\":true,\"serialOn\":")+
    (serialOutputEnabled?"true":"false")+"}");
}
void handleSerialSettingsSave() {
  if(programRunning || stepMotionPending() || otaInProgress || zCalStarted) {
    server.send(409,"text/plain; charset=utf-8","Сначала остановите программу и двигатели");return;
  }
  const String enabled=server.arg("enabled");
  if(enabled!="0" && enabled!="1") {server.send(422,"text/plain; charset=utf-8","Укажите состояние Serial: 0 или 1");return;}
  if(!saveSerialOutput(enabled=="1")) {server.send(507,"text/plain; charset=utf-8","Не удалось сохранить настройку Serial");return;}
  handleSerialSettings();
}
void handleSettings() { sendLittleFSFile("/settings.html"); }

void handleSettingsSave() {
  if (zCalStarted || calibrationBusy()) { server.send(409,"text/plain","CALIBRATION_BUSY");return; }
  if (programRunning || motorNaklonMoving || motorPovorotMoving || motor_Naklon.getState() ||
      motor_Povorot.getState()) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите движение");
    return;
  }
  const bool nextSerialOutput = server.hasArg("serialOn");
  if (server.hasArg("web"))
    WEB_REQUEST_PERIOD_MS = constrain(server.arg("web").toInt(), 1, 1000);
  if (server.hasArg("refresh"))
    WEB_PAGE_REFRESH_PERIOD_MS = constrain(server.arg("refresh").toInt(), 250, 60000);
  if (server.hasArg("pn"))
    PAUSE_AFTER_NAKLON_MS = constrain(server.arg("pn").toInt(), 0, 60000);
  const uint32_t ya=server.hasArg("na") ? max(1L,server.arg("na").toInt()) : NAKLON_ACCELERATION;
  const uint32_t ys=server.hasArg("ns") ? max(1L,server.arg("ns").toInt()) : NAKLON_MAX_SPEED;
  const uint32_t za=server.hasArg("pa") ? max(1L,server.arg("pa").toInt()) : POVOROT_ACCELERATION;
  const uint32_t zs=server.hasArg("ps") ? max(1L,server.arg("ps").toInt()) : POVOROT_MAX_SPEED;
  if((ya!=NAKLON_ACCELERATION || ys!=NAKLON_MAX_SPEED || za!=POVOROT_ACCELERATION || zs!=POVOROT_MAX_SPEED) &&
      !persistTmcFile(uartNaklon,uartPovorot,ya,ys,za,zs)) {
    server.send(507,"text/plain; charset=utf-8","Не удалось сохранить файл UART и параметры движения");return;
  }
  NAKLON_ACCELERATION=ya;NAKLON_MAX_SPEED=ys;POVOROT_ACCELERATION=za;POVOROT_MAX_SPEED=zs;
  if (server.hasArg("max"))
    MAX_ACTION_TIME_SEC = max(1L, server.arg("max").toInt());
  if (server.hasArg("tz")) {
    String zone = server.arg("tz");
    zone.trim();
    if (zone.length() && zone.length() <= 100) {
      TZ_INFO = zone;
      applyTimezone();
    }
  }
  if (server.hasArg("ntpPeriodMin")) {
    uint32_t minutes = constrain(server.arg("ntpPeriodMin").toInt(), 1, 10080);
    NTP_SYNC_PERIOD_SEC = minutes * 60UL;
  }
  if (server.hasArg("ntpServer1")) {
    String value = server.arg("ntpServer1");
    value.trim();
    if (value.length() && value.length() <= 120 && value.indexOf(' ') < 0)
      NTP_SERVER_1 = value;
  }
  if (server.hasArg("ntpServer2")) {
    String value = server.arg("ntpServer2");
    value.trim();
    if (value.length() && value.length() <= 120 && value.indexOf(' ') < 0)
      NTP_SERVER_2 = value;
  }

  if(!saveSerialOutput(nextSerialOutput)) {server.send(507,"text/plain; charset=utf-8","Не удалось сохранить настройку Serial");return;}
  prefs.putUInt("webPeriod", WEB_REQUEST_PERIOD_MS);
  prefs.putUInt("webRefresh", WEB_PAGE_REFRESH_PERIOD_MS);
  prefs.putUInt("pauseNak", PAUSE_AFTER_NAKLON_MS);
  prefs.putUInt("nakAcc", NAKLON_ACCELERATION);
  prefs.putUInt("nakSpd", NAKLON_MAX_SPEED);
  prefs.putUInt("povAcc", POVOROT_ACCELERATION);
  prefs.putUInt("povSpd", POVOROT_MAX_SPEED);
  prefs.putUInt("maxAct", MAX_ACTION_TIME_SEC);
  prefs.putString("tz", TZ_INFO);
  prefs.putUInt("ntpPeriod", NTP_SYNC_PERIOD_SEC);
  prefs.putString("ntpServer1", NTP_SERVER_1);
  prefs.putString("ntpServer2", NTP_SERVER_2);

  motor_Naklon.setAcceleration(NAKLON_ACCELERATION);
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  motor_Povorot.setAcceleration(POVOROT_ACCELERATION);
  motor_Povorot.setMaxSpeed(POVOROT_MAX_SPEED);
  oledMarkDirty();

  redirectHome();
}

// ============================================================
//                           СЕТЬ
// ============================================================

void loadWiFiSettings() {
  savedSSID = prefs.getString("ssid", "");
  savedPassword = prefs.getString("pass", "");
  homeWiFiEnabled = prefs.getBool("homeEn", true);
  apSSID = prefs.getString("apssid", "RemuageVRN");
  apPassword = prefs.getString("appass", "43214321");

  if (apSSID.length() == 0)
    apSSID = "RemuageVRN";
  if (apPassword.length() < 8 || apPassword.length() > 63)
    apPassword = "43214321";
}

void startAP() {
  // AP ВСЕГДА работает параллельно с попыткой подключения к домашней Wi-Fi.
  // Поэтому при отсутствии/потере домашней сети доступ к МК через AP сохраняется.
  WiFi.mode(WIFI_AP_STA);
  delay(50);

  WiFi.softAPdisconnect(true);
  delay(50);

  bool ok = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  if (ok) {
    // По умолчанию SoftAP ESP32 отключает клиента после 300 с без
    // принятых от него данных. Для контроллера это слишком мало:
    // телефон/ПК может просто лежать без активности.
    esp_err_t inactiveResult = esp_wifi_set_inactive_time(WIFI_IF_AP, 65535);
    DBGLN(String("AP: ") + apSSID + " IP " + WiFi.softAPIP().toString());
    DBGLN(String("AP: inactive timeout = 65535 s, result=") + String((int)inactiveResult));
  } else {
    DBGLN("AP: ошибка запуска точки доступа");
  }
}

void connectHomeWiFi() {
  WiFi.mode(WIFI_AP_STA);

  if (!homeWiFiEnabled) {
    DBGLN("WiFi HOME: отключено пользователем в настройках");
    WiFi.disconnect(false, false);
    return;
  }

  if (!savedSSID.length()) {
    DBGLN("WiFi HOME: SSID не задан, работаем через AP");
    WiFi.disconnect(false, false);
    return;
  }

  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false); // Только подключение STA; точку доступа SoftAP не останавливать.
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  DBGLN(String("WiFi: асинхронное подключение к ") + savedSSID +
        " (AP: " + WiFi.softAPIP().toString() + ")");
}

void handleNetwork() {
  if(!bootstrapWebReady()) {handleBootstrapSetup();return;}
  sendLittleFSFile("/network.html");
}
void handleBootstrapWifiSave() {
  if(!server.hasArg("homeen") || !bootstrapCredentialsValid(server.arg("ssid"),server.arg("pass"))) {
    server.send(422,"text/plain; charset=utf-8","Укажите SSID (1–32 байта) и пароль: пустой для открытой сети, 8–63 байта либо 64 шестнадцатеричных символа");return;
  }
  handleNetworkSave();
}

void handleNetworkSave() {
  if (programRunning || motorNaklonMoving || motorPovorotMoving || motor_Naklon.getState() ||
      motor_Povorot.getState()) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите движение");
    return;
  }
  String oldApSSID = apSSID;
  String oldApPassword = apPassword;
  homeWiFiEnabled = server.hasArg("homeen");
  savedSSID = server.hasArg("ssid") ? server.arg("ssid") : "";
  savedPassword = server.hasArg("pass") ? server.arg("pass") : "";

  if (server.hasArg("apssid")) {
    String v = server.arg("apssid");
    v.trim();
    if (v.length() > 0 && v.length() <= 32)
      apSSID = v;
  }

  if (server.hasArg("appass")) {
    String v = server.arg("appass");
    if (v.length() >= 8 && v.length() <= 63)
      apPassword = v;
  }

  prefs.putBool("homeEn", homeWiFiEnabled);
  prefs.putString("ssid", savedSSID);
  prefs.putString("pass", savedPassword);
  prefs.putString("apssid", apSSID);
  prefs.putString("appass", apPassword);

  bool apChanged = (oldApSSID != apSSID || oldApPassword != apPassword);
  if (apChanged) {
    // При изменении AP клиент будет отключён неизбежно — это смена параметров точки доступа.
    startAP();
  }
  if (homeWiFiEnabled)
    connectHomeWiFi();
  else {
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    DBGLN("WiFi HOME: отключено пользователем, AP продолжает работать");
  }
  redirectHome();
}

void handleNetworkNTP() {
  if ((programRunning && !recoveryRequired) || motorNaklonMoving || motorPovorotMoving ||
      motor_Naklon.getState() || motor_Povorot.getState()) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите движение");
    return;
  }
  syncTimeNTP(true);
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain; charset=utf-8", "NTP недоступен. Установите время из браузера.");
    return;
  }
  server.sendHeader("Location", "/network");
  server.send(303);
}

void handleNetworkScan() {
  if (programRunning) {
    server.send(409, "text/plain; charset=utf-8", "Сначала остановите программу");
    return;
  }
  int n = WiFi.scanNetworks();
  String json = "{\"ok\":" + String(n >= 0 ? "true" : "false") + ",\"networks\":[";
  for (int i = 0; i < n; ++i) {
    if (i)
      json += ',';
    json += "{\"ssid\":" + editorJsonQuote(WiFi.SSID(i)) + ",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"channel\":" + String(WiFi.channel(i)) +
            ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") +
            "}";
  }
  WiFi.scanDelete();
  server.send(n >= 0 ? 200 : 503, "application/json; charset=utf-8", json + "]}");
}

// ============================================================
// АСИНХРОННЫЙ ФАЙЛОВЫЙ МЕНЕДЖЕР LITTLEFS, ПОРТ 8080
// ============================================================
// На ОБОИХ портах используется ESPAsyncWebServer. Нет зависимости от синхронного WebServer
// или вызова handleClient() для порта 8080. Интерфейс читается из редактируемых файлов.
// Смените пароль перед подключением устройства к недоверенной сети.
#define REMUATOR_FILES_USER "admin"
#define REMUATOR_FILES_PASSWORD "2026"

// Редактируемые HTML, CSS и JavaScript отдаются из корня LittleFS.

bool fmAuthenticated(AsyncWebServerRequest *r) {
  if (r->authenticate(REMUATOR_FILES_USER, REMUATOR_FILES_PASSWORD))
    return true;
  r->requestAuthentication();
  return false;
}

bool fmValidPath(const String &p) {
  return p.startsWith("/") && p.indexOf("..") < 0 && p.indexOf('\\') < 0 && p.indexOf(':') < 0 &&
         p.indexOf('?') < 0 && p.indexOf('#') < 0 && p.indexOf("//") < 0 && p != "/state" &&
         !p.startsWith("/state/") && p.length() <= 128;
}

String fmJson(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if ((uint8_t)c < 0x20)
      out += ' ';
    else
      out += c;
  }
  return out;
}

String fmArg(AsyncWebServerRequest *r, const char *key, bool post) {
  const AsyncWebParameter *p = r->getParam(key, post);
  return p ? p->value() : String();
}

struct FmUpload {
  File file;
  String path;
  bool ok = false;
};

void serveBootstrapFileManager(AsyncWebServerRequest *r,bool forceMemory) {
  if(stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
  ControlGuard guard(r);if(!guard)return;
  if(!fmAuthenticated(r)) return;
  AsyncWebServerResponse *response;
  if(forceMemory || !bootstrapManagerReady() || !bootstrapWebReady())
    response=r->beginResponse(200,"text/html; charset=utf-8",BOOTSTRAP_MANAGER_HTML);
  else response=r->beginResponse(LittleFS,"/filemanager.html","text/html; charset=utf-8",false);
  response->addHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
  response->addHeader("Pragma","no-cache");response->addHeader("Expires","0");r->send(response);
}
void setupAsyncFileManager() {
  filesServer.on("/",HTTP_GET,[](AsyncWebServerRequest *r){serveBootstrapFileManager(r,false);});
  filesServer.on("/bootstrap",HTTP_GET,[](AsyncWebServerRequest *r){serveBootstrapFileManager(r,true);});
  filesServer.on("/setup/status",HTTP_GET,[](AsyncWebServerRequest *r){
    if(stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Сначала остановите движение");return;}
    ControlGuard guard(r);if(!guard)return;if(!fmAuthenticated(r))return;
    auto *response=r->beginResponse(200,"application/json; charset=utf-8",bootstrapStatusJson());
    response->addHeader("Cache-Control","no-store");r->send(response);
  });
  filesServer.on("/api/version",HTTP_GET,[](AsyncWebServerRequest *r){
    r->send(200,"application/json; charset=utf-8",String("{\"version\":\"")+REMUATOR_VERSION+"\"}");
  });
  for (const char *asset : {"/filemanager.css", "/filemanager.js", "/style.css", "/site-shell.js"}) {
    filesServer.on(asset, HTTP_GET, [](AsyncWebServerRequest *r) {
      if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
      if (!fmAuthenticated(r))
        return;
      if (!LittleFS.exists(r->url())) {
        r->send(404, "text/plain", "Asset missing");
        return;
      }
      AsyncWebServerResponse *response =
          r->beginResponse(LittleFS, r->url(), contentTypeForPath(r->url()), false);
      response->addHeader("Cache-Control", "no-store");
      r->send(response);
    });
  }
  // Менеджер использует отдельный источник :8080, поэтому значку сайта нужен собственный маршрут.
  filesServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *r) {
    ControlGuard guard(r);if(!guard)return;
    // Значок общедоступен: запрос Basic Auth может помешать браузеру
    // показать его для второго HTTP-порта.
    if (!LittleFS.exists("/favicon.ico")) {
      r->send(404, "text/plain", "Icon not found");
      return;
    }
    AsyncWebServerResponse *response =
        r->beginResponse(LittleFS, "/favicon.ico", "image/x-icon", false);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    r->send(response);
  });
  filesServer.on("/api/list", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String dir = fmArg(r, "dir");
    if (!fmValidPath(dir)) {
      r->send(400, "text/plain", "Bad folder");
      return;
    }
    File folder = LittleFS.open(dir, "r");
    if (!folder || !folder.isDirectory()) {
      r->send(404, "text/plain", "Folder not found");
      return;
    }
    String result = "{\"total\":" + String(LittleFS.totalBytes()) +
                    ",\"used\":" + String(LittleFS.usedBytes()) + ",\"entries\":[";
    bool first = true;
    File f = folder.openNextFile();
    while (f) {
      String path = String(f.path()), name = path.substring(path.lastIndexOf('/') + 1);
      if (path == "/state" || path.startsWith("/state/")) {
        f = folder.openNextFile();
        continue;
      }
      if (!first)
        result += ',';
      first = false;
      result += "{\"name\":\"" + fmJson(name) +
                "\",\"directory\":" + (f.isDirectory() ? "true" : "false") +
                ",\"size\":" + String(f.size()) + "}";
      f = folder.openNextFile();
    }
    folder.close();
    result += "]}";
    r->send(200, "application/json; charset=utf-8", result);
  });
  filesServer.on("/api/download", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String path = fmArg(r, "path");
    if (!fmValidPath(path) || !LittleFS.exists(path)) {
      r->send(404, "text/plain", "File not found");
      return;
    }
    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) {
      r->send(400, "text/plain", "Not a file");
      return;
    }
    f.close();
    r->send(LittleFS, path, contentTypeForPath(path), true);
  });
  filesServer.on("/api/read", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String path = fmArg(r, "path");
    if (!fmValidPath(path) || !LittleFS.exists(path)) {
      r->send(404, "text/plain", "File not found");
      return;
    }
    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory() || f.size() > 65536) {
      r->send(413, "text/plain", "Not text or larger than 64 KiB");
      return;
    }
    f.close();
    r->send(LittleFS, path, "text/plain; charset=utf-8", false);
  });
  filesServer.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String path = fmArg(r, "path", true), content = fmArg(r, "content", true);
    if (!fmValidPath(path) || path == "/" || content.length() > 65536) {
      r->send(400, "text/plain", "Bad path or size");
      return;
    }
    File f = LittleFS.open(path, "w");
    if (!f) {
      r->send(500, "text/plain", "File open failed");
      return;
    }
    size_t n = f.write((const uint8_t *)content.c_str(), content.length());
    f.close();
    if (n != content.length()) {
      r->send(507, "text/plain", "Storage full/write failed");
      return;
    }
    markProgramsDirty(path);
    r->send(200, "text/plain", "Saved");
  });
  filesServer.on("/api/delete", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String path = fmArg(r, "path");
    if (!fmValidPath(path) || path == "/") {
      r->send(400, "text/plain", "Bad path");
      return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
      r->send(404, "text/plain", "Not found");
      return;
    }
    bool directory = f.isDirectory();
    f.close();
    bool ok = directory ? LittleFS.rmdir(path) : LittleFS.remove(path);
    if (ok)
      markProgramsDirty(path);
    r->send(ok ? 200 : 500, "text/plain", ok ? "Deleted" : "Cannot delete (folder must be empty)");
  });
  filesServer.on("/api/rename", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String from = fmArg(r, "from"), to = fmArg(r, "to");
    if (!fmValidPath(from) || !fmValidPath(to) || from == "/" || to == "/" || LittleFS.exists(to)) {
      r->send(400, "text/plain", "Bad source/target or target already exists");
      return;
    }
    bool ok = LittleFS.rename(from, to);
    if (ok) {
      markProgramsDirty(from);
      markProgramsDirty(to);
    }
    r->send(ok ? 200 : 500, "text/plain", ok ? "Renamed" : "Rename failed");
  });
  filesServer.on("/api/mkdir", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);if(!guard)return;
    if (!fmAuthenticated(r))
      return;
    String path = fmArg(r, "path");
    if (!fmValidPath(path) || path == "/" || LittleFS.exists(path)) {
      r->send(400, "text/plain", "Bad path/existing path");
      return;
    }
    bool ok = LittleFS.mkdir(path);
    if (ok)
      markProgramsDirty(path);
    r->send(ok ? 200 : 500, "text/plain", ok ? "Directory created" : "Cannot create directory");
  });
  filesServer.on(
      "/api/upload", HTTP_POST,
      [](AsyncWebServerRequest *r) {
        if (stepMotionPending()) {r->send(423,"text/plain; charset=utf-8","Движение: файловые операции запрещены");return;}
    ControlGuard guard(r);
        if(!guard) {auto *ctx=static_cast<FmUpload *>(r->_tempObject);if(ctx){delete ctx;r->_tempObject=nullptr;}return;}
        if (!fmAuthenticated(r))
          return;
        FmUpload *ctx = static_cast<FmUpload *>(r->_tempObject);
        bool ok = ctx && ctx->ok;
        if (ok && ctx)
          markProgramsDirty(ctx->path);
        if (ctx) {
          if (ctx->file)
            ctx->file.close();
          delete ctx;
          r->_tempObject = nullptr;
        }
        r->send(ok ? 200 : 500, "text/plain", ok ? "File uploaded" : "Upload failed");
      },
      [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *bytes, size_t len,
         bool final) {
        if(stepMotionPending()) return; // Окончательный обработчик POST возвращает 423.
        ControlGuard guard(r,false);
        if(!guard) {auto *ctx=static_cast<FmUpload *>(r->_tempObject);if(ctx)ctx->ok=false;return;}
        if (!r->authenticate(REMUATOR_FILES_USER, REMUATOR_FILES_PASSWORD))
          return;
        if (index == 0) {
          // Пути из браузера и имена загружаемых файлов не считаются доверенными.
          filename.replace("\\", "/");
          filename = filename.substring(filename.lastIndexOf('/') + 1);
          String dir = fmArg(r, "dir");
          FmUpload *ctx = new FmUpload();
          r->_tempObject = ctx;
          if (!fmValidPath(dir) || filename.length() == 0 || filename == "." || filename == ".." ||
              filename.indexOf(':') >= 0 || filename.indexOf('?') >= 0 ||
              filename.indexOf('#') >= 0)
            return;
          ctx->path = (dir == "/" ? "" : dir) + "/" + filename;
          if (!fmValidPath(ctx->path))
            return;
          ctx->file = LittleFS.open(ctx->path, "w");
          ctx->ok = (bool)ctx->file;
        }
        FmUpload *ctx = static_cast<FmUpload *>(r->_tempObject);
        if (!ctx || !ctx->ok)
          return;
        if (len && ctx->file.write(bytes, len) != len)
          ctx->ok = false;
        if (final && ctx->file)
          ctx->file.close();
      });
  filesServer.begin();
  filesServerReady = true;
  DBGLN(String("LittleFS async manager AP: http://") + WiFi.softAPIP().toString() + ":8080/");
  if (WiFi.status() == WL_CONNECTED)
    DBGLN(String("LittleFS async manager STA: http://") + WiFi.localIP().toString() + ":8080/");
}

// ============================================================
//                       ВЕБ-РЕГИСТРАЦИЯ
// ============================================================

String bootstrapStatusJson() {
  String missing="[";bool first=true;
  for(const char *path:BOOTSTRAP_REQUIRED_FILES) if(!bootstrapFsMounted || !bootstrapAssetPresent(path)) {
    if(!first)missing+=',';first=false;missing+='"';missing+=path;missing+='"';
  }
  missing+=']';
  const bool sta=WiFi.status()==WL_CONNECTED;
  return String("{\"webReady\":")+(first?"true":"false")+
    ",\"fsMounted\":"+(bootstrapFsMounted?"true":"false")+
    ",\"staConnected\":"+(sta?"true":"false")+
    ",\"sta\":\""+(sta?WiFi.localIP().toString():String(""))+"\",\"ap\":\""+
    WiFi.softAPIP().toString()+"\",\"missing\":"+missing+"}";
}
void handleNetworkStatus() {
  bool sta = (WiFi.status() == WL_CONNECTED);
  String json = String("{\"ap\":\"") + WiFi.softAPIP().toString() + "\",\"sta\":\"" +
                (sta ? WiFi.localIP().toString() : String("")) +
                "\",\"staConnected\":" + (sta ? "true" : "false") + "}";
  server.send(200, "application/json; charset=utf-8", json);
}

#include "calibration_z.h"
#include "web_api.h"
#include "calibration_web.h"


void stopMotorTest() {
  motor_Naklon.brake(); motor_Povorot.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  motor_Povorot.setTarget(motor_Povorot.getCurrent());
  motorNaklonMoving=motorPovorotMoving=false;
  serialMoveNaklonActive=serialMovePovorotActive=false;
  motorTestActive=false;
  releaseDrivers();
}
void handleMotorTest() {
  String action=server.arg("action");
  if(action=="heartbeat") {
    if(motorTestActive) motorTestHeartbeat=millis();
  } else if(action=="stop") {
    if(motorTestActive) stopMotorTest();
  } else {
    if(programRunning || homingActive || calibrationBusy() || zCalStarted || stepMotionPending() || rebootAt) {
      server.send(409,"text/plain; charset=utf-8","Сначала остановите программу и все движения");return;
    }
    bool y=server.arg("motor")=="nak";
    if(!y && server.arg("motor")!="pov") {server.send(422,"text/plain","UNKNOWN_MOTOR");return;}
    int direction=action=="plus"?1:action=="minus"?-1:0;
    if(!direction || (y && !canMoveY(direction))) {server.send(409,"text/plain; charset=utf-8","Проверьте направление и концевики Y");return;}
    if(!(y ? tmcNaklon.settingsApplied : tmcPovorot.settingsApplied)) {
      server.send(409,"text/plain; charset=utf-8","Сначала сохраните и подтвердите настройки UART");return;
    }
    auto &motor=y?motor_Naklon:motor_Povorot;
    int64_t target=(int64_t)motor.getCurrent()+direction*1000000LL;
    if(target>INT32_MAX || target<INT32_MIN) {server.send(422,"text/plain","STEP_RANGE");return;}
    motorTestActive=true;motorTestHeartbeat=motorTestStarted=millis();
    if(y) {moveDirectionNaklon=direction; motorNaklonMoving=true;}
    else {moveDirectionPovorot=direction; motorPovorotMoving=true;}
    enableDrivers(); motor.setTarget((int32_t)target);
  }
  server.send(200,"application/json","{\"ok\":true}");
}
void handleReboot() {
  if (rebootAt) {server.send(200,"application/json","{\"ok\":true}");return;}
  if(calibrationBusy()) calStopWithError("Калибровка прервана перезагрузкой");
  if(zCalStarted) {zCalibrationStopMotion();zCalStarted=false;}
  if(motorTestActive) stopMotorTest();
  stopProgram();
  disableDrivers();
  if(!saveExecution()) {server.send(503,"text/plain; charset=utf-8","Не удалось сохранить состояние перед перезагрузкой; проверьте LittleFS");return;}
  rebootAt=millis()+1000;
  server.send(200,"application/json","{\"ok\":true}");
}


void handleReferenceAction() {
  if(stepMotionPending() || (programRunning && !(programPaused && recoveryRequired))) {
    server.send(409,"text/plain; charset=utf-8","Сначала остановите движение и программу");return;
  }
  const String action=server.arg("action");
  if(action=="zeroZ") {
    if(programRunning) {server.send(409,"text/plain; charset=utf-8","Остановите программу перед заданием нуля Z");return;}
    zCalStarted=false;zCalJogging=false;
    motor_Povorot.brake();motor_Povorot.setCurrent(0);motor_Povorot.setTarget(0);
    zCalStepSnapshot.store(0);if(!saveExecution()){server.send(503,"text/plain",executionMessage);return;}
  } else if(action=="apply" || action=="applyY" || action=="applyZ") {
    const bool useY=action!="applyZ",useZ=action!="applyY";
    CalBlob stored;
    if(useY)calLoadXFile();
    if(useY && (!calLoadFile(stored) || stored.yAxis!=calYAxis || stored.xAxis!=calXAxis)) {
      server.send(422,"text/plain; charset=utf-8","Нет совместимой сохранённой таблицы Y");return;
    }
    if(useZ && !zCalibrationValid()) {
      File f=LittleFS.open("/calibration_z.csv","r");
      if(!f){server.send(422,"text/plain; charset=utf-8","Нет файла калибровки Z");return;}
      String header=f.readStringUntil('\n'),line=f.readStringUntil('\n');f.close();header.trim();line.trim();
      unsigned micro=0;long steps=0;char tail=0;
      if(header!="microsteps,stepsPer360" || sscanf(line.c_str(),"%u,%ld%c",&micro,&steps,&tail)!=2 ||
         micro!=uartPovorot.microsteps || steps<100 || steps>Z_CAL_MAX_ABS_STEPS) {
        server.send(422,"text/plain; charset=utf-8","Файл Z повреждён или микрошаг не совпадает");return;
      }
      const uint32_t digest=zCalibrationDigest(steps,micro);
      prefs.putBool("zCalValid",false);
      prefs.putInt("zCalSteps",steps);prefs.putUShort("zCalMicro",micro);prefs.putUInt("zCalHash",digest);prefs.putBool("zCalValid",true);
      zCalibrationLoad();
      if(!zCalibrationValid()){server.send(503,"text/plain; charset=utf-8","Не удалось применить калибровку Z");return;}
    }
    if(useY) {
      calActive=stored;calTableValid=true;calPhase=CAL_IDLE;
      prefs.putBytes("yCalBlob",&calActive,sizeof(calActive));
      if(calSample(true) && freshMeasuredY()) {motor_Naklon.setCurrent(calibratedAngleToSteps(calAngle));motor_Naklon.setTarget(motor_Naklon.getCurrent());}
    }
  } else {server.send(422,"text/plain","UNKNOWN_ACTION");return;}
  server.send(200,"application/json","{\"ok\":true}");
}
void handleLive() {
  const bool actual=freshMeasuredY();
  String json=String("{\"ok\":true,\"bootId\":")+String(controllerBootId)+",\"cycle\":"+String(controllerCycle)+
    ",\"epoch\":"+String((long long)nowEpoch())+",\"timeValid\":"+(timeValid()?"true":"false")+
    ",\"running\":"+(programRunning?"true":"false")+",\"paused\":"+(programPaused?"true":"false")+
    ",\"name\":"+editorJsonQuote(programRunning?String(runProgram.name):String("Программа"))+
    ",\"moving\":"+(stepMotionPending()?"true":"false")+",\"recoveryRequired\":"+(recoveryRequired?"true":"false")+
    ",\"autoResume\":"+(recoveryAutoPending?"true":"false")+",\"currentStep\":"+String(currentProgramStep+1)+
    ",\"actualY\":"+(actual?String(calAngle,1):String("null"))+
    ",\"yBySteps\":true,\"sensorRecovering\":false"+
    ",\"actualX\":"+(calXFresh()?String(calCurrentX,1):String("null"))+
    ",\"xDeviation\":"+(calXFresh() && calXStored?String(fabsf(calCurrentX-calRefX),1):String("null"))+
    ",\"xTolerance\":"+String(calToleranceX,1)+",\"xRunTolerance\":"+String(calRunToleranceX,1)+
    ",\"xTripped\":"+(xProtectionTripped?"true":"false")+
    ",\"estimatedY\":"+String(calibratedStepsToAngle(motor_Naklon.getCurrent()),2)+
    ",\"targetY\":"+String(programRunning?runProgram.steps[currentProgramStep].angleNaklon:stepsToAngleNaklon(motor_Naklon.getTarget()),2)+
    ",\"naklonDone\":"+(execState.naklonCompleted?"true":"false")+",\"rotationDone\":"+(execState.povorotCompleted?"true":"false")+
    ",\"homing\":"+(homingActive?"true":"false")+",\"message\":"+editorJsonQuote(executionMessage)+"}";
  if(programRunning && currentProgramStep<runProgram.count) {
    const ProgramStep &row=runProgram.steps[currentProgramStep];
    json.remove(json.length()-1);
    json+=",\"totalRows\":"+String(runProgram.count)+",\"currentRow\":{\"number\":"+String(currentProgramStep+1)+
      ",\"start\":"+String((long long)scheduledEpoch[currentProgramStep])+
      ",\"incY\":"+String(row.incAngleCenti/100.0f,2)+",\"y\":"+String(row.angleNaklon,2)+
      ",\"incRot\":"+String(row.incRot)+",\"rot\":"+String(row.anglePovorot,1)+
      ",\"orientation\":"+String(programOrientationParts(row.rotationParts)*22.5f,1)+
      ",\"pause\":"+editorJsonQuote(programPauseText(row.pauseSeconds).c_str())+"}}";
  }
  server.send(200,"application/json; charset=utf-8",json);
}

void setupWebServer() {
  server.enableOtaStatus();
  // Инструкция передаётся из Flash без LittleFS и мьютекса управления, в том числе при движении.
  server.onMemoryPage("/help",USER_GUIDE_HTML);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/programs", HTTP_GET, handlePrograms);
  server.on("/program/select", HTTP_GET, handleProgramSelect);
  server.on("/program/restore", HTTP_GET, handleProgramRestore);
  server.on("/program/stop", HTTP_GET, handleProgramStop);
  server.on("/program/edit", HTTP_GET, handleProgramEdit);
  server.on("/edit", HTTP_GET, handleProgramEdit);
  server.on("/api/editor", HTTP_GET, handleEditorGet);
  server.on("/api/editor/update", HTTP_POST, handleEditorUpdate);
  server.on("/api/editor/save", HTTP_POST, handleEditorSave);
  server.on("/api/editor/discard", HTTP_POST, handleEditorDiscard);
  server.on("/api/editor/download", HTTP_POST, handleEditorDownload);
  server.on("/api/version", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8",
                String("{\"version\":\"") + REMUATOR_VERSION + "\"}");
  });
  server.on("/api/home", HTTP_GET, handleApiHome);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/motor-test", HTTP_POST, handleMotorTest);
  server.on("/settings/serial", HTTP_GET, [](){sendLittleFSFile("/serial.html");});
  server.on("/api/serial/settings", HTTP_GET, handleSerialSettings);
  server.on("/api/serial/settings", HTTP_POST, handleSerialSettingsSave);
  server.on("/api/serial", HTTP_GET, [](){server.send(200,"text/plain; charset=utf-8",captureStartupSerial.load() ? "Запуск ещё выполняется. Нажмите Обновить после завершения." : startupSerial);});
  server.on("/api/live",HTTP_GET,handleLive);
  server.on("/api/reference",HTTP_POST,handleReferenceAction);
  server.on("/api/settings", HTTP_GET, handleApiSettings);
  server.on("/api/settings/uart", HTTP_GET, handleApiUart);
  server.on("/api/settings/uart/diagnostics", HTTP_GET, handleApiUartDiagnostics);
  server.on("/api/network", HTTP_GET, handleApiNetwork);
  server.on("/api/time", HTTP_POST, handleApiTime);
  server.on("/api/program", HTTP_POST, handleApiProgram);
  server.on("/api/program", HTTP_GET, handleApiProgram);
  server.on("/api/power", HTTP_GET, handleApiPower);
  server.on("/api/manual", HTTP_GET, handleApiManual);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings/calibration-y", HTTP_GET, handleCalibrationPage);
  server.on("/settings/calibration-z", HTTP_GET, handleZCalibrationPage);
  server.on("/api/calibration/z", HTTP_GET, handleZCalibrationStatus);
  server.on("/api/calibration/z", HTTP_POST, handleZCalibrationAction);
  server.on("/api/calibration/y", HTTP_GET, handleCalibrationStatus);
  server.on("/api/calibration/y", HTTP_POST, handleCalibrationAction);
  server.on("/settings/about", HTTP_GET, [](){sendLittleFSFile("/about.html");});
  server.on("/api/readme", HTTP_GET, handleReadme);
  server.on("/settings/save", HTTP_POST, handleSettingsSave);
  server.on("/settings/uart", HTTP_GET, handleUartSettings);
  server.on("/settings/uart/save", HTTP_POST, handleUartSettingsSave);

  server.on("/setup",HTTP_GET,handleBootstrapSetup);
  server.on("/setup/wifi",HTTP_POST,handleBootstrapWifiSave);
  server.on("/setup/status",HTTP_GET,[](){server.send(200,"application/json; charset=utf-8",bootstrapStatusJson());});
  server.on("/network", HTTP_GET, handleNetwork);
  server.on("/api/network/status", HTTP_GET, handleNetworkStatus);
  server.on("/network/save", HTTP_POST, handleNetworkSave);
  server.on("/network/scan", HTTP_GET, handleNetworkScan);
  server.on("/network/ntp", HTTP_GET, handleNetworkNTP);

  server.on("/index.html", HTTP_GET, handleStaticIndex);
  server.on("/style.css", HTTP_GET, handleStaticCSS);
  server.on("/app.js", HTTP_GET, handleStaticJS);
  server.on("/favicon.ico", HTTP_GET, handleStaticIcon);
  server.onNotFound(handleStaticFile);

  server.begin();
  DBGLN("ESPAsyncWebServer запущен на порту 80");

  setupAsyncFileManager();
  DBGLN(String("HTTP: AP http://") + WiFi.softAPIP().toString() + "/ | files :8080/");
  if (WiFi.status() == WL_CONNECTED)
    DBGLN(String("HTTP: STA http://") + WiFi.localIP().toString() + "/ | files :8080/");
  DBGLN("HTTP server запущен");
}

// ============================================================
// ОБНОВЛЕНИЕ ПРОШИВКИ ПО WI-FI ЧЕРЕЗ ARDUINO OTA
// ============================================================

const char *otaResetReasonText() {
  switch(esp_reset_reason()) {
  case ESP_RST_POWERON:return "power_on";
  case ESP_RST_SW:return "software_reset";
  case ESP_RST_PANIC:return "panic";
  case ESP_RST_INT_WDT:return "interrupt_watchdog";
  case ESP_RST_TASK_WDT:return "task_watchdog";
  case ESP_RST_WDT:return "watchdog";
  case ESP_RST_BROWNOUT:return "brownout";
  default:return "other";
  }
}
String otaStatusJson() {
  const esp_partition_t *running=esp_ota_get_running_partition();
  const esp_partition_t *next=esp_ota_get_next_update_partition(nullptr);
  return String("{\"ok\":true,\"enabled\":")+(otaServiceEnabled?"true":"false")+
    ",\"uploading\":"+(otaInProgress.load()?"true":"false")+
    ",\"complete\":"+(otaTransferComplete.load()?"true":"false")+
    ",\"received\":"+String(otaTransferred.load())+",\"total\":"+String(otaTotal.load())+
    ",\"otaError\":"+String(otaLastError.load())+",\"flashError\":"+String(otaFlashError.load())+
    ",\"resetReason\":\""+otaResetReasonText()+"\",\"resetReasonCode\":"+String(int(esp_reset_reason()))+
    ",\"runningSlotBytes\":"+String(running?running->size:0)+
    ",\"nextSlotBytes\":"+String(next?next->size:0)+"}";
}
void setupArduinoOTA() {
  // Проверяем наличие заданного пароля, без ограничения на прежнюю длину-заглушку.
  if (strlen(REMUATOR_OTA_PASSWORD) == 0) {
    DBGLN("OTA отключено: пароль не задан.");
    return;
  }

  // Одной таблицы разделов для OTA недостаточно: нужны два раздела приложения.
  const esp_partition_t *slot0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t *slot1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!slot0 || !slot1) {
    DBGLN("OTA disabled: ota_0/ota_1 partitions missing; keep the existing partitions.csv.");
    return;
  }
  DBGLN(String("OTA: slot0=") + String(slot0->size / 1024) +
        " KiB slot1=" + String(slot1->size / 1024) + " KiB");
  const esp_partition_t *current = esp_ota_get_running_partition();
  if (current)
    DBGLN(String("OTA: running from ") + current->label);

  DBGLN(String("Reset reason: ")+otaResetReasonText()+" ("+String(int(esp_reset_reason()))+")");
  ArduinoOTA.setTimeout(5000); // Допускаем короткие провалы доставки в Wi-Fi, не меняя протокол OTA.
  ArduinoOTA.setHostname("remuator-s3");
  ArduinoOTA.setPassword(REMUATOR_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    // Обработчик вызывается только при остановленной программе и обоих двигателях.
    otaInProgress = true;
    otaTransferred=0;otaTotal=0;otaLastError=-1;otaFlashError=0;otaTransferComplete=false;
    disableDrivers();
    otaSavedSleep=WiFi.getSleep();otaSleepChanged=WiFi.setSleep(false);
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      LittleFS.end(); // Перед обновлением файловой системы она размонтируется.
      DBGLN("OTA: filesystem update started; LittleFS unmounted");
    } else {
      DBGLN("OTA: firmware update started; motors disabled");
    }
  });
  ArduinoOTA.onProgress([](unsigned int received,unsigned int total) {otaTransferred=received;otaTotal=total;});
  ArduinoOTA.onEnd([]() {otaTransferComplete=true;DBGLN("OTA: update complete; restarting");});
  ArduinoOTA.onError([](ota_error_t error) {
    otaLastError=int(error);otaFlashError=Update.getError();
    DBGLN(String("OTA ERROR: ")+String(int(error))+"; Flash: "+String(Update.getError())+" "+Update.errorString());
    // После ошибки восстанавливается доступ: обновление OTA могло размонтировать LittleFS.
    if (otaInProgress.load() && ArduinoOTA.getCommand() == U_SPIFFS)
      mountProgramFS();
    if(otaSleepChanged) {WiFi.setSleep(otaSavedSleep);otaSleepChanged=false;}
    otaInProgress = false;
  });
  ArduinoOTA.begin();
  otaServiceEnabled = true;
  DBGLN("OTA ready on port 3232. Stop the program/motors before updating.");
}

// ============================================================
//                    ВОССТАНОВЛЕНИЕ
// ============================================================

void finishProgram() {
  runMeta.finishedEpoch = nowEpoch();
  runMeta.noticeVisible = true;
  stopProgram();
  if (!recoveryMotionUncertain) {
    appendRunLog("FINISH", "Ремюаж окончен; последняя выдержка завершена", runMeta.finishedEpoch,
                 runProgram.count);
    executionMessage = "";
  }
}

void logRecoveryPower() {
  if (recoveryPowerLogged || !timeValid())
    return;
  runMeta.powerOnEpoch = nowEpoch() - (int64_t)((uint64_t)esp_timer_get_time() / 1000000ULL);
  if (!ensureRunLog() || !saveExecution())
    return;
  appendRunLog("POWER_LOSS",
               "Время приблизительное: последняя подтверждённая запись перед отключением",
               runMeta.outageEpoch);
  appendRunLog(
      "POWER_ON",
      executionJournalDegraded
          ? "Выбрана целая запись; найдена также неполная запись или запись прежнего формата"
          : "Восстановлена последняя целая запись",
      runMeta.powerOnEpoch);
  recoveryPowerLogged = true;
}

bool resumeRecoveredProgram() {
  if (!overallCalibrationReady()) {executionMessage=overallCalibrationReason();return false;}
  if (!programZTargetsSafe(runProgram)) {executionMessage="Координаты Z выходят за допустимый диапазон";return false;}
  if (yEndstopFault)
    return false;
  if (!programRunning || !recoveryRequired || recoveryMotionUncertain || !timeValid())
    return false;
  logRecoveryPower();
  if (recoveryMotionUncertain)
    return false;
  calSample(true);
  if(!freshMeasuredY() && !sensorFallbackY) {executionMessage="Восстановление ожидает ADXL345";return false;}
  // Без датчика доступна только последняя сохранённая координата STEP.
  if(freshMeasuredY()) {
    if(digitalRead(Y_END_PIN)==HIGH && digitalRead(Y_UPPER_END_PIN)==HIGH) return false;
    const int32_t position=digitalRead(Y_END_PIN)==HIGH ? 0 :
      digitalRead(Y_UPPER_END_PIN)==HIGH ? calibratedUpperSteps() : calibratedAngleToSteps(calAngle);
    motor_Naklon.setCurrent(position);
  }
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  yBootReferencePending=false;
  const uint64_t now = (uint64_t)esp_timer_get_time();
  const RecoveryDecision decision = decideRecovery(
      execState.naklonCompleted, execState.povorotCompleted,
      currentProgramStep + 1 >= runProgram.count, scheduledEpoch[currentProgramStep + 1],
      nowEpoch(), runProgram.steps[currentProgramStep].pauseSeconds);
  recoveryRequired = false;
  recoveryAutoPending = false;
  programPaused = false;
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  pauseStopPending = false;
  executionMessage = "";
  if (decision.action == RecoveryAction::Finish) {
    finishProgram();
    return true;
  }
  if (decision.action == RecoveryAction::Next) {
    ++currentProgramStep;
    execState.naklonCompleted = false;
    execState.povorotCompleted = false;
    programState = MOVING_NAKLON;
    beginStepInterval();
  } else {
    stepInterval.start(now, decision.remainingUs);
    if (decision.reanchor)
      scheduledEpoch[currentProgramStep] = nowEpoch();
    if (decision.action == RecoveryAction::Tilt)
      programState = MOVING_NAKLON;
    else if (decision.action == RecoveryAction::Rotate) {
      // Завершённый наклон не повторяется. Оставшаяся выдержка после него сохраняется.
      uint64_t settle = settleInterval.remaining(now);
      if (settle) {
        settleInterval.start(now, settle);
        programState = WAITING_AFTER_NAKLON;
      } else
        programState = MOVING_POVOROT;
    } else
      programState = WAITING_BETWEEN_STEPS;
    rebuildRemainingSchedule();
  }
  if (!saveExecution())
    return false;
  appendRunLog("RESUME", "Продолжение по сохранённым отметкам; расписание пересчитано");
  if (programState == MOVING_NAKLON)
    beginNaklonAction();
  else if (programState == MOVING_POVOROT)
    beginPovorotAction();
  oledMarkDirty();
  return !programPaused;
}

void serviceRecovery() {
  if (!recoveryRequired || !programRunning || recoveryMotionUncertain)
    return;
  logRecoveryPower();
  if(!recoveryAutoPending || !timeValid()) return;
  if(!freshMeasuredY()) calSample(true);
  if(!freshMeasuredY() && !sensorFallbackY) {
    if(!bootSensorWaitMs) bootSensorWaitMs=millis();
    if(uint32_t(millis()-bootSensorWaitMs)<SENSOR_RECOVERY_TIMEOUT_MS) return;
    if(!calibrationValid() || !calXStored || !zCalibrationValid()) return;
    sensorFallbackY=true;
    executionMessage="ADXL345 недоступен после включения: Y по карте шагов от последней записи";
    appendRunLog("Y_STEP_FALLBACK",executionMessage);
  }
  if(!overallCalibrationReady()) return;
  const bool overdue=nowEpoch()>=scheduledEpoch[currentProgramStep+1];
  if(!recoveryCountdownStarted) {
    recoveryCountdownStarted=true;
    recoveryReadyUs=(uint64_t)esp_timer_get_time()+(overdue?RECOVERY_DELAY_US:0);
  }
  if((uint64_t)esp_timer_get_time()>=recoveryReadyUs) resumeRecoveredProgram();
}

void restoreExecutionAfterBoot() {
  if (!loadExecution()) {
    // Сохранённое сообщение о завершении и имя журнала сохраняются и в режиме ожидания.
    if (!executionReadError) {
      resetExecution();
      if(synchronizeYAfterPower()) saveExecution();
    }
    return;
  }
  const uint64_t now = (uint64_t)esp_timer_get_time();
  const int64_t saved = execState.savedEpoch;
  runMeta.outageEpoch = saved;
  runMeta.powerOnEpoch = 0;
  runMeta.noticeVisible = true;
  recoveryPowerLogged = false;
  uint64_t settle = execState.settleRemainingUs;
  if (timeValid() && nowEpoch() >= saved) {
    const uint64_t elapsed = uint64_t(nowEpoch() - saved) * 1000000ULL;
    settle = settle > elapsed ? settle - elapsed : 0;
  }
  stepInterval.start(now, execState.remainingUs);
  stepInterval.pause(now);
  settleInterval.start(now, settle);
  // Выдержка отсчитывается по реальному времени и может закончиться во время двухсекундного ожидания.
  recoveryRequired = true;
  recoveryMotionUncertain = false;
  if(programState==MOVING_POVOROT && execState.motionUncertain) {
    // Признак команды записан до STEP: прерванная команда Z принимается выполненной один раз.
    execState.povorotCompleted=true;
    motor_Povorot.setCurrent(programRotationSteps(runProgram.steps[currentProgramStep].rotationParts,currentZSteps360()));
    motor_Povorot.setTarget(motor_Povorot.getCurrent());
    programState=WAITING_BETWEEN_STEPS;
  }
  recoveryAutoPending = !execState.paused;
  recoveryCountdownStarted=false;
  recoveryReadyUs=now;
  pauseStopPending = false;
  serialMoveNaklonActive = false;
  serialMovePovorotActive = false;
  disableDrivers();
  executionMessage=execState.paused ? "Сохранена ручная пауза" : "Восстановление по журналу; ожидаются часы и измерение Y";
  logRecoveryPower();
}

// ============================================================
// НАСТРОЙКА ПРИ ЗАПУСКЕ
// ============================================================

void setup() {
  controllerBootId=esp_random();
  controlMutex = xSemaphoreCreateRecursiveMutex();
  // Отключить оба драйвера до Serial, OLED, Wi-Fi и загрузки настроек.
  setupDriverEnablePins();
  prefs.begin("remuage", false);
  serialOutputEnabled = prefs.getBool("serialOn", true);
#if SERIAL_DEBUG
  Serial.begin(DEBUG_BAUD);
  delay(200);
  DBGLN("");
  DBGLN("======================================");
  DBG("REmuator ESP32-S3 v");
  DBGLN(REMUATOR_VERSION);
  DBGLN("======================================");
#endif

  // Встроенный RGB LED.
  pinMode(RGB_BUILTIN, OUTPUT);
  rgbLedWrite(RGB_BUILTIN, 255, 0, 0);

  pinMode(Y_END_PIN, INPUT);       // внешний резистор 10 кОм -> +3,3 В
  pinMode(Y_UPPER_END_PIN, INPUT); // NC -> GND, размыкание = HIGH

  // Физические кнопки отключены: GPIO4/5/6 заняты мотором Y.
  // НЕ конфигурируем BTN_* и BTN_SUPPLY_PIN. Код handleButtons() сохранён.

  // OLED и DS3231 используют одну шину I2C.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  oled.init();
  oled.flipV(true);
  oled.flipH(true);
  oledShowStartup();
  oledMarkDirty();

  WiFi.onEvent(onWiFiEvent);

  initializePrograms();
  DBGLN(bootstrapWebReady() ? "Web assets OK" : "Web assets missing: embedded setup and file manager enabled");
  ensureProgramsFile();
  debugLoopStack("after CSV startup");
  if (!LittleFS.exists("/Logs"))
    LittleFS.mkdir("/Logs");

  // Выбор восстановлен по неизменному имени файла из NVS selectedPath.

  setupMotors();
  calibrationInit(); // ADXL345, NVS + резерв /calibration_y.csv

  // Общая UART: по умолчанию Y=TMC2208@0, Z=TMC2209@1.
  initTMC();
  zCalibrationLoad(); // после чтения настроек микрошага TMC


  // Подключение Wi-Fi.
  loadWiFiSettings();
  startAP();
  connectHomeWiFi();

  // При перезагрузке сначала загружаем календарное UTC из автономных часов.
  // Только после этого восстанавливаем расписание из журнала LittleFS.
  applyTimezone();
  restoreTimeFromRtc();

  restoreExecutionAfterBoot();

  setupWebServer();
  setupArduinoOTA();
  debugLoopStack("setup complete");

  DBGLN("Система готова.");
  DBG("Выбрана программа: ");
  DBGLN(programs.empty() ? String("нет CSV") : String(programs[selectedProgram].name));
  DBG("Web period: ");
  DBG(WEB_REQUEST_PERIOD_MS);
  DBGLN(" ms");

  if(!yEndstopFault && !recoveryMotionUncertain) releaseDrivers();
  portENTER_CRITICAL(&startupSerialMux);
  captureStartupSerial = false;
  portEXIT_CRITICAL(&startupSerialMux);
  // Отключённый пример вызова: gpio_viewer.begin();
}

// ============================================================
// ОСНОВНОЙ ЦИКЛ
// ============================================================

void loop() {
  {
    ControlGuard guard;
    ++controllerCycle;
    if(rebootAt) {
      disableDrivers();
      if((int32_t)(millis()-rebootAt)>=0) ESP.restart();
      return;
    }
    if(motorTestActive && ((uint32_t)(millis()-motorTestHeartbeat)>2200 ||
        (uint32_t)(millis()-motorTestStarted)>120000 ||
        (!motor_Naklon.getState() && !motor_Povorot.getState()))) stopMotorTest();
    if(normalDriverPower) releaseDrivers();
    // OTA обслуживается только при остановленной программе и ОБОИХ двигателях.
    // Во время загрузки механика не обслуживается; обработчик начала отключает драйверы.
    if (otaServiceEnabled && !zCalStarted && !calibrationBusy() &&
        (otaInProgress || (!programRunning && !serialMoveNaklonActive && !serialMovePovorotActive &&
                           motor_Naklon.getCurrent() == motor_Naklon.getTarget() &&
                           motor_Povorot.getCurrent() == motor_Povorot.getTarget()))) {
      ArduinoOTA.handle();
      if (otaInProgress) {
        delay(1);
        return;
      }
    }

    // Любая запись NVS или транзакция I2C способна задержать генерацию STEP.
    // Отложим обслуживание RTC/NTP и USB-консоли до полной остановки осей.
    if (!stepMotionPending()) {
      pollTimeSync();
      if(!calI2cLock || xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(25))==pdTRUE) {
        serviceRtcClock();
        if(calI2cLock) xSemaphoreGive(calI2cLock);
      }
    }

    if(recoveryRequired && !stepMotionPending()) serviceRecovery();

    // Концевики проверяются ДО следующего импульса STEP.
    checkEndstops();
    serviceXProtection();
    motor_Naklon.tick();
    motor_Povorot.tick();
    calMotorStepSnapshot.store(motor_Naklon.getCurrent(), std::memory_order_relaxed);
    calSensorBackground.store(motor_Naklon.getState() || motor_Povorot.getState() ||
      motor_Naklon.getCurrent()!=motor_Naklon.getTarget() || motor_Povorot.getCurrent()!=motor_Povorot.getTarget(),std::memory_order_relaxed);
    calibrationService();
    if(!programRunning && !stepMotionPending() && !motorTestActive && calPhase==CAL_IDLE &&
       yBootReferencePending && synchronizeYAfterPower()) saveExecution();
    serviceHoming();
    if (HARDWARE_BUTTONS_ENABLED)
      handleButtons();

#if SERIAL_DEBUG
    if (serialOutputEnabled && !zCalStarted && !stepMotionPending())
      handleSerial(); // включая команды управления; во время движения отложены
#endif

    if (zCalStarted) zCalibrationService();
    zCalStepSnapshot.store(motor_Povorot.getCurrent(), std::memory_order_relaxed);
    if (programRunning) {
      executeProgram();
    } else {
      // Завершение ручного движения из Serial.
      if (!homingActive && serialMoveNaklonActive &&
          motor_Naklon.getCurrent() == motor_Naklon.getTarget()) {
        serialMoveNaklonActive = false;
        releaseDrivers();
      }

      if (serialMovePovorotActive && motor_Povorot.getCurrent() == motor_Povorot.getTarget()) {
        serialMovePovorotActive = false;
        releaseDrivers();
      }
    }

    // Периодическая запись выполняется только в покое: запись Flash не должна прерывать STEP.
    if (programRunning && !stepMotionPending() && !recoveryRequired &&
        !recoveryMotionUncertain &&
        (uint64_t)esp_timer_get_time() - lastCheckpointUs >= 60000000ULL)
      saveExecution();

    // После ручного перемещения или обнуления концевиком сохраняется положение в покое.
    // Одна запись после остановки, а не на каждом импульсе.
    if (!programRunning && !stepMotionPending() &&
        (execState.naklonPosition != motor_Naklon.getCurrent() ||
         execState.povorotPosition != motor_Povorot.getCurrent()))
      saveExecution();

    // Оба порта, 80 и 8080, асинхронны: вызов handleClient() не нужен.

    // В режиме WiFi AP+STA точка доступа SoftAP постоянно включена. Повторное подключение STA
    // выполняется в основном цикле без блокирующего ожидания 12 секунд и только в покое.
    if (!stepMotionPending() && homeWiFiEnabled && savedSSID.length() &&
        WiFi.status() != WL_CONNECTED && !otaInProgress &&
        (!programRunning || recoveryRequired) && millis() - lastStaReconnectMs >= 30000UL) {
      lastStaReconnectMs = millis();
      WiFi.reconnect();
      DBGLN(String("WiFi STA retry; AP IP ") + WiFi.softAPIP().toString());
    }

    // NTP с периодом из настроек; запись в RTC выполняется позже, когда оси стоят.
    if (!stepMotionPending() && homeWiFiEnabled && WiFi.status() == WL_CONNECTED &&
        (!programRunning || recoveryRequired || programPaused || programState == WAITING_BETWEEN_STEPS) &&
        !motor_Naklon.getState() && !motor_Povorot.getState() && !homingActive) {
      time_t t = nowEpoch();
      if (lastNtpSyncEpoch == 0 || t - lastNtpSyncEpoch >= NTP_SYNC_PERIOD_SEC) {
        syncTimeNTP(false);
      }
    }

    // Светодиод и OLED работают с периферией и не должны блокировать STEP.
    if (!stepMotionPending()) {
      setConnectionLED();
      if(!calI2cLock || xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(25))==pdTRUE) {
        oledShowStatus();
        if(calI2cLock) xSemaphoreGive(calI2cLock);
      }
    }

  } // Мьютекс controlMutex освобождается перед передачей процессора асинхронным обработчикам HTTP.
  // GyverStepper.tick() — программный генератор. delay(1) ограничивал
  // его частоту примерно 1000 проходов/с при заданных 6000 шагов/с.
  // vTaskDelay(0) уступает CPU без обязательного ожидания системного тика.
  // При выдержке у верхнего концевика калибровка логически занята, но импульсы STEP
  // не требуются. Уступаем процессор, чтобы работали HTTP-команды остановки, статус и задача ADXL.
  if (motor_Naklon.getCurrent()==motor_Naklon.getTarget() &&
      motor_Povorot.getCurrent()==motor_Povorot.getTarget() &&
      !motor_Naklon.getState() && !motor_Povorot.getState()) delay(1);
  // Во время программной генерации STEP нельзя уступать процессор на целый такт ОС.
}
