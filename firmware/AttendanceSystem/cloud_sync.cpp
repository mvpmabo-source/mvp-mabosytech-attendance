/**
 * cloud_sync.cpp — Cloud synchronization implementation.
 *
 * FIXES APPLIED:
 *   1. TLS: Uses ESP32 mbedtls root CA bundle for proper certificate
 *      validation. setInsecure() is never called.
 *   2. PERSISTENT OUTBOX: Pending events are stored on SD card in
 *      /cloud_outbox.csv so they survive reboot. Boot recovery reloads
 *      them. Original attendance.csv is never modified or deleted.
 *   3. SERVER CONFIRMATION: Events are only removed from the outbox
 *      when the server returns HTTP 200 AND JSON {"ok":true}. Partial
 *      HTTP 200 without the ok flag does not confirm acceptance.
 *   4. BACKOFF: Each event has its own retry count and next-attempt
 *      timestamp with exponential backoff (1s to ~1h).
 *   5. NO SILENT DROPS: If the outbox is full, new events are still
 *      persisted to the SD outbox file (append-only) and loaded on
 *      the next drain cycle.
 */
#include "cloud_sync.h"
#include "app.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdlib>

CloudSyncClass CloudSync;

// ──────────────────────────────────────────────
// Initialization
// ──────────────────────────────────────────────

void CloudSyncClass::begin(const char* deviceId, const char* apiKey) {
  if (!deviceId || !*deviceId || !apiKey || !*apiKey) {
    _enabled = false;
    Serial.println("[CLOUD] Disabled: CLOUD_DEVICE_ID or CLOUD_API_KEY is missing.");
    return;
  }
  if (strncmp(CLOUD_BASE_URL, "https://", 8) != 0) {
    _enabled = false;
    Serial.println("[CLOUD] Disabled: set the exact Bolt device-ingest Server Function URL in secrets_local.h.");
    return;
  }
  strncpy(_deviceId, deviceId, sizeof(_deviceId) - 1);
  _deviceId[sizeof(_deviceId) - 1] = '\0';
  strncpy(_apiKey, apiKey, sizeof(_apiKey) - 1);
  _apiKey[sizeof(_apiKey) - 1] = '\0';
  _enabled = true;

  // Use the ESP32 Arduino core's built-in Mozilla CA bundle. This avoids
  // parsing multiple PEM roots into heap on every TLS connection, which
  // previously failed with MBEDTLS_ERR_X509_ALLOC_FAILED on this device.
  // Certificate-chain validation remains enabled; never use setInsecure().
  _tls.useBuiltinCACertBundle();
  Serial.println("[CLOUD] Built-in CA bundle enabled; TLS verification remains ON.");
}

// ──────────────────────────────────────────────
// Event UID generation
// ──────────────────────────────────────────────

void CloudSyncClass::generateEventUid(char* buf, size_t bufLen, const char* date, const char* timeStr, int fpId) {
  // Deterministic UID: device + date + time + fingerprint_id
  // The server deduplicates on this field, so retries are safe.
  snprintf(buf, bufLen, "%s-%s-%s-%d", _deviceId, date, timeStr, fpId);
}

// ──────────────────────────────────────────────
// Enqueue attendance event
// ──────────────────────────────────────────────

void CloudSyncClass::enqueueAttendance(
  const char* date, const char* timeStr, int fingerprintId,
  const char* name, const char* collegeId, const char* role,
  const char* department, const char* eventType, const char* status
) {
  if (!_enabled) return;

  // Build the outbox item
  CloudOutboxItem item;
  memset(&item, 0, sizeof(item));
  char eventUid[48];
  generateEventUid(eventUid, sizeof(eventUid), date, timeStr, fingerprintId);
  strncpy(item.date, date, sizeof(item.date) - 1);
  item.date[sizeof(item.date) - 1] = '\0';
  strncpy(item.timeStr, timeStr, sizeof(item.timeStr) - 1);
  item.timeStr[sizeof(item.timeStr) - 1] = '\0';
  item.fingerprintId = fingerprintId;
  strncpy(item.name, name ? name : "", sizeof(item.name) - 1);
  item.name[sizeof(item.name) - 1] = '\0';
  strncpy(item.collegeId, collegeId ? collegeId : "", sizeof(item.collegeId) - 1);
  item.collegeId[sizeof(item.collegeId) - 1] = '\0';
  strncpy(item.role, role ? role : "", sizeof(item.role) - 1);
  item.role[sizeof(item.role) - 1] = '\0';
  strncpy(item.department, department ? department : "", sizeof(item.department) - 1);
  item.department[sizeof(item.department) - 1] = '\0';
  strncpy(item.eventType, eventType, sizeof(item.eventType) - 1);
  item.eventType[sizeof(item.eventType) - 1] = '\0';
  strncpy(item.status, status, sizeof(item.status) - 1);
  item.status[sizeof(item.status) - 1] = '\0';
  item.retryCount = 0;
  item.nextAttempt = 0;
  item.uploaded = false;

  // Keep the event in RAM even if the SD lock is busy or the card is absent.
  // processOutbox() retries persistence and never uploads an unpersisted event.
  item.persisted = sdAvailable && persistOutboxItem(item);
  if (_outboxCount < CLOUD_OUTBOX_MAX) {
    _outbox[_outboxCount++] = item;
  } else if (item.persisted) {
    Serial.println("[CLOUD] RAM outbox full; event remains durable on SD for later recovery.");
  } else {
    Serial.println("[CLOUD] CRITICAL: RAM outbox full and SD persistence failed; cloud copy could not be queued.");
  }

  Serial.printf("[CLOUD] Enqueued event uid=%s fp=%d type=%s\n", eventUid, fingerprintId, eventType);
}

// ──────────────────────────────────────────────
// SD persistent outbox helpers
// ──────────────────────────────────────────────

bool CloudSyncClass::persistOutboxItem(const CloudOutboxItem& item) {
  if (!sdAvailable) return false;

  // SD access is serialized against sdTask; fail fast if busy.
  if (!lockCloudSd(1000)) return false;
  // Create and initialize a new outbox explicitly before opening append mode.
  if (!SD.exists(CLOUD_OUTBOX_FILE)) {
    File init = SD.open(CLOUD_OUTBOX_FILE, FILE_WRITE);
    if (!init) {
      Serial.println("[CLOUD] ERROR: cannot create outbox file on SD");
      unlockCloudSd();
      return false;
    }
    const String header = String(CLOUD_OUTBOX_HEADER) + "\n";
    const bool headerOk = init.write((const uint8_t*)header.c_str(), header.length()) == header.length();
    init.flush();
    init.close();
    if (!headerOk) { unlockCloudSd(); return false; }
  }
  File f = SD.open(CLOUD_OUTBOX_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[CLOUD] ERROR: cannot append outbox file on SD");
    unlockCloudSd();
    return false;
  }

  // Regenerate the deterministic UID from event identity fields.
  char eventUid[48];
  generateEventUid(eventUid, sizeof(eventUid), item.date, item.timeStr, item.fingerprintId);
  // CSV: event_uid,date,time,fingerprint_id,name,college_id,role,department,event_type,status,retry_count,next_attempt
  String row;
  row.reserve(160);
  row += eventUid;               row += ",";
  row += item.date;              row += ",";
  row += item.timeStr;           row += ",";
  row += String(item.fingerprintId); row += ",";
  row += csvEscape(String(item.name));        row += ",";
  row += csvEscape(String(item.collegeId));   row += ",";
  row += csvEscape(String(item.role));        row += ",";
  row += csvEscape(String(item.department));  row += ",";
  row += item.eventType;         row += ",";
  row += item.status;            row += ",";
  row += String(item.retryCount); row += ",";
  row += String(item.nextAttempt);
  row += "\n";

  size_t written = f.write((const uint8_t*)row.c_str(), row.length());
  f.flush();
  f.close();

  const bool ok = written == row.length();
  if (!ok) Serial.println("[CLOUD] WARNING: outbox SD write may be incomplete");
  unlockCloudSd();
  return ok;
}

bool CloudSyncClass::compactOutboxFile() {
  if (!sdAvailable || !lockCloudSd(1000)) return false;

  // Write the active in-RAM window to a temp file. The SD outbox may contain
  // additional records that intentionally do not fit in RAM; preserve those
  // raw rows instead of dropping them during compaction.
  SD.remove(CLOUD_OUTBOX_TMP_FILE);
  File tmp = SD.open(CLOUD_OUTBOX_TMP_FILE, FILE_WRITE);
  if (!tmp) { unlockCloudSd(); return false; }

  const String header = String(CLOUD_OUTBOX_HEADER) + "\n";
  size_t expectedBytes = header.length();
  bool ok = tmp.write((const uint8_t*)header.c_str(), header.length()) == header.length();

  for (int i = 0; ok && i < _outboxCount; i++) {
    const CloudOutboxItem& item = _outbox[i];
    if (item.uploaded) continue;

    char eventUid[48];
    generateEventUid(eventUid, sizeof(eventUid), item.date, item.timeStr, item.fingerprintId);
    String row;
    row.reserve(180);
    row += eventUid; row += ","; row += item.date; row += ",";
    row += item.timeStr; row += ","; row += String(item.fingerprintId); row += ",";
    row += csvEscape(String(item.name)); row += ",";
    row += csvEscape(String(item.collegeId)); row += ",";
    row += csvEscape(String(item.role)); row += ",";
    row += csvEscape(String(item.department)); row += ",";
    row += item.eventType; row += ","; row += item.status; row += ",";
    row += String(item.retryCount); row += ","; row += String(item.nextAttempt); row += "\n";
    const size_t rowBytes = row.length();
    ok = tmp.write((const uint8_t*)row.c_str(), rowBytes) == rowBytes;
    expectedBytes += rowBytes;
  }
  tmp.flush();
  tmp.close();

  // Preserve rows from the old SD file that are not represented in RAM.
  // Entries marked uploaded are still represented in RAM at compaction time,
  // so they are intentionally omitted from the replacement file.
  if (ok && SD.exists(CLOUD_OUTBOX_FILE)) {
    File existing = SD.open(CLOUD_OUTBOX_FILE, FILE_READ);
    if (!existing) {
      ok = false;
    } else {
      String oldHeader = existing.readStringUntil('\n');
      if (oldHeader != CLOUD_OUTBOX_HEADER) ok = false;

      File append = ok ? SD.open(CLOUD_OUTBOX_TMP_FILE, FILE_APPEND) : File();
      if (ok && !append) ok = false;

      while (ok && existing.available()) {
        String rawRow = existing.readStringUntil('\n');
        if (rawRow.length() == 0) continue;

        bool representedInRam = false;
        const int comma = rawRow.indexOf(',');
        if (comma > 0) {
          for (int i = 0; i < _outboxCount; i++) {
            char eventUid[48];
            generateEventUid(eventUid, sizeof(eventUid),
                             _outbox[i].date, _outbox[i].timeStr, _outbox[i].fingerprintId);
            const size_t uidLen = strlen(eventUid);
            if (uidLen == (size_t)comma &&
                strncmp(rawRow.c_str(), eventUid, uidLen) == 0) {
              representedInRam = true;
              break;
            }
          }
        }

        if (representedInRam) continue;

        const size_t rowBytes = rawRow.length();
        if (append.write((const uint8_t*)rawRow.c_str(), rowBytes) != rowBytes ||
            append.write((const uint8_t*)"\n", 1) != 1) {
          ok = false;
          break;
        }
        expectedBytes += rowBytes + 1;
      }

      if (append) {
        append.flush();
        append.close();
      }
      existing.close();
    }
  }

  // Verify the complete candidate before promoting it over the live file.
  File verify = ok ? SD.open(CLOUD_OUTBOX_TMP_FILE, FILE_READ) : File();
  if (!verify) ok = false;
  if (verify) {
    if (verify.size() != expectedBytes) ok = false;
    String gotHeader = verify.readStringUntil('\n');
    if (gotHeader != CLOUD_OUTBOX_HEADER) ok = false;
    verify.close();
  }

  if (ok) {
    SD.remove(CLOUD_OUTBOX_BACKUP_FILE);
    if (SD.exists(CLOUD_OUTBOX_FILE) && !SD.rename(CLOUD_OUTBOX_FILE, CLOUD_OUTBOX_BACKUP_FILE)) ok = false;
    if (ok && !SD.rename(CLOUD_OUTBOX_TMP_FILE, CLOUD_OUTBOX_FILE)) {
      if (SD.exists(CLOUD_OUTBOX_BACKUP_FILE)) SD.rename(CLOUD_OUTBOX_BACKUP_FILE, CLOUD_OUTBOX_FILE);
      ok = false;
    }
  }

  unlockCloudSd();
  if (!ok) Serial.println("[CLOUD] Outbox compaction failed; original/backup retained for recovery.");
  return ok;
}

bool CloudSyncClass::recoverOutboxFiles() {
  if (!sdAvailable || !lockCloudSd(1000)) return false;
  // A live file wins; never replace it with an unverified temporary candidate.
  if (!SD.exists(CLOUD_OUTBOX_FILE)) {
    if (SD.exists(CLOUD_OUTBOX_TMP_FILE) && SD.rename(CLOUD_OUTBOX_TMP_FILE, CLOUD_OUTBOX_FILE)) {
      Serial.println("[CLOUD] Recovered outbox from temp candidate.");
    } else if (SD.exists(CLOUD_OUTBOX_BACKUP_FILE) && SD.rename(CLOUD_OUTBOX_BACKUP_FILE, CLOUD_OUTBOX_FILE)) {
      Serial.println("[CLOUD] Recovered outbox from backup.");
    }
  }
  const bool exists = SD.exists(CLOUD_OUTBOX_FILE);
  unlockCloudSd();
  return exists;
}

bool CloudSyncClass::loadOutboxFromSD() {
  if (!sdAvailable || !lockCloudSd(1000)) return false;

  File f = SD.open(CLOUD_OUTBOX_FILE, FILE_READ);
  if (!f) { unlockCloudSd(); return false; }  // No outbox file — nothing to recover

  sdReaderAttach(f);
  String fields[12];
  // Skip header
  readCsvRecord(f, fields, 12);

  int loaded = 0;
  while (readCsvRecord(f, fields, 12) && _outboxCount < CLOUD_OUTBOX_MAX) {
    bool allEmpty = true;
    for (int i = 0; i < 12; i++) {
      if (fields[i].length() > 0) { allEmpty = false; break; }
    }
    if (allEmpty) continue;

    CloudOutboxItem item;
    memset(&item, 0, sizeof(item));
    // field[0] is the persisted deterministic UID; regenerate it from identity
    // fields to save 48 bytes per in-RAM outbox entry.
    strncpy(item.date, fields[1].c_str(), sizeof(item.date) - 1);
    strncpy(item.timeStr, fields[2].c_str(), sizeof(item.timeStr) - 1);
    item.fingerprintId = fields[3].toInt();
    strncpy(item.name, fields[4].c_str(), sizeof(item.name) - 1);
    strncpy(item.collegeId, fields[5].c_str(), sizeof(item.collegeId) - 1);
    strncpy(item.role, fields[6].c_str(), sizeof(item.role) - 1);
    strncpy(item.department, fields[7].c_str(), sizeof(item.department) - 1);
    strncpy(item.eventType, fields[8].c_str(), sizeof(item.eventType) - 1);
    strncpy(item.status, fields[9].c_str(), sizeof(item.status) - 1);
    item.retryCount = fields[10].toInt();
    // nextAttempt uses millis(), which resets on reboot; don't restore stale time.
    item.nextAttempt = 0;
    item.uploaded = false;
    item.persisted = true;

    // Check for duplicate event_uid already in RAM outbox
    bool dup = false;
    for (int i = 0; i < _outboxCount; i++) {
      if (_outbox[i].fingerprintId == item.fingerprintId &&
          strcmp(_outbox[i].date, item.date) == 0 &&
          strcmp(_outbox[i].timeStr, item.timeStr) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) continue;

    _outbox[_outboxCount] = item;
    _outboxCount++;
    loaded++;

    if (_outboxCount >= CLOUD_OUTBOX_MAX) {
      Serial.println("[CLOUD] Outbox RAM buffer full during recovery; remaining events stay on SD.");
      break;
    }
  }
  f.close();
  unlockCloudSd();

  if (loaded > 0) {
    Serial.printf("[CLOUD] Recovered %d pending events from SD outbox.\n", loaded);
  }
  return loaded > 0;
}

void CloudSyncClass::recoverOutboxFromSD() {
  if (!_enabled || !sdAvailable) return;
  recoverOutboxFiles();
  loadOutboxFromSD();
}

// ──────────────────────────────────────────────
// Heartbeat
// ──────────────────────────────────────────────

void CloudSyncClass::sendHeartbeat(
  const char* fwVersion, bool wifiOk, const char* ip,
  int rssi, int freeHeap, bool sdOk, bool rtcOk, bool rtcValid,
  int templates, int capacity, int entryCount, int exitCount,
  int pendingCount, const char* checkinTime, const char* checkoutTime,
  const char* deviceName, bool isEnrolling, bool fpOk
) {
  if (!_enabled) return;

  // Send any pending command acks first
  sendCommandAcks();

  String body;
  {
    JsonDocument doc;
  doc["type"] = "heartbeat";
  doc["device_id"] = _deviceId;
  doc["api_key"] = _apiKey;

  JsonObject hb = doc["heartbeat"].to<JsonObject>();
  hb["firmware_version"] = fwVersion ? fwVersion : "";
  hb["wifi_connected"] = wifiOk;
  hb["ip_address"] = ip ? ip : "";
  hb["rssi"] = rssi;
  hb["free_heap"] = freeHeap;
  hb["sd_available"] = sdOk;
  hb["rtc_available"] = rtcOk;
  hb["rtc_time_valid"] = rtcValid;
  hb["templates"] = templates;
  hb["capacity"] = capacity;
  hb["entry_count"] = entryCount;
  hb["exit_count"] = exitCount;
  hb["pending_count"] = pendingCount;
  hb["checkin_time"] = checkinTime ? checkinTime : "";
  hb["checkout_time"] = checkoutTime ? checkoutTime : "";
  hb["device_name"] = deviceName ? deviceName : "";
  hb["is_enrolling"] = isEnrolling;
  hb["fingerprint_ok"] = fpOk;

    serializeJson(doc, body);
  }

  String response;
  int httpStatus = 0;
  if (postJson(body, &response, &httpStatus)) {
    processHeartbeatResponse(response);
  } else {
    Serial.printf("[CLOUD] Heartbeat failed (HTTP %d)\n", httpStatus);
  }
}

// ──────────────────────────────────────────────
// Heartbeat response processing (commands)
// ──────────────────────────────────────────────

void CloudSyncClass::processHeartbeatResponse(const String& response) {
  if (response.length() == 0) {
    Serial.println("[CLOUD] Heartbeat response body is empty; no commands can be dispatched.");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    String preview = response;
    preview.replace("\r", " ");
    preview.replace("\n", " ");
    if (preview.length() > 180) preview.remove(180);
    Serial.printf("[CLOUD] Heartbeat JSON parse failed: %s body=%s\n",
                  err.c_str(), preview.c_str());
    return;
  }
  if (!(doc["ok"] | false)) {
    Serial.printf("[CLOUD] Heartbeat response ok=false error=%s\n",
                  (const char*)(doc["error"] | "unspecified"));
    return;
  }

  // Some cloud deployments also return the persisted device settings alongside commands.
  // Apply them through a callback so the app can validate and persist them on the main loop.
  if (_settingsCallback && !doc["settings"].isNull()) {
    String settingsPayload;
    serializeJson(doc["settings"], settingsPayload);
    _settingsCallback(settingsPayload.c_str());
  } else if (_settingsCallback && !doc["device_settings"].isNull()) {
    String settingsPayload;
    serializeJson(doc["device_settings"], settingsPayload);
    _settingsCallback(settingsPayload.c_str());
  }

  JsonArray cmds = doc["pending_commands"].as<JsonArray>();
  if (cmds.isNull()) {
    Serial.println("[CLOUD] Heartbeat JSON has no pending_commands array.");
    return;
  }
  Serial.printf("[CLOUD] Heartbeat accepted; pending commands received=%u\n",
                (unsigned)cmds.size());

  for (JsonObject cmd : cmds) {
    String id = cmd["id"].as<String>();
    const char* command = cmd["command"] | "";
    if (!id.length() || !command[0]) {
      Serial.println("[CLOUD] Ignoring malformed pending command (missing id or command).");
      continue;
    }

    int slot = -1;
    for (int i = 0; i < _pendingAckCount; i++) {
      if (strcmp(_pendingAcks[i].id, id.c_str()) == 0) { slot = i; break; }
    }
    if (slot < 0) {
      if (_pendingAckCount >= CLOUD_MAX_PENDING_CMDS) {
        Serial.println("[CLOUD] Command queue full; command left pending on server.");
        continue;
      }
      slot = _pendingAckCount++;
      memset(&_pendingAcks[slot], 0, sizeof(_pendingAcks[slot]));
      strncpy(_pendingAcks[slot].id, id.c_str(), sizeof(_pendingAcks[slot].id)-1);
      strncpy(_pendingAcks[slot].command, command, sizeof(_pendingAcks[slot].command)-1);
      JsonVariantConst payload = cmd["payload"];
      if (!payload.isNull()) {
        String payloadStr;
        serializeJson(payload, payloadStr);
        strncpy(_pendingAcks[slot].payload, payloadStr.c_str(), sizeof(_pendingAcks[slot].payload)-1);
      } else {
        strcpy(_pendingAcks[slot].payload, "{}");
      }
      _pendingAcks[slot].ackReady = false;
      _pendingAcks[slot].succeeded = false;
      Serial.printf("[CLOUD] Command received id=%s type=%s\n", id.c_str(), command);
      if (_commandCallback) {
        _commandCallback(_pendingAcks[slot].id, _pendingAcks[slot].command, _pendingAcks[slot].payload);
      } else {
        completeCommand(id.c_str(), false, "{\"error\":\"No command handler registered\"}");
      }
    }
  }
}

void CloudSyncClass::completeCommand(const char* commandId, bool success, const char* resultJson) {
  if (!commandId || !commandId[0]) return;
  for (int i = 0; i < _pendingAckCount; i++) {
    if (strcmp(_pendingAcks[i].id, commandId) != 0) continue;
    _pendingAcks[i].succeeded = success;
    _pendingAcks[i].ackReady = true;
    if (resultJson && resultJson[0]) {
      strncpy(_pendingAcks[i].result, resultJson, sizeof(_pendingAcks[i].result)-1);
    } else {
      strcpy(_pendingAcks[i].result, success ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    return;
  }
}

void CloudSyncClass::flushCommandAcks() {
  sendCommandAcks();
}

void CloudSyncClass::sendCommandAcks() {
  int writeIndex = 0;
  for (int i = 0; i < _pendingAckCount; i++) {
    if (!_pendingAcks[i].ackReady) {
      if (writeIndex != i) _pendingAcks[writeIndex] = _pendingAcks[i];
      writeIndex++;
      continue;
    }
    String body;
    {
      JsonDocument doc;
    doc["type"] = "ack_command";
    doc["device_id"] = _deviceId;
    doc["api_key"] = _apiKey;
    doc["command_id"] = _pendingAcks[i].id;
    doc["ack_status"] = _pendingAcks[i].succeeded ? "acknowledged" : "failed";
    JsonDocument resultDoc;
    if (deserializeJson(resultDoc, _pendingAcks[i].result) == DeserializationError::Ok) {
      doc["result"] = resultDoc.as<JsonVariant>();
    } else {
      doc["result"] = _pendingAcks[i].result;
    }

      serializeJson(doc, body);
    }
    int status = 0;
    if (postJson(body, nullptr, &status) && status >= 200 && status < 300) {
      Serial.printf("[CLOUD] Command ack sent id=%s status=%s\n", _pendingAcks[i].id,
                    _pendingAcks[i].succeeded ? "acknowledged" : "failed");
    } else {
      if (writeIndex != i) _pendingAcks[writeIndex] = _pendingAcks[i];
      writeIndex++;
      Serial.printf("[CLOUD] Command ack failed id=%s HTTP=%d; will retry\n", _pendingAcks[i].id, status);
    }
  }
  _pendingAckCount = writeIndex;
}

// ──────────────────────────────────────────────
// Main loop — drain outbox periodically
// ──────────────────────────────────────────────

void CloudSyncClass::loop() {
  if (!_enabled) return;

  unsigned long now = millis();

  if (now - _lastSyncMs >= CLOUD_SYNC_INTERVAL_MS) {
    _lastSyncMs = now;
    processOutbox();
  }
}

// ──────────────────────────────────────────────
// Process outbox — batch upload with confirmation
// ──────────────────────────────────────────────

void CloudSyncClass::processOutbox() {
  // If a server-confirmed batch could not be compacted earlier, retry the SD
  // transaction before reusing its RAM slots.
  bool hasConfirmedInRam = false;
  for (int i = 0; i < _outboxCount; i++) {
    if (_outbox[i].uploaded) { hasConfirmedInRam = true; break; }
  }
  if (hasConfirmedInRam) {
    if (compactOutboxFile()) {
      int newCount = 0;
      for (int i = 0; i < _outboxCount; i++) {
        if (_outbox[i].uploaded) continue;
        if (newCount != i) _outbox[newCount] = _outbox[i];
        newCount++;
      }
      _outboxCount = newCount;
    } else {
      Serial.println("[CLOUD] Confirmed items retained in RAM until SD compaction succeeds.");
    }
  }

  // Refill this bounded RAM window from the durable SD outbox as capacity opens.
  if (sdAvailable && _outboxCount < CLOUD_OUTBOX_MAX) loadOutboxFromSD();
  if (_outboxCount == 0) return;

  unsigned long now = millis();

  // Collect events that are eligible for upload (nextAttempt has passed)
  // and have not exceeded max retries.
  int eligibleIdx[CLOUD_OUTBOX_MAX];
  int eligibleCount = 0;

  for (int i = 0; i < _outboxCount; i++) {
    if (_outbox[i].uploaded) continue;
    if (!_outbox[i].persisted) {
      if (sdAvailable && persistOutboxItem(_outbox[i])) _outbox[i].persisted = true;
      else continue;
    }
    // Check backoff: nextAttempt is a millis() target
    if (_outbox[i].nextAttempt > 0 && (long)(now - _outbox[i].nextAttempt) < 0) {
      continue;  // backoff not elapsed yet
    }

    eligibleIdx[eligibleCount] = i;
    eligibleCount++;
    if (eligibleCount >= CLOUD_BATCH_SIZE) break;
  }

  if (eligibleCount == 0) return;

  // Build the batch JSON payload
  String body;
  {
    JsonDocument doc;
  doc["type"] = "batch";
  doc["device_id"] = _deviceId;
  doc["api_key"] = _apiKey;

  JsonArray events = doc["events"].to<JsonArray>();
  for (int j = 0; j < eligibleCount; j++) {
    CloudOutboxItem& item = _outbox[eligibleIdx[j]];
    JsonObject evt = events.add<JsonObject>();
    char eventUid[48];
    generateEventUid(eventUid, sizeof(eventUid), item.date, item.timeStr, item.fingerprintId);
    evt["event_uid"] = eventUid;
    evt["date"] = item.date;
    evt["time"] = item.timeStr;
    evt["fingerprint_id"] = item.fingerprintId;
    evt["name"] = item.name;
    evt["college_id"] = item.collegeId;
    evt["role"] = item.role;
    evt["department"] = item.department;
    evt["event_type"] = item.eventType;
    evt["status"] = item.status;
  }

    serializeJson(doc, body);
  }

  String response;
  int httpStatus = 0;

  if (postJson(body, &response, &httpStatus)) {
    // Parse the server response to confirm acceptance
    bool serverConfirmed = false;
    int acceptedCount = 0;

    if (response.length() > 0) {
      JsonDocument respDoc;
      DeserializationError err = deserializeJson(respDoc, response);
      if (!err) {
        bool ok = respDoc["ok"] | false;
        if (ok) {
          serverConfirmed = true;
          acceptedCount = respDoc["accepted"] | eligibleCount;
        }
      }
    }

    if (serverConfirmed) {
      // Mark uploaded events and remove them from the outbox
      for (int j = 0; j < eligibleCount; j++) {
        _outbox[eligibleIdx[j]].uploaded = true;
      }

      // Commit acknowledged UID removal to SD before releasing RAM slots.
      if (compactOutboxFile()) {
        int newCount = 0;
        for (int i = 0; i < _outboxCount; i++) {
          if (_outbox[i].uploaded) continue;
          if (newCount != i) _outbox[newCount] = _outbox[i];
          newCount++;
        }
        _outboxCount = newCount;
        if (sdAvailable && _outboxCount < CLOUD_OUTBOX_MAX) loadOutboxFromSD();
      } else {
        Serial.println("[CLOUD] Batch accepted; confirmed items retained until SD compaction succeeds.");
      }

      Serial.printf("[CLOUD] Batch upload confirmed: %d events accepted.\n", acceptedCount);
    } else {
      // HTTP 200 but no ok:true — server did not confirm. Apply backoff.
      Serial.println("[CLOUD] HTTP 200 but server did not confirm; applying backoff.");
      for (int j = 0; j < eligibleCount; j++) {
        _outbox[eligibleIdx[j]].retryCount++;
        uint32_t backoff = 1000UL << min((int)_outbox[eligibleIdx[j]].retryCount, 12);
        _outbox[eligibleIdx[j]].nextAttempt = millis() + backoff;
      }
      compactOutboxFile();
    }
  } else {
    // HTTP failure or connection error — apply backoff, keep events
    Serial.printf("[CLOUD] Upload failed (HTTP %d); events retained with backoff.\n", httpStatus);
    for (int j = 0; j < eligibleCount; j++) {
      _outbox[eligibleIdx[j]].retryCount++;
      uint32_t backoff = 1000UL << min((int)_outbox[eligibleIdx[j]].retryCount, 12);
      _outbox[eligibleIdx[j]].nextAttempt = millis() + backoff;
    }
    // Persist updated retry counts to SD
    compactOutboxFile();
  }
}

// ──────────────────────────────────────────────
// POST JSON with proper HTTP parsing
// ──────────────────────────────────────────────

bool CloudSyncClass::postJson(const String& body, String* responseOut, int* httpStatusOut) {
  // TLS handshakes require a large contiguous internal-RAM allocation.
  // Log both total and largest-block heap to distinguish low memory from
  // fragmentation without weakening certificate verification.
  Serial.printf("[CLOUD] TLS preflight heap=%u maxAlloc=%u minFree=%u internalFree=%u internalMaxAlloc=%u\n",
                (unsigned int)ESP.getFreeHeap(),
                (unsigned int)ESP.getMaxAllocHeap(),
                (unsigned int)ESP.getMinFreeHeap(),
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // Parse the URL into stack buffers to avoid allocating temporary Strings
  // immediately before mbedTLS performs RSA/X.509 verification.
  const char* baseUrl = CLOUD_BASE_URL;
  if (strncmp(baseUrl, "https://", 8) != 0) {
    Serial.println("[CLOUD] TLS URL rejected: HTTPS is required.");
    if (httpStatusOut) *httpStatusOut = 0;
    return false;
  }
  const char* hostStart = baseUrl + 8;
  const char* pathStart = strchr(hostStart, '/');
  const size_t hostLen = pathStart ? (size_t)(pathStart - hostStart) : strlen(hostStart);
  char host[128];
  char path[192];
  if (hostLen == 0 || hostLen >= sizeof(host) ||
      (pathStart && strlen(pathStart) >= sizeof(path))) {
    Serial.println("[CLOUD] TLS URL rejected: host/path is too long or empty.");
    if (httpStatusOut) *httpStatusOut = 0;
    return false;
  }
  memcpy(host, hostStart, hostLen);
  host[hostLen] = '\0';
  if (pathStart) memcpy(path, pathStart, strlen(pathStart) + 1);
  else strcpy(path, "/");

  // Connect with TLS certificate validation (no setInsecure)
  if (!_tls.connect(host, 443, CLOUD_TIMEOUT_MS)) {
    char tlsError[160] = {0};
    const int tlsErrorCode = _tls.lastError(tlsError, sizeof(tlsError));
    Serial.printf("[CLOUD] TLS connection failed: code=%d detail=%s heap=%u maxAlloc=%u minFree=%u internalFree=%u internalMaxAlloc=%u\n",
                  tlsErrorCode, tlsError[0] ? tlsError : "(no TLS detail)",
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)ESP.getMaxAllocHeap(),
                  (unsigned int)ESP.getMinFreeHeap(),
                  (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (httpStatusOut) *httpStatusOut = 0;
    return false;
  }

  // Verify the TLS connection is actually established with a valid cert
  if (!_tls.connected()) {
    Serial.println("[CLOUD] TLS connection dropped after handshake");
    if (httpStatusOut) *httpStatusOut = 0;
    return false;
  }

  _tls.printf(
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n",
    path, host, (int)body.length()
  );
  // Optional public/anon key for the managed function gateway. The device
  // API key remains in the JSON body and is validated server-side.
  if (CLOUD_PUBLIC_API_KEY[0] != '\0') {
    _tls.printf("apikey: %s\r\nAuthorization: Bearer %s\r\n",
                CLOUD_PUBLIC_API_KEY, CLOUD_PUBLIC_API_KEY);
  }
  _tls.print("Connection: close\r\n\r\n");
  _tls.print(body);

  // Parse status/headers explicitly, then consume the response according to
  // HTTP framing. Supabase/Edge gateways may return chunked bodies; feeding
  // chunk markers to ArduinoJson silently prevented heartbeat commands reaching
  // the callback even though the heartbeat itself was stored successfully.
  _tls.setTimeout(1000);
  bool ok = false;
  bool chunked = false;
  long contentLength = -1;
  int httpStatus = 0;
  unsigned long deadline = millis() + CLOUD_TIMEOUT_MS;
  const size_t MAX_RESPONSE_BYTES = 8192;
  String responseBody;
  responseBody.reserve(768);

  String statusLine = _tls.readStringUntil('\n');
  statusLine.trim();
  if (statusLine.startsWith("HTTP/")) {
    const int spaceIdx = statusLine.indexOf(' ');
    if (spaceIdx >= 0 && statusLine.length() >= (unsigned)(spaceIdx + 4)) {
      httpStatus = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
      ok = (httpStatus == 200);
    }
  } else {
    Serial.printf("[CLOUD] HTTP status line malformed: %s\n",
                  statusLine.length() ? statusLine.c_str() : "(empty)");
  }

  // Read headers through the first blank line.
  bool headersDone = false;
  while (_tls.connected() && (long)(deadline - millis()) > 0) {
    String header = _tls.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) {
      headersDone = true;
      break;
    }

    const int colon = header.indexOf(':');
    if (colon <= 0) continue;
    String key = header.substring(0, colon);
    String value = header.substring(colon + 1);
    key.trim();
    value.trim();
    key.toLowerCase();
    value.toLowerCase();

    if (key == "transfer-encoding" && value.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (key == "content-length") {
      const long parsedLength = value.toInt();
      if (parsedLength >= 0) contentLength = parsedLength;
    }
  }

  // Wait briefly for one byte without blocking forever on a stalled server.
  auto readResponseByte = [&]() -> int {
    while (!_tls.available() && _tls.connected() && (long)(deadline - millis()) > 0) {
      delay(1);
    }
    return _tls.available() ? _tls.read() : -1;
  };

  bool bodyReadOk = headersDone;
  if (!headersDone) {
    Serial.println("[CLOUD] HTTP response headers were incomplete.");
    bodyReadOk = false;
  } else if (chunked) {
    // Decode chunked transfer encoding before exposing JSON to callers.
    while ((long)(deadline - millis()) > 0) {
      String sizeLine = _tls.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;

      const int extension = sizeLine.indexOf(';');
      if (extension >= 0) sizeLine.remove(extension);
      sizeLine.trim();
      char* endPtr = nullptr;
      const unsigned long chunkSize = strtoul(sizeLine.c_str(), &endPtr, 16);
      if (endPtr == sizeLine.c_str() || !endPtr || *endPtr != '\0' ||
          chunkSize > MAX_RESPONSE_BYTES ||
          responseBody.length() + chunkSize > MAX_RESPONSE_BYTES) {
        Serial.println("[CLOUD] Invalid/oversized chunked HTTP response.");
        bodyReadOk = false;
        break;
      }

      if (chunkSize == 0) {
        // Consume optional trailers; they are not part of the JSON body.
        while (_tls.connected() && (long)(deadline - millis()) > 0) {
          String trailer = _tls.readStringUntil('\n');
          trailer.trim();
          if (trailer.length() == 0) break;
        }
        break;
      }

      for (unsigned long i = 0; i < chunkSize; ++i) {
        const int ch = readResponseByte();
        if (ch < 0) {
          bodyReadOk = false;
          break;
        }
        responseBody += (char)ch;
      }
      if (!bodyReadOk) break;

      const int cr = readResponseByte();
      const int lf = readResponseByte();
      if (cr != '\r' || lf != '\n') {
        Serial.println("[CLOUD] Chunk terminator malformed.");
        bodyReadOk = false;
        break;
      }
    }
  } else if (contentLength >= 0) {
    if ((unsigned long)contentLength > MAX_RESPONSE_BYTES) {
      Serial.printf("[CLOUD] HTTP response too large: %ld bytes\n", contentLength);
      bodyReadOk = false;
    } else {
      for (long i = 0; i < contentLength; ++i) {
        const int ch = readResponseByte();
        if (ch < 0) {
          bodyReadOk = false;
          break;
        }
        responseBody += (char)ch;
      }
    }
  } else {
    // Close-delimited fallback for servers which omit Content-Length.
    while ((long)(deadline - millis()) > 0 && (_tls.connected() || _tls.available())) {
      const int ch = readResponseByte();
      if (ch < 0) {
        if (!_tls.connected()) break;
        continue;
      }
      if (responseBody.length() >= MAX_RESPONSE_BYTES) {
        Serial.println("[CLOUD] HTTP response exceeded safety limit.");
        bodyReadOk = false;
        break;
      }
      responseBody += (char)ch;
    }
  }

  _tls.stop();
  responseBody.trim();

  if (httpStatusOut) *httpStatusOut = httpStatus;
  if (responseOut) *responseOut = responseBody;

  Serial.printf("[CLOUD] HTTP response status=%d bytes=%u framing=%s bodyOK=%s\n",
                httpStatus, (unsigned int)responseBody.length(),
                chunked ? "chunked" : (contentLength >= 0 ? "content-length" : "close"),
                bodyReadOk ? "yes" : "no");

  if (!ok || !bodyReadOk) {
    String preview = responseBody;
    preview.replace("\r", " ");
    preview.replace("\n", " ");
    if (preview.length() > 180) preview.remove(180);
    Serial.printf("[CLOUD] HTTP request failed status=%d response=%s\n",
                  httpStatus, preview.length() ? preview.c_str() : "(empty)");
    return false;
  }

  return true;
}
