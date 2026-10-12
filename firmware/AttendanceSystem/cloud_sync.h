#pragma once
/**
 * cloud_sync.h — Cloud synchronization for MABOSYTECH ESP32 Attendance System
 *
 * Sends attendance events and periodic heartbeats to the hosted cloud
 * dashboard via HTTPS with proper TLS certificate validation.
 *
 * PERSISTENT OUTBOX:
 *   Pending attendance events are written to a dedicated SD file
 *   (/cloud_outbox.csv) so they survive power loss and reboot. The
 *   outbox is recovered on boot and drained with exponential backoff.
 *   Events are only removed from the outbox after the server explicitly
 *   confirms acceptance (HTTP 200 + JSON {"ok":true}). The server
 *   deduplicates by event_uid so retries are safe.
 *
 * TLS:
 *   Uses the Arduino-ESP32 built-in root CA bundle for certificate-chain
 *   validation. Insecure TLS is never enabled.
 *
 * Heartbeat responses carry pending commands from the dashboard. A
 * callback is invoked so the main app can execute them (sync RTC,
 * restart) and the ack is sent automatically on the next heartbeat.
 */
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

extern SemaphoreHandle_t cloudSdMutex;
inline bool lockCloudSd(uint32_t timeoutMs = 1000) {
  return cloudSdMutex && xSemaphoreTake(cloudSdMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
inline void unlockCloudSd() {
  if (cloudSdMutex) xSemaphoreGive(cloudSdMutex);
}

// Bolt Database server-function endpoint is project-specific and must be set
// in secrets_local.h. Do not point devices at a guessed or old backend URL.
#ifndef CLOUD_BASE_URL
#define CLOUD_BASE_URL "https://mfzskhrwepiyqzwrzeow.supabase.co/functions/v1/device-ingest"
#endif
// Public/anon application key used by the dashboard's Bolt Database client.
// This is not a service-role/privileged key; never put a service-role key here.
#ifndef CLOUD_PUBLIC_API_KEY
#define CLOUD_PUBLIC_API_KEY ""
#endif

#define CLOUD_SYNC_INTERVAL_MS 15000UL
#define CLOUD_HEARTBEAT_INTERVAL_MS 60000UL
#define CLOUD_TIMEOUT_MS 15000UL
#define CLOUD_BATCH_SIZE 10
#define CLOUD_OUTBOX_MAX 64
#define CLOUD_MAX_PENDING_CMDS 8
#define CLOUD_MAX_RETRIES 12
#define CLOUD_OUTBOX_FILE "/cloud_outbox.csv"
#define CLOUD_OUTBOX_TMP_FILE "/cloud_outbox.tmp"
#define CLOUD_OUTBOX_BACKUP_FILE "/cloud_outbox.bak"
#define CLOUD_OUTBOX_HEADER "event_uid,date,time,fingerprint_id,name,college_id,role,department,event_type,status,retry_count,next_attempt"

struct CloudOutboxItem {
  // event_uid is derived from device ID + date + time + fingerprint ID,
  // so do not store a second copy of it in every RAM record.
  char date[11];        // YYYY-MM-DD + NUL
  char timeStr[9];      // HH:MM:SS + NUL
  uint8_t fingerprintId; // fingerprint ID range is 1..200
  char name[32];
  char collegeId[24];
  char role[16];
  char department[24];
  char eventType[10];   // CHECK_IN / CHECK_OUT, including NUL
  char status[11];      // longest value LEFT_EARLY + NUL
  uint8_t retryCount;
  uint32_t nextAttempt;  // millis() target for next attempt (0 = immediate)
  bool  uploaded : 1;    // marker: server confirmed acceptance
  bool  persisted : 1;   // true only after durable SD outbox write/recovery
};

struct CloudPendingCommand {
  char id[40];          // UUID from device_commands.id (or a legacy numeric ID)
  char command[32];
  char payload[192];    // serialized JSON payload
  char result[128];     // serialized result JSON for the dashboard
  bool ackReady;
  bool succeeded;
};

// The handler can complete immediately or keep a command pending until a
// long-running operation (such as fingerprint enrollment) actually finishes.
typedef void (*CloudCommandCallback)(const char* commandId, const char* command, const char* payload);

class CloudSyncClass {
public:
  void begin(const char* deviceId, const char* apiKey);
  void loop();
  bool isEnabled() const { return _enabled; }

  void enqueueAttendance(
    const char* date, const char* timeStr, int fingerprintId,
    const char* name, const char* collegeId, const char* role,
    const char* department, const char* eventType, const char* status
  );

  void sendHeartbeat(
    const char* fwVersion, bool wifiOk, const char* ip,
    int rssi, int freeHeap, bool sdOk, bool rtcOk, bool rtcValid,
    int templates, int capacity, int entryCount, int exitCount,
    int pendingCount, const char* checkinTime, const char* checkoutTime,
    const char* deviceName, bool isEnrolling, bool fpOk
  );

  void onCommand(CloudCommandCallback cb) { _commandCallback = cb; }
  void completeCommand(const char* commandId, bool success, const char* resultJson = nullptr);
  void flushCommandAcks();

  int pendingCount() const { return _outboxCount; }

  // Call after SD is mounted and ready. Loads any persisted pending events.
  void recoverOutboxFromSD();

private:
  bool _enabled = false;
  char _deviceId[32];
  char _apiKey[80];
  WiFiClientSecure _tls;
  CloudCommandCallback _commandCallback = nullptr;

  CloudOutboxItem _outbox[CLOUD_OUTBOX_MAX];
  int _outboxCount = 0;

  CloudPendingCommand _pendingAcks[CLOUD_MAX_PENDING_CMDS];
  int _pendingAckCount = 0;

  unsigned long _lastSyncMs = 0;

  bool postJson(const String& body, String* responseOut, int* httpStatusOut);
  void processOutbox();
  void generateEventUid(char* buf, size_t bufLen, const char* date, const char* timeStr, int fpId);
  void processHeartbeatResponse(const String& response);
  void sendCommandAcks();

  // SD persistent outbox helpers
  bool persistOutboxItem(const CloudOutboxItem& item);
  bool compactOutboxFile();
  bool recoverOutboxFiles();
  bool loadOutboxFromSD();
};

extern CloudSyncClass CloudSync;
