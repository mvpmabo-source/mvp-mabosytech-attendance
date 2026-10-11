/**
 * cloud_sync.cpp — Cloud synchronization implementation.
 *
 * Uses WiFiClientSecure with certificate validation disabled (ESP32
 * root CA bundle is large; for a single known host we rely on the
 * Supabase TLS cert). In production you should install a root CA
 * bundle or pin the certificate.
 */
#include "cloud_sync.h"
#include <ArduinoJson.h>

CloudSyncClass CloudSync;

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
  _tls.setInsecure();
}

void CloudSyncClass::generateEventUid(char* buf, size_t bufLen, const char* date, const char* timeStr, int fpId) {
  snprintf(buf, bufLen, "%s-%s-%s-%d", _deviceId, date, timeStr, fpId);
}

void CloudSyncClass::enqueueAttendance(
  const char* date, const char* timeStr, int fingerprintId,
  const char* name, const char* collegeId, const char* role,
  const char* department, const char* eventType, const char* status
) {
  if (!_enabled) return;
  if (_outboxHead >= CLOUD_OUTBOX_MAX) {
    memmove(&_outbox[0], &_outbox[1], sizeof(CloudOutboxItem) * (CLOUD_OUTBOX_MAX - 1));
    _outboxHead = CLOUD_OUTBOX_MAX - 1;
  }

  CloudOutboxItem& item = _outbox[_outboxHead];
  generateEventUid(item.eventUid, sizeof(item.eventUid), date, timeStr, fingerprintId);
  strncpy(item.date, date, sizeof(item.date) - 1); item.date[sizeof(item.date)-1] = '\0';
  strncpy(item.timeStr, timeStr, sizeof(item.timeStr) - 1); item.timeStr[sizeof(item.timeStr)-1] = '\0';
  item.fingerprintId = fingerprintId;
  strncpy(item.name, name ? name : "", sizeof(item.name) - 1); item.name[sizeof(item.name)-1] = '\0';
  strncpy(item.collegeId, collegeId ? collegeId : "", sizeof(item.collegeId) - 1); item.collegeId[sizeof(item.collegeId)-1] = '\0';
  strncpy(item.role, role ? role : "", sizeof(item.role) - 1); item.role[sizeof(item.role)-1] = '\0';
  strncpy(item.department, department ? department : "", sizeof(item.department) - 1); item.department[sizeof(item.department)-1] = '\0';
  strncpy(item.eventType, eventType, sizeof(item.eventType) - 1); item.eventType[sizeof(item.eventType)-1] = '\0';
  strncpy(item.status, status, sizeof(item.status) - 1); item.status[sizeof(item.status)-1] = '\0';
  item.retryCount = 0;
  item.nextAttemptMs = 0;
  _outboxHead++;
}

void CloudSyncClass::sendHeartbeat(
  const char* fwVersion, bool wifiOk, const char* ip,
  int rssi, int freeHeap, bool sdOk, bool rtcOk, bool rtcValid,
  int templates, int capacity, int entryCount, int exitCount,
  int pendingCount, const char* checkinTime, const char* checkoutTime,
  const char* deviceName, bool isEnrolling, bool fpOk
) {
  if (!_enabled) return;
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
  if (postJson(body, &response)) {
    processHeartbeatResponse(response);
  }
}

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
    bool success = false;
    if (_commandCallback) {
      success = _commandCallback(command, payload);
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
    postJson(body);
  }
  _pendingAckCount = 0;
}

void CloudSyncClass::loop() {
  if (!_enabled) return;
  unsigned long now = millis();
  if (now - _lastSyncMs >= CLOUD_SYNC_INTERVAL_MS) {
    _lastSyncMs = now;
    processOutbox();
  }
}

void CloudSyncClass::processOutbox() {
  if (_outboxHead == 0) return;
  int batchCount = min(_outboxHead, CLOUD_BATCH_SIZE);
  JsonDocument doc;
  doc["type"] = "batch";
  doc["device_id"] = _deviceId;
  doc["api_key"] = _apiKey;
  JsonArray events = doc["events"].to<JsonArray>();
  for (int i = 0; i < batchCount; i++) {
    CloudOutboxItem& item = _outbox[i];
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
  if (postJson(body)) {
    int remaining = _outboxHead - batchCount;
    if (remaining > 0) {
      memmove(&_outbox[0], &_outbox[batchCount], sizeof(CloudOutboxItem) * remaining);
    }
    _outboxHead = remaining;
  } else {
    for (int i = 0; i < batchCount; i++) {
      _outbox[i].retryCount++;
      unsigned long backoff = 1000UL << min(_outbox[i].retryCount, 8);
      _outbox[i].nextAttemptMs = millis() + backoff;
    }
  }
}

bool CloudSyncClass::postJson(const String& body, String* responseOut) {
  if (!_tls.connect(CLOUD_BASE_URL, 443, CLOUD_TIMEOUT_MS)) {
    return false;
  }
  String url = CLOUD_BASE_URL;
  url.replace("https://", "");
  int pathIdx = url.indexOf('/');
  String host = pathIdx >= 0 ? url.substring(0, pathIdx) : url;
  String path = pathIdx >= 0 ? url.substring(pathIdx) : "/";
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
  bool ok = false;
  bool headersDone = false;
  unsigned long deadline = millis() + CLOUD_TIMEOUT_MS;
  String responseLine;
  while (_tls.connected() && millis() < deadline) {
    String line = _tls.readStringUntil('\n');
    if (line.startsWith("HTTP/1.1 200") || line.startsWith("HTTP/1.0 200")) {
      ok = true;
    }
    if (line == "\r" || line.length() == 0) {
      headersDone = true;
      continue;
    }
    if (headersDone && responseOut) {
      responseLine += line;
    }
  }
  if (responseOut && ok) {
    *responseOut = responseLine;
  }
  _tls.stop();
  return ok;
}
