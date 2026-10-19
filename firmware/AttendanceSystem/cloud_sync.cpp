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

namespace {
struct HeartbeatBodyContext {
  const char* deviceId; const char* apiKey; const char* fwVersion; bool wifiOk;
  const char* ip; int rssi; int freeHeap; bool sdOk; bool rtcOk; bool rtcValid;
  int templates; int capacity; int entryCount; int exitCount; int pendingCount;
  const char* checkinTime; const char* checkoutTime; const char* deviceName;
  bool isEnrolling; bool fpOk;
};
struct CommandAckBodyContext { const char* deviceId; const char* apiKey; const CloudPendingCommand* ack; };
struct BatchBodyContext {
  const char* deviceId; const char* apiKey; const CloudOutboxItem* items;
  const int* indices; int count;
};

bool serializeRequestBody(JsonDocument& doc, String& body) {
  const size_t expected = measureJson(doc);
  if (!body.reserve(expected)) {
    Serial.printf("[CLOUD] Cannot reserve JSON request body (%u bytes).\\n", (unsigned int)expected);
    return false;
  }
  const size_t written = serializeJson(doc, body);
  if (written != expected) {
    Serial.printf("[CLOUD] JSON request body incomplete: %u/%u bytes.\\n",
                  (unsigned int)written, (unsigned int)expected);
    return false;
  }
  return true;
}

bool buildHeartbeatBody(String& body, const void* opaque) {
  const HeartbeatBodyContext& c = *static_cast<const HeartbeatBodyContext*>(opaque);
  JsonDocument doc;
  doc["type"] = "heartbeat"; doc["device_id"] = c.deviceId; doc["api_key"] = c.apiKey;
  JsonObject hb = doc["heartbeat"].to<JsonObject>();
  hb["firmware_version"] = c.fwVersion ? c.fwVersion : "";
  hb["wifi_connected"] = c.wifiOk; hb["ip_address"] = c.ip ? c.ip : "";
  hb["rssi"] = c.rssi; hb["free_heap"] = c.freeHeap; hb["sd_available"] = c.sdOk;
  hb["rtc_available"] = c.rtcOk; hb["rtc_time_valid"] = c.rtcValid;
  hb["templates"] = c.templates; hb["capacity"] = c.capacity;
  hb["entry_count"] = c.entryCount; hb["exit_count"] = c.exitCount;
  hb["pending_count"] = c.pendingCount;
  hb["checkin_time"] = c.checkinTime ? c.checkinTime : "";
  hb["checkout_time"] = c.checkoutTime ? c.checkoutTime : "";
  hb["device_name"] = c.deviceName ? c.deviceName : "";
  hb["is_enrolling"] = c.isEnrolling; hb["fingerprint_ok"] = c.fpOk;
  return serializeRequestBody(doc, body);
}

bool buildCommandAckBody(String& body, const void* opaque) {
  const CommandAckBodyContext& c = *static_cast<const CommandAckBodyContext*>(opaque);
  JsonDocument doc;
  doc["type"] = "ack_command"; doc["device_id"] = c.deviceId; doc["api_key"] = c.apiKey;
  doc["command_id"] = c.ack->id;
  doc["ack_status"] = c.ack->succeeded ? "acknowledged" : "failed";
  JsonDocument resultDoc;
  if (deserializeJson(resultDoc, c.ack->result) == DeserializationError::Ok)
    doc["result"] = resultDoc.as<JsonVariant>();
  else
    doc["result"] = c.ack->result;
  return serializeRequestBody(doc, body);
}

bool buildBatchBody(String& body, const void* opaque) {
  const BatchBodyContext& c = *static_cast<const BatchBodyContext*>(opaque);
  JsonDocument doc;
  doc["type"] = "batch"; doc["device_id"] = c.deviceId; doc["api_key"] = c.apiKey;
  JsonArray events = doc["events"].to<JsonArray>();
  for (int j = 0; j < c.count; ++j) {
    const CloudOutboxItem& item = c.items[c.indices[j]];
    JsonObject evt = events.add<JsonObject>();
    char eventUid[48];
    snprintf(eventUid, sizeof(eventUid), "%s-%s-%s-%d",
             c.deviceId, item.date, item.timeStr, (int)item.fingerprintId);
    evt["event_uid"] = eventUid; evt["date"] = item.date; evt["time"] = item.timeStr;
    evt["fingerprint_id"] = item.fingerprintId; evt["name"] = item.name;
    evt["college_id"] = item.collegeId; evt["role"] = item.role;
    evt["department"] = item.department; evt["event_type"] = item.eventType;
    evt["status"] = item.status;
  }
  return serializeRequestBody(doc, body);
}
}  // namespace


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

  _enqueueQueue = xQueueCreate(CLOUD_ENQUEUE_QUEUE_DEPTH, sizeof(CloudOutboxItem));
  _heartbeatQueue = xQueueCreate(1, sizeof(CloudHeartbeatRequest));
  _commandDispatchQueue = xQueueCreate(1, sizeof(CloudCommandDispatch));
  _settingsDispatchQueue = xQueueCreate(1, sizeof(CloudSettingsDispatch));
  _ackUpdateQueue = xQueueCreate(CLOUD_MAX_PENDING_CMDS, sizeof(CloudAckUpdate));
  _networkMutex = xSemaphoreCreateMutex();

  if (!_enqueueQueue || !_heartbeatQueue || !_commandDispatchQueue ||
      !_settingsDispatchQueue || !_ackUpdateQueue || !_networkMutex) {
    Serial.println("[CLOUD] ERROR: background-worker queues could not be allocated; cloud sync disabled.");
    if (_enqueueQueue) vQueueDelete(_enqueueQueue);
    if (_heartbeatQueue) vQueueDelete(_heartbeatQueue);
    if (_commandDispatchQueue) vQueueDelete(_commandDispatchQueue);
    if (_settingsDispatchQueue) vQueueDelete(_settingsDispatchQueue);
    if (_ackUpdateQueue) vQueueDelete(_ackUpdateQueue);
    if (_networkMutex) vSemaphoreDelete(_networkMutex);
    _networkMutex = nullptr;
    _enqueueQueue = _heartbeatQueue = _commandDispatchQueue = nullptr;
    _settingsDispatchQueue = _ackUpdateQueue = nullptr;
    _enabled = false;
    return;
  }

  // Keep strict certificate validation and the built-in root CA bundle.
  // All potentially blocking TLS/HTTP calls are made only by cloudSync.
  _tls.useBuiltinCACertBundle();
  _enabled = true;
  const BaseType_t taskOk = xTaskCreatePinnedToCore(
    CloudSyncClass::workerTaskEntry, "cloudSync", CLOUD_WORKER_STACK_BYTES,
    this, 1, &_workerTask, 0
  );
  if (taskOk != pdPASS) {
    Serial.println("[CLOUD] ERROR: background worker could not start; cloud sync disabled.");
    if (_enqueueQueue) vQueueDelete(_enqueueQueue);
    if (_heartbeatQueue) vQueueDelete(_heartbeatQueue);
    if (_commandDispatchQueue) vQueueDelete(_commandDispatchQueue);
    if (_settingsDispatchQueue) vQueueDelete(_settingsDispatchQueue);
    if (_ackUpdateQueue) vQueueDelete(_ackUpdateQueue);
    if (_networkMutex) vSemaphoreDelete(_networkMutex);
    _networkMutex = nullptr;
    _enqueueQueue = _heartbeatQueue = _commandDispatchQueue = nullptr;
    _settingsDispatchQueue = _ackUpdateQueue = nullptr;
    _workerTask = nullptr;
    _enabled = false;
    return;
  }
  Serial.printf("[CLOUD] Background worker started on core 0; freeHeap=%u maxAlloc=%u\n",
                (unsigned int)ESP.getFreeHeap(), (unsigned int)ESP.getMaxAllocHeap());
}

bool CloudSyncClass::beginExclusiveNetworkOperation(uint32_t timeoutMs) {
  if (!_enabled || !_networkMutex) return false;
  if (xSemaphoreTake(_networkMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    Serial.println("[CLOUD] Exclusive TLS slot busy; refusing OTA start so attendance/network state is not corrupted.");
    return false;
  }
  // Release the idle keep-alive session before OTA allocates its own TLS client.
  _tls.stop();
  Serial.println("[CLOUD] Exclusive TLS slot acquired for Cloud OTA; idle cloud TLS session released.");
  return true;
}

void CloudSyncClass::endExclusiveNetworkOperation() {
  if (_networkMutex) xSemaphoreGive(_networkMutex);
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
  if (!_enabled || !_enqueueQueue) return;

  CloudOutboxItem item{};
  char eventUid[48];
  generateEventUid(eventUid, sizeof(eventUid), date ? date : "", timeStr ? timeStr : "", fingerprintId);
  strncpy(item.date, date ? date : "", sizeof(item.date) - 1);
  strncpy(item.timeStr, timeStr ? timeStr : "", sizeof(item.timeStr) - 1);
  item.fingerprintId = fingerprintId;
  strncpy(item.name, name ? name : "", sizeof(item.name) - 1);
  strncpy(item.collegeId, collegeId ? collegeId : "", sizeof(item.collegeId) - 1);
  strncpy(item.role, role ? role : "", sizeof(item.role) - 1);
  strncpy(item.department, department ? department : "", sizeof(item.department) - 1);
  strncpy(item.eventType, eventType ? eventType : "", sizeof(item.eventType) - 1);
  strncpy(item.status, status ? status : "", sizeof(item.status) - 1);
  item.retryCount = 0;
  item.nextAttempt = 0;
  item.uploaded = false;

  // Durability-first when SD is immediately available; never wait behind sdTask
  // on the attendance/main loop. Failed fast writes are retried by the worker.
  item.persisted = sdAvailable && persistOutboxItem(item, 0);
  if (xQueueSend(_enqueueQueue, &item, 0) == pdTRUE) {
    if (_workerTask) xTaskNotifyGive(_workerTask);
    Serial.printf("[CLOUD] Event queued uid=%s fp=%d type=%s persisted=%s\n",
                  eventUid, fingerprintId, item.eventType, item.persisted ? "yes" : "pending");
    return;
  }

  if (item.persisted || persistOutboxItem(item, 10)) {
    Serial.printf("[CLOUD] Event uid=%s retained durably on SD; enqueue queue is full.\n", eventUid);
    if (_workerTask) xTaskNotifyGive(_workerTask);
  } else {
    Serial.printf("[CLOUD] CRITICAL: local attendance is saved, but cloud outbox queue and SD fallback failed for uid=%s.\n",
                  eventUid);
  }
}

// ──────────────────────────────────────────────
// SD persistent outbox helpers
// ──────────────────────────────────────────────

bool CloudSyncClass::persistOutboxItem(const CloudOutboxItem& item, uint32_t lockTimeoutMs) {
  if (!sdAvailable) return false;

  // The main-loop path passes zero to avoid waiting; worker retries are bounded.
  if (!lockCloudSd(lockTimeoutMs)) return false;
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
  if (!_enabled || !_workerTask) return;
  _recoveryRequested = true;
  xTaskNotifyGive(_workerTask);
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
  if (!_enabled || !_heartbeatQueue) return;
  CloudHeartbeatRequest request{};
  auto copyField = [](char* target, size_t targetSize, const char* value) {
    if (!target || targetSize == 0) return;
    strncpy(target, value ? value : "", targetSize - 1);
    target[targetSize - 1] = '\0';
  };
  copyField(request.fwVersion, sizeof(request.fwVersion), fwVersion);
  copyField(request.ip, sizeof(request.ip), ip);
  copyField(request.checkinTime, sizeof(request.checkinTime), checkinTime);
  copyField(request.checkoutTime, sizeof(request.checkoutTime), checkoutTime);
  copyField(request.deviceName, sizeof(request.deviceName), deviceName);
  request.rssi = rssi;
  request.freeHeap = freeHeap;
  request.templates = templates;
  request.capacity = capacity;
  request.entryCount = entryCount;
  request.exitCount = exitCount;
  request.pendingCount = pendingCount;
  request.wifiOk = wifiOk;
  request.sdOk = sdOk;
  request.rtcOk = rtcOk;
  request.rtcValid = rtcValid;
  request.isEnrolling = isEnrolling;
  request.fpOk = fpOk;

  // One-slot mailbox coalesces heartbeat snapshots while TLS is busy.
  if (xQueueOverwrite(_heartbeatQueue, &request) != pdPASS) {
    Serial.println("[CLOUD] Heartbeat mailbox unavailable; snapshot not queued.");
    return;
  }
  if (_workerTask) xTaskNotifyGive(_workerTask);
}

void CloudSyncClass::sendHeartbeatRequest(const CloudHeartbeatRequest& request) {
  const HeartbeatBodyContext payload = {
    _deviceId, _apiKey, request.fwVersion, request.wifiOk, request.ip,
    request.rssi, request.freeHeap, request.sdOk, request.rtcOk, request.rtcValid,
    request.templates, request.capacity, request.entryCount, request.exitCount,
    request.pendingCount, request.checkinTime, request.checkoutTime,
    request.deviceName, request.isEnrolling, request.fpOk
  };
  String response;
  int httpStatus = 0;
  if (postJson(buildHeartbeatBody, &payload, &response, &httpStatus)) {
    processHeartbeatResponse(response);
  } else {
    Serial.printf("[CLOUD] Heartbeat failed (HTTP %d); next queued snapshot will retry.\n", httpStatus);
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

  // Settings writes stay in the Arduino/main-loop context.
  JsonVariantConst settingsValue = doc["settings"];
  if (settingsValue.isNull()) settingsValue = doc["device_settings"];
  if (!settingsValue.isNull() && _settingsDispatchQueue) {
    String settingsPayload;
    serializeJson(settingsValue, settingsPayload);
    CloudSettingsDispatch settingsMessage{};
    if (settingsPayload.length() < sizeof(settingsMessage.json)) {
      strncpy(settingsMessage.json, settingsPayload.c_str(), sizeof(settingsMessage.json)-1);
      xQueueOverwrite(_settingsDispatchQueue, &settingsMessage);
    } else {
      Serial.printf("[CLOUD] Settings payload too large to dispatch (%u bytes); ignored.\n",
                    (unsigned)settingsPayload.length());
    }
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
    // Execute strictly one command per heartbeat. Enrollment is a long-running
    // sensor operation; dispatching a following restart in the same response
    // reboots the ESP before the OLED can enter enrollment mode.
    if (slot >= 0) {
      Serial.printf("[CLOUD] First pending command %s is still in flight; deferring later commands.\n",
                    id.c_str());
      break;
    }

    if (_pendingAckCount >= CLOUD_MAX_PENDING_CMDS) {
      Serial.println("[CLOUD] Command queue full; command left pending on server.");
      break;
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
    CloudCommandDispatch dispatch{};
    strncpy(dispatch.id, _pendingAcks[slot].id, sizeof(dispatch.id)-1);
    strncpy(dispatch.command, _pendingAcks[slot].command, sizeof(dispatch.command)-1);
    strncpy(dispatch.payload, _pendingAcks[slot].payload, sizeof(dispatch.payload)-1);
    if (!_commandDispatchQueue || xQueueSend(_commandDispatchQueue, &dispatch, 0) != pdTRUE) {
      Serial.println("[CLOUD] Main-loop command queue is full; command remains pending on server.");
      memset(&_pendingAcks[slot], 0, sizeof(_pendingAcks[slot]));
      _pendingAckCount--;
      break;
    }
    Serial.printf("[CLOUD] Command queued for main loop id=%s type=%s\n", id.c_str(), command);
    break;
  }
}

void CloudSyncClass::completeCommand(const char* commandId, bool success, const char* resultJson) {
  if (!commandId || !commandId[0] || !_ackUpdateQueue) return;
  CloudAckUpdate update{};
  strncpy(update.id, commandId, sizeof(update.id)-1);
  update.success = success;
  if (resultJson && resultJson[0]) {
    strncpy(update.result, resultJson, sizeof(update.result)-1);
  } else {
    strncpy(update.result, success ? "{\"ok\":true}" : "{\"ok\":false}", sizeof(update.result)-1);
  }
  if (xQueueSend(_ackUpdateQueue, &update, 0) != pdTRUE) {
    Serial.printf("[CLOUD] CRITICAL: command ack-update queue is full for id=%s.\n", commandId);
    return;
  }
  if (_workerTask) xTaskNotifyGive(_workerTask);
}

void CloudSyncClass::flushCommandAcks() {
  if (_workerTask) xTaskNotifyGive(_workerTask);
}

int CloudSyncClass::pendingCount() const {
  const int queued = _enqueueQueue ? (int)uxQueueMessagesWaiting(_enqueueQueue) : 0;
  return (int)_pendingCountSnapshot + queued;
}

void CloudSyncClass::serviceQueuedEvents() {
  if (!_enqueueQueue) return;
  CloudOutboxItem item;
  while (xQueueReceive(_enqueueQueue, &item, 0) == pdTRUE) {
    if (!item.persisted) item.persisted = sdAvailable && persistOutboxItem(item, 1000);
    if (_outboxCount < CLOUD_OUTBOX_MAX) {
      _outbox[_outboxCount++] = item;
      continue;
    }
    if (item.persisted) {
      Serial.printf("[CLOUD] RAM outbox full; event fp=%u remains durable on SD.\n",
                    (unsigned)item.fingerprintId);
      continue;
    }
    if (xQueueSendToFront(_enqueueQueue, &item, 0) != pdTRUE &&
        !persistOutboxItem(item, 10)) {
      Serial.printf("[CLOUD] CRITICAL: unable to retain cloud event fp=%u after SD/queue pressure.\n",
                    (unsigned)item.fingerprintId);
    }
    break;
  }
}

void CloudSyncClass::serviceAckUpdates() {
  if (!_ackUpdateQueue) return;
  CloudAckUpdate update;
  while (xQueueReceive(_ackUpdateQueue, &update, 0) == pdTRUE) {
    bool found = false;
    for (int i = 0; i < _pendingAckCount; ++i) {
      if (strcmp(_pendingAcks[i].id, update.id) != 0) continue;
      _pendingAcks[i].succeeded = update.success;
      _pendingAcks[i].ackReady = true;
      strncpy(_pendingAcks[i].result, update.result, sizeof(_pendingAcks[i].result)-1);
      _pendingAcks[i].result[sizeof(_pendingAcks[i].result)-1] = '\0';
      _ackRetryAtMs = millis();
      _ackRetryCount = 0;
      found = true;
      break;
    }
    if (!found) Serial.printf("[CLOUD] Ack completion arrived after command slot was gone: id=%s\n", update.id);
  }
}

bool CloudSyncClass::hasReadyAck() const {
  for (int i = 0; i < _pendingAckCount; ++i) if (_pendingAcks[i].ackReady) return true;
  return false;
}

void CloudSyncClass::sendCommandAcks() {
  int readyIndex = -1;
  for (int i = 0; i < _pendingAckCount; ++i) {
    if (_pendingAcks[i].ackReady) { readyIndex = i; break; }
  }
  if (readyIndex < 0) return;
  const CloudPendingCommand ackToSend = _pendingAcks[readyIndex];
  const CommandAckBodyContext payload = {_deviceId, _apiKey, &ackToSend};
  int status = 0;
  if (postJson(buildCommandAckBody, &payload, nullptr, &status) && status >= 200 && status < 300) {
    for (int i = 0; i < _pendingAckCount; ++i) {
      if (strcmp(_pendingAcks[i].id, ackToSend.id) != 0) continue;
      for (int j = i + 1; j < _pendingAckCount; ++j) _pendingAcks[j - 1] = _pendingAcks[j];
      _pendingAckCount--;
      break;
    }
    _ackRetryAtMs = 0;
    _ackRetryCount = 0;
    Serial.printf("[CLOUD] Command ack confirmed by server id=%s status=%s\n",
                  ackToSend.id, ackToSend.succeeded ? "acknowledged" : "failed");
    if (_ackSentCallback) _ackSentCallback(ackToSend.id);
  } else {
    if (_ackRetryCount < 5) _ackRetryCount++;
    const uint32_t backoffMs = 1000UL << _ackRetryCount;
    _ackRetryAtMs = millis() + backoffMs;
    Serial.printf("[CLOUD] Command ack failed id=%s HTTP=%d; retry in %lu ms.\n",
                  ackToSend.id, status, (unsigned long)backoffMs);
  }
}

void CloudSyncClass::workerTaskEntry(void* argument) {
  if (!argument) { vTaskDelete(nullptr); return; }
  static_cast<CloudSyncClass*>(argument)->workerTaskLoop();
  vTaskDelete(nullptr);
}

void CloudSyncClass::workerTaskLoop() {
  for (;;) {
    if (!_enabled) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    if (_recoveryRequested) {
      _recoveryRequested = false;
      if (sdAvailable) {
        recoverOutboxFiles();
        loadOutboxFromSD();
      } else {
        Serial.println("[CLOUD] SD unavailable during outbox recovery; local attendance remains independent.");
      }
      _outboxReady = true;
      _pendingCountSnapshot = _outboxCount;
      Serial.printf("[CLOUD] Async outbox recovery finished; RAM window=%d pending queue=%u.\n",
                    _outboxCount, (unsigned)(_enqueueQueue ? uxQueueMessagesWaiting(_enqueueQueue) : 0));
    }

    if (!_outboxReady) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }

    CloudHeartbeatRequest incomingHeartbeat{};
    if (_heartbeatQueue && xQueueReceive(_heartbeatQueue, &incomingHeartbeat, 0) == pdTRUE) {
      _latestHeartbeat = incomingHeartbeat;
      _heartbeatPending = true;
    }

    serviceQueuedEvents();
    serviceAckUpdates();

    const uint32_t now = millis();
    const bool networkReady = WiFi.status() == WL_CONNECTED;
    if (!networkReady) {
      if (_lastOfflineLogMs == 0 || now - _lastOfflineLogMs >= 30000UL) {
        _lastOfflineLogMs = now;
        Serial.println("[CLOUD] Wi-Fi offline; events remain queued and will retry after reconnect.");
      }
    } else {
      _lastOfflineLogMs = 0;
      if (hasReadyAck() && (int32_t)(now - _ackRetryAtMs) >= 0) {
        sendCommandAcks();
      } else {
        const bool outboxDue = (uint32_t)(now - _lastSyncMs) >= CLOUD_SYNC_INTERVAL_MS;
        if (outboxDue && (_preferOutbox || !_heartbeatPending)) {
          _lastSyncMs = now;
          _preferOutbox = false;
          processOutbox();
        } else if (_heartbeatPending) {
          _heartbeatPending = false;
          _preferOutbox = true;
          sendHeartbeatRequest(_latestHeartbeat);
        } else if (outboxDue) {
          _lastSyncMs = now;
          _preferOutbox = false;
          processOutbox();
        }
      }
    }
    _pendingCountSnapshot = _outboxCount;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
}

void CloudSyncClass::loop() {
  if (!_enabled) return;
  CloudSettingsDispatch settings{};
  if (_settingsDispatchQueue && xQueueReceive(_settingsDispatchQueue, &settings, 0) == pdTRUE) {
    if (_settingsCallback) _settingsCallback(settings.json);
  }
  CloudCommandDispatch command{};
  if (_commandDispatchQueue && xQueueReceive(_commandDispatchQueue, &command, 0) == pdTRUE) {
    if (_commandCallback) _commandCallback(command.id, command.command, command.payload);
    else completeCommand(command.id, false, "{\"error\":\"No command handler registered\"}");
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

  // Defer JSON allocation until after TLS verification to preserve contiguous RAM.
  const BatchBodyContext payload = {_deviceId, _apiKey, _outbox, eligibleIdx, eligibleCount};
  String response;
  int httpStatus = 0;

  if (postJson(buildBatchBody, &payload, &response, &httpStatus)) {
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

bool CloudSyncClass::postJson(CloudJsonBodyBuilder bodyBuilder, const void* builderContext, String* responseOut, int* httpStatusOut) {
  // Only one TLS handshake may use the ESP32's constrained internal heap at
  // a time. Cloud OTA takes this same mutex before opening its own HTTPS client.
  if (!_networkMutex || xSemaphoreTake(_networkMutex, pdMS_TO_TICKS(1200)) != pdTRUE) {
    if (httpStatusOut) *httpStatusOut = 0;
    Serial.println("[CLOUD] TLS slot unavailable (another verified HTTPS transfer is active); request deferred for retry.");
    return false;
  }
  struct NetworkMutexRelease {
    SemaphoreHandle_t mutex;
    ~NetworkMutexRelease() { if (mutex) xSemaphoreGive(mutex); }
  } networkMutexRelease{_networkMutex};

  // TLS handshakes require a large contiguous internal-RAM allocation.
  // Log both total and largest-block heap to distinguish low memory from
  // fragmentation without weakening certificate verification.
  Serial.printf("[CLOUD] TLS preflight heap=%u maxAlloc=%u minFree=%u internalFree=%u internalMaxAlloc=%u\n",
                (unsigned int)ESP.getFreeHeap(),
                (unsigned int)ESP.getMaxAllocHeap(),
                (unsigned int)ESP.getMinFreeHeap(),
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // Parse the URL into stack buffers and defer payload allocations until after
  // mbedTLS completes RSA/X.509 verification.
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

  // Connect with TLS certificate validation (no setInsecure). Log only the host,
  // never credentials or the full request URL.
  if (_tls.connected()) {
    Serial.printf("[CLOUD] Reusing validated TLS connection; heap=%u maxAlloc=%u\n",
                  (unsigned int)ESP.getFreeHeap(), (unsigned int)ESP.getMaxAllocHeap());
  } else {
    // Remove any stale socket/TLS context before a fresh handshake. Reusing a
    // healthy keep-alive connection avoids repeated RSA/X.509 allocations.
    _tls.stop();
    Serial.printf("[CLOUD] TLS target host=%s port=443\n", host);
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
      _tls.stop();
      if (httpStatusOut) *httpStatusOut = 0;
      return false;
    }
  }

  // Verify the TLS connection is actually established with a valid cert
  if (!_tls.connected()) {
    Serial.println("[CLOUD] TLS connection dropped after handshake");
    _tls.stop();
    if (httpStatusOut) *httpStatusOut = 0;
    return false;
  }

  Serial.printf("[CLOUD] TLS connected; building request heap=%u maxAlloc=%u\\n",
                (unsigned int)ESP.getFreeHeap(),
                (unsigned int)ESP.getMaxAllocHeap());
  String body;
  if (!bodyBuilder || !bodyBuilder(body, builderContext)) {
    Serial.println("[CLOUD] JSON request body could not be built after TLS handshake.");
    _tls.stop();
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
  _tls.print("Connection: keep-alive\r\n\r\n");
  _tls.print(body);

  // Read complete HTTP lines only after bytes are actually available. Stream's
  // readStringUntil() can time out before the first server byte arrives; that
  // previously made the status parser treat a timeout as a blank header line.
  _tls.setTimeout(1000);
  bool ok = false;
  bool chunked = false;
  long contentLength = -1;
  int httpStatus = 0;
  unsigned long deadline = millis() + CLOUD_TIMEOUT_MS;
  const size_t MAX_RESPONSE_BYTES = 8192;
  String responseBody;
  responseBody.reserve(768);

  auto readHttpLine = [&](String& line, size_t maxLineLength) -> bool {
    line = "";
    while ((long)(deadline - millis()) > 0) {
      if (_tls.available() <= 0) {
        if (!_tls.connected()) return false;
        delay(1);
        continue;
      }

      const int ch = _tls.read();
      if (ch < 0) continue;
      if (ch == '\n') return true;
      if (ch == '\r') continue;
      if (line.length() >= maxLineLength) {
        Serial.println("[CLOUD] HTTP line exceeded safety limit.");
        return false;
      }
      line += (char)ch;
    }
    return false;
  };

  // Tolerate at most a few leading CRLFs, but never mistake a read timeout for
  // a genuine blank line. Require a real HTTP status line before reading headers.
  String statusLine;
  bool gotStatusLine = false;
  for (uint8_t skipped = 0; skipped < 4; ++skipped) {
    if (!readHttpLine(statusLine, 512)) break;
    statusLine.trim();
    if (statusLine.length() == 0) continue;
    gotStatusLine = true;
    break;
  }

  if (gotStatusLine && statusLine.startsWith("HTTP/")) {
    const int spaceIdx = statusLine.indexOf(' ');
    if (spaceIdx >= 0 && statusLine.length() >= (unsigned)(spaceIdx + 4)) {
      httpStatus = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
      ok = (httpStatus == 200);
    }
  } else {
    Serial.printf("[CLOUD] HTTP status line invalid/unavailable: %s\n",
                  statusLine.length() ? statusLine.c_str() : "(timeout or empty)");
  }

  // Parse headers and their framing metadata up to a genuine blank line.
  bool headersDone = false;
  bool serverRequestedClose = false;
  while ((long)(deadline - millis()) > 0) {
    String header;
    if (!readHttpLine(header, 512)) break;
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

    if (key == "connection" && value.indexOf("close") >= 0) {
      serverRequestedClose = true;
    } else if (key == "transfer-encoding" && value.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (key == "content-length") {
      const long parsedLength = value.toInt();
      if (parsedLength >= 0) contentLength = parsedLength;
    }
  }

  // Exact-byte reader shared by Content-Length and chunked framing.
  auto readResponseByte = [&]() -> int {
    while ((long)(deadline - millis()) > 0) {
      if (_tls.available() > 0) return _tls.read();
      if (!_tls.connected()) return -1;
      delay(1);
    }
    return -1;
  };

  bool bodyReadOk = headersDone && gotStatusLine && statusLine.startsWith("HTTP/");
  if (!headersDone) {
    Serial.println("[CLOUD] HTTP response headers were incomplete.");
    bodyReadOk = false;
  } else if (chunked) {
    // Decode RFC 9112 chunked transfer encoding before exposing JSON to callers.
    while ((long)(deadline - millis()) > 0) {
      String sizeLine;
      if (!readHttpLine(sizeLine, 64)) {
        bodyReadOk = false;
        break;
      }
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
        while ((long)(deadline - millis()) > 0) {
          String trailer;
          if (!readHttpLine(trailer, 512)) break;
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

  responseBody.trim();

  // Retain a validated TLS session only when framing is complete and the
  // server permits keep-alive. Explicit OTA takes the same mutex and stops
  // this socket before creating its dedicated download client.
  if (!ok || !bodyReadOk || serverRequestedClose || !_tls.connected()) {
    _tls.stop();
  }

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
