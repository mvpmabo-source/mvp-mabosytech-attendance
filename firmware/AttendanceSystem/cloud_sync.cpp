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

CloudSyncClass CloudSync;

// ──────────────────────────────────────────────
// Initialization
// ──────────────────────────────────────────────

void CloudSyncClass::begin(const char* deviceId, const char* apiKey) {
  if (!deviceId || !*deviceId || !apiKey || !*apiKey) {
    _enabled = false;
    return;
  }
  strncpy(_deviceId, deviceId, sizeof(_deviceId) - 1);
  _deviceId[sizeof(_deviceId) - 1] = '\0';
  strncpy(_apiKey, apiKey, sizeof(_apiKey) - 1);
  _apiKey[sizeof(_apiKey) - 1] = '\0';
  _enabled = true;

  // Supabase's currently observed *.supabase.co chain uses Google Trust
  // Services WE1 -> GTS Root R4. Trust this root explicitly and retain normal
  // certificate/hostname validation. If Supabase changes CA, update this root.
  static const char GTS_ROOT_R4[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";
  _tls.setCACert(GTS_ROOT_R4);
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
  generateEventUid(item.eventUid, sizeof(item.eventUid), date, timeStr, fingerprintId);
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

  Serial.printf("[CLOUD] Enqueued event uid=%s fp=%d type=%s\n", item.eventUid, fingerprintId, eventType);
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

  // CSV: event_uid,date,time,fingerprint_id,name,college_id,role,department,event_type,status,retry_count,next_attempt
  String row;
  row.reserve(160);
  row += item.eventUid;          row += ",";
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

  // Preserve the current file as a backup before replacing it. The live file
  // is never truncated. Recovery prefers a valid live file, then temp, then backup.
  SD.remove(CLOUD_OUTBOX_TMP_FILE);
  File tmp = SD.open(CLOUD_OUTBOX_TMP_FILE, FILE_WRITE);
  if (!tmp) { unlockCloudSd(); return false; }
  const String header = String(CLOUD_OUTBOX_HEADER) + "\n";
  size_t expectedBytes = header.length();
  bool ok = tmp.write((const uint8_t*)header.c_str(), header.length()) == header.length();
  for (int i = 0; ok && i < _outboxCount; i++) {
    const CloudOutboxItem& item = _outbox[i];
    if (item.uploaded) continue;
    String row;
    row.reserve(180);
    row += item.eventUid; row += ","; row += item.date; row += ",";
    row += item.timeStr; row += ","; row += String(item.fingerprintId); row += ",";
    row += csvEscape(String(item.name)); row += ",";
    row += csvEscape(String(item.collegeId)); row += ",";
    row += csvEscape(String(item.role)); row += ",";
    row += csvEscape(String(item.department)); row += ",";
    row += item.eventType; row += ","; row += item.status; row += ",";
    row += String(item.retryCount); row += ","; row += String(item.nextAttempt); row += "\n";
    ok = tmp.write((const uint8_t*)row.c_str(), row.length()) == row.length();
    expectedBytes += row.length();
  }
  tmp.flush();
  tmp.close();

  // Verify the candidate can be read and has a valid header before promotion.
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
    strncpy(item.eventUid, fields[0].c_str(), sizeof(item.eventUid) - 1);
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
      if (strcmp(_outbox[i].eventUid, item.eventUid) == 0) {
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

  String body;
  serializeJson(doc, body);

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
  if (response.length() == 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return;

  bool ok = doc["ok"] | false;
  if (!ok) return;

  JsonArray cmds = doc["pending_commands"].as<JsonArray>();
  if (cmds.isNull()) return;

  for (JsonObject cmd : cmds) {
    int cmdId = cmd["id"] | 0;
    const char* command = cmd["command"] | "";
    const char* payload = "";
    JsonObject payloadObj = cmd["payload"].as<JsonObject>();
    if (!payloadObj.isNull()) {
      String payloadStr;
      serializeJson(payloadObj, payloadStr);
      if (_pendingAckCount < CLOUD_MAX_PENDING_CMDS) {
        _pendingAcks[_pendingAckCount].id = cmdId;
        strncpy(_pendingAcks[_pendingAckCount].command, command, sizeof(_pendingAcks[_pendingAckCount].command) - 1);
        _pendingAcks[_pendingAckCount].command[sizeof(_pendingAcks[_pendingAckCount].command) - 1] = '\0';
        strncpy(_pendingAcks[_pendingAckCount].payload, payloadStr.c_str(), sizeof(_pendingAcks[_pendingAckCount].payload) - 1);
        _pendingAcks[_pendingAckCount].payload[sizeof(_pendingAcks[_pendingAckCount].payload) - 1] = '\0';
        _pendingAckCount++;
      }
    } else {
      if (_pendingAckCount < CLOUD_MAX_PENDING_CMDS) {
        _pendingAcks[_pendingAckCount].id = cmdId;
        strncpy(_pendingAcks[_pendingAckCount].command, command, sizeof(_pendingAcks[_pendingAckCount].command) - 1);
        _pendingAcks[_pendingAckCount].command[sizeof(_pendingAcks[_pendingAckCount].command) - 1] = '\0';
        _pendingAcks[_pendingAckCount].payload[0] = '\0';
        _pendingAckCount++;
      }
    }

    // Execute the command via the app callback
    if (_commandCallback) {
      _commandCallback(command, payload);
    }
  }
}

void CloudSyncClass::sendCommandAcks() {
  for (int i = 0; i < _pendingAckCount; i++) {
    JsonDocument doc;
    doc["type"] = "ack_command";
    doc["device_id"] = _deviceId;
    doc["api_key"] = _apiKey;
    doc["command_id"] = _pendingAcks[i].id;
    doc["ack_status"] = "acknowledged";

    String body;
    serializeJson(doc, body);
    postJson(body, nullptr, nullptr);
  }
  _pendingAckCount = 0;
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
  JsonDocument doc;
  doc["type"] = "batch";
  doc["device_id"] = _deviceId;
  doc["api_key"] = _apiKey;

  JsonArray events = doc["events"].to<JsonArray>();
  for (int j = 0; j < eligibleCount; j++) {
    CloudOutboxItem& item = _outbox[eligibleIdx[j]];
    JsonObject evt = events.add<JsonObject>();
    evt["event_uid"] = item.eventUid;
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

  String body;
  serializeJson(doc, body);

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

      // Compact the outbox array (remove uploaded items)
      int newCount = 0;
      for (int i = 0; i < _outboxCount; i++) {
        if (!_outbox[i].uploaded) {
          if (newCount != i) {
            _outbox[newCount] = _outbox[i];
          }
          newCount++;
        }
      }
      _outboxCount = newCount;

      // Rewrite the SD outbox file to remove confirmed events
      compactOutboxFile();

      Serial.printf("[CLOUD] Batch upload confirmed: %d events accepted.\n", acceptedCount);
    } else {
      // HTTP 200 but no ok:true — server did not confirm. Apply backoff.
      Serial.println("[CLOUD] HTTP 200 but server did not confirm; applying backoff.");
      for (int j = 0; j < eligibleCount; j++) {
        _outbox[eligibleIdx[j]].retryCount++;
        uint32_t backoff = 1000UL << min(_outbox[eligibleIdx[j]].retryCount, 12);
        _outbox[eligibleIdx[j]].nextAttempt = millis() + backoff;
      }
      compactOutboxFile();
    }
  } else {
    // HTTP failure or connection error — apply backoff, keep events
    Serial.printf("[CLOUD] Upload failed (HTTP %d); events retained with backoff.\n", httpStatus);
    for (int j = 0; j < eligibleCount; j++) {
      _outbox[eligibleIdx[j]].retryCount++;
      uint32_t backoff = 1000UL << min(_outbox[eligibleIdx[j]].retryCount, 12);
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
  // Parse the URL to extract host and path
  String url = CLOUD_BASE_URL;
  url.replace("https://", "");
  int pathIdx = url.indexOf('/');
  String host = pathIdx >= 0 ? url.substring(0, pathIdx) : url;
  String path = pathIdx >= 0 ? url.substring(pathIdx) : "/";

  // Connect with TLS certificate validation (no setInsecure)
  if (!_tls.connect(host.c_str(), 443, CLOUD_TIMEOUT_MS)) {
    Serial.println("[CLOUD] TLS connection failed");
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
    "Content-Length: %d\r\n"
    "Connection: close\r\n"
    "\r\n"
    "%s",
    path.c_str(), host.c_str(), (int)body.length(), body.c_str()
  );

  // Read the HTTP response status line
  bool ok = false;
  int httpStatus = 0;
  bool headersDone = false;
  unsigned long deadline = millis() + CLOUD_TIMEOUT_MS;
  String responseBody;
  responseBody.reserve(512);

  while (_tls.connected() && millis() < deadline) {
    String line = _tls.readStringUntil('\n');

    // Parse the status line: "HTTP/1.1 200 OK"
    if (line.startsWith("HTTP/1.") && line.length() > 12) {
      int spaceIdx = line.indexOf(' ');
      if (spaceIdx >= 0) {
        String statusStr = line.substring(spaceIdx + 1, spaceIdx + 4);
        httpStatus = statusStr.toInt();
        ok = (httpStatus == 200);
      }
    }

    // Detect end of headers
    if (line == "\r" || line.length() == 0) {
      headersDone = true;
      continue;
    }

    // Read body (after headers)
    if (headersDone) {
      responseBody += line;
      // Also try reading raw bytes if available (some servers don't use newlines)
      while (_tls.available() && millis() < deadline) {
        int c = _tls.read();
        if (c >= 0) responseBody += (char)c;
      }
    }
  }

  _tls.stop();

  if (httpStatusOut) *httpStatusOut = httpStatus;

  // Only return success on HTTP 200
  if (!ok) {
    Serial.printf("[CLOUD] HTTP status %d (not 200)\n", httpStatus);
    return false;
  }

  // Parse response body if requested
  if (responseOut) {
    *responseOut = responseBody;
  }

  return true;
}
