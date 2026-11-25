#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#include "Config.h"

namespace {
const char kBufferPath[] = "/buffer.jsonl";
const char kTempPath[] = "/buffer.tmp";
const char kBackupPath[] = "/buffer.bak";
}  // namespace

class OfflineBuffer {
 public:
  bool begin() {
    if (!LittleFS.begin(true)) {
      Serial.println("[storage] LittleFS mount failed");
      return false;
    }
    recoverFiles();
    if (!LittleFS.exists(kBufferPath)) {
      File file = LittleFS.open(kBufferPath, FILE_WRITE);
      if (!file) {
        Serial.println("[storage] Could not create telemetry buffer");
        return false;
      }
      file.close();
    }
    return true;
  }

  bool append(const String& jsonLine) {
    if (!ready() || jsonLine.indexOf('\n') >= 0 || jsonLine.indexOf('\r') >= 0) {
      Serial.println("[storage] Invalid record or buffer unavailable");
      return false;
    }

    const size_t lines = lineCount();
    if (lines < AirPulseConfig::OFFLINE_BUFFER_MAX_LINES) {
      File file = LittleFS.open(kBufferPath, FILE_APPEND);
      if (!file) {
        Serial.println("[storage] Could not append telemetry record");
        return false;
      }
      const size_t written = file.println(jsonLine);
      file.close();
      if (written == 0) {
        Serial.println("[storage] Telemetry record write failed");
        return false;
      }
      return true;
    }

    return rewriteKeepingNewest(jsonLine);
  }

  bool hasRecords() const {
    File file = LittleFS.open(kBufferPath, FILE_READ);
    if (!file) {
      return false;
    }
    const bool hasData = file.size() > 0;
    file.close();
    return hasData;
  }

  bool peekFirst(String& jsonLine) const {
    File file = LittleFS.open(kBufferPath, FILE_READ);
    if (!file) {
      Serial.println("[storage] Could not read telemetry buffer");
      return false;
    }
    jsonLine = file.readStringUntil('\n');
    file.close();
    jsonLine.trim();
    return jsonLine.length() != 0;
  }

  bool removeFirst() {
    File input = LittleFS.open(kBufferPath, FILE_READ);
    if (!input) {
      Serial.println("[storage] Could not open buffer for removal");
      return false;
    }
    input.readStringUntil('\n');
    File output = LittleFS.open(kTempPath, FILE_WRITE);
    if (!output) {
      input.close();
      Serial.println("[storage] Could not create temporary buffer");
      return false;
    }
    while (input.available()) {
      String line = input.readStringUntil('\n');
      if (line.length() != 0 && output.println(line) == 0) {
        input.close();
        output.close();
        LittleFS.remove(kTempPath);
        Serial.println("[storage] Failed while compacting buffer");
        return false;
      }
    }
    input.close();
    output.close();
    return replaceBufferWithTemp();
  }

 private:
  bool ready() const {
    return LittleFS.exists(kBufferPath);
  }

  size_t lineCount() const {
    File file = LittleFS.open(kBufferPath, FILE_READ);
    if (!file) {
      return 0;
    }
    size_t count = 0;
    while (file.available()) {
      if (file.read() == '\n') {
        ++count;
      }
    }
    file.close();
    return count;
  }

  bool rewriteKeepingNewest(const String& newRecord) {
    File input = LittleFS.open(kBufferPath, FILE_READ);
    if (!input) {
      Serial.println("[storage] Could not read full telemetry buffer");
      return false;
    }
    File output = LittleFS.open(kTempPath, FILE_WRITE);
    if (!output) {
      input.close();
      Serial.println("[storage] Could not create replacement buffer");
      return false;
    }

    size_t skipped = 0;
    while (input.available()) {
      String line = input.readStringUntil('\n');
      if (line.length() == 0) {
        continue;
      }
      if (skipped++ < 1) {
        continue;
      }
      if (output.println(line) == 0) {
        input.close();
        output.close();
        LittleFS.remove(kTempPath);
        Serial.println("[storage] Failed to rewrite telemetry buffer");
        return false;
      }
    }
    input.close();
    const bool newRecordWritten = output.println(newRecord) > 0;
    output.close();
    if (!newRecordWritten) {
      LittleFS.remove(kTempPath);
      Serial.println("[storage] Failed to add newest telemetry record");
      return false;
    }
    return replaceBufferWithTemp();
  }

  bool replaceBufferWithTemp() {
    if (LittleFS.exists(kBackupPath) && !LittleFS.remove(kBackupPath)) {
      Serial.println("[storage] Could not clear buffer backup");
      return false;
    }
    if (LittleFS.exists(kBufferPath) &&
        !LittleFS.rename(kBufferPath, kBackupPath)) {
      Serial.println("[storage] Could not preserve current buffer");
      return false;
    }
    if (!LittleFS.rename(kTempPath, kBufferPath)) {
      if (LittleFS.exists(kBackupPath)) {
        LittleFS.rename(kBackupPath, kBufferPath);
      }
      Serial.println("[storage] Could not install updated buffer");
      return false;
    }
    if (LittleFS.exists(kBackupPath)) {
      LittleFS.remove(kBackupPath);
    }
    return true;
  }

  void recoverFiles() {
    if (!LittleFS.exists(kBufferPath)) {
      if (LittleFS.exists(kTempPath)) {
        LittleFS.rename(kTempPath, kBufferPath);
      } else if (LittleFS.exists(kBackupPath)) {
        LittleFS.rename(kBackupPath, kBufferPath);
      }
    }
    if (LittleFS.exists(kBufferPath)) {
      if (LittleFS.exists(kTempPath)) {
        LittleFS.remove(kTempPath);
      }
      if (LittleFS.exists(kBackupPath)) {
        LittleFS.remove(kBackupPath);
      }
    }
  }

};
