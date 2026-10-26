#pragma once
#include "checked_journal.h"

// Included after ExecutionRecord, with LittleFS already declared.
struct ExecutionFileStorage {
  bool exists(const char *path) { return LittleFS.exists(path); }
  bool ensureDirectory() { return LittleFS.exists("/state") || LittleFS.mkdir("/state"); }
  bool read(const char *path, void *buffer, size_t size) {
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory() || file.size() != size)
      return false;
    const bool ok = file.read(static_cast<uint8_t *>(buffer), size) == size;
    file.close();
    return ok;
  }
  bool write(const char *path, const void *buffer, size_t size) {
    File file = LittleFS.open(path, "w");
    if (!file)
      return false;
    const size_t written = file.write(static_cast<const uint8_t *>(buffer), size);
    file.flush();
    file.close();
    return written == size;
  }
  bool replace(const char *temporary, const char *destination) {
    return LittleFS.rename(temporary, destination);
  }
};
ExecutionFileStorage executionStorage;
CheckedJournal<ExecutionFileStorage, ExecutionRecord> executionJournal(executionStorage);
