#pragma once
// Этот файл должен быть первым подключением #include в скетче .ino.
// Препроцессор Arduino может вставить прототипы функций ПЕРЕД всеми
// объявлениями скетча. Полные определения используемых типов должны находиться здесь.
#include <Arduino.h>
#include "esp_wifi.h"
#include "program_model.h"
#include "recovery_policy.h"

#include <ArduinoOTA.h>
// Для совместимых типов HTTPUpload заголовок WebServer.h нужен до ESPAsyncWebServer.h.
#include <WebServer.h>
#include <ESPAsyncWebServer.h>
#include <GyverOLED.h>
#include <GyverStepper.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <TMCStepper.h>
#include <WiFi.h>
#include <Wire.h>

#include <algorithm>
#include <atomic>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <memory>
#include <sys/time.h>
#include <time.h>
#include <vector>

#include "tmc_config.h"
#include "tmc_axis.h"
#include "nc_endstop.h"
#include "ds3231_clock.h"
