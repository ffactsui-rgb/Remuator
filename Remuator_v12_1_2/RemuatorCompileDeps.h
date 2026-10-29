#pragma once
// Must be the first #include in the .ino.
// Arduino's .ino preprocessor may insert generated prototypes BEFORE all
// declarations in the sketch. Put their complete parameter types here.
#include <Arduino.h>
#include "esp_wifi.h"
#include "program_model.h"
#include "recovery_policy.h"

#include <ArduinoOTA.h>
// Compatibility HTTPUpload types need WebServer.h before ESPAsyncWebServer.h.
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
