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
#include <freertos/queue.h>
#include <freertos/task.h>
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
#define CLOUD_BATCH_SIZE 5
#define CLOUD_OUTBOX_MAX 12  // RAM window only; remaining pending events stay on SD
#define CLOUD_ENQUEUE_QUEUE_DEPTH 16
#define CLOUD_WORKER_STACK_BYTES 8192
#define CLOUD_MAX_PENDING_CMDS 4  // extra commands remain pending on the server until a slot opens
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
  char payload[768];    // serialized JSON payload; can carry a short-lived signed OTA URL
  char result[128];     // serialized result JSON for the dashboard
  bool ackReady;
  bool succeeded;
};

// Fixed-size messages cross task boundaries; temporary String buffers from
// loop() are copied before the asynchronous network worker sees them.
struct CloudHeartbeatRequest {
  char fwVersion[24];
  char ip[16];
  char checkinTime[10];
  char checkoutTime[10];
  char deviceName[64];
  int rssi;
  int freeHeap;
  int templates;
  int capacity;
  int entryCount;
  int exitCount;
  int pendingCount;
  bool wifiOk;
  bool sdOk;
  bool rtcOk;
  bool rtcValid;
  bool isEnrolling;
  bool fpOk;
};

struct CloudCommandDispatch {
  char id[40];
  char command[32];
  char payload[768]; // can carry a short-lived signed Cloud OTA URL
};

struct CloudSettingsDispatch {
  char json[768];
};

struct CloudAckUpdate {
  char id[40];
  char result[128];
  bool success;
};

// A body builder runs only after the TLS handshake, keeping JSON allocations
// away from peak certificate-verification memory use.
typedef bool (*CloudJsonBodyBuilder)(String& body, const void* context);

// The handler can complete immediately or keep a command pending until a
// long-running operation (such as fingerprint enrollment) actually finishes.
typedef void (*CloudCommandCallback)(const char* commandId, const char* command, const char* payload);
typedef void (*CloudSettingsCallback)(const char* settingsJson);
typedef void (*CloudCommandAckSentCallback)(const char* commandId);

class CloudSyncClass {
public:
  void begin(const char* deviceId, const char* apiKey);
  // Acquires the single network/TLS slot for a deliberate OTA download.
  // Fail fast if cloud sync is busy; regular attendance must not wait for cloud I/O.
  bool beginExclusiveNetworkOperation(uint32_t timeoutMs = 1500);
  void endExclusiveNetworkOperation();

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
  void onSettings(CloudSettingsCallback cb) { _settingsCallback = cb; }
  void onCommandAckSent(CloudCommandAckSentCallback cb) { _ackSentCallback = cb; }

  void completeCommand(const char* commandId, bool success, const char* resultJson = nullptr);
  void flushCommandAcks();

  int pendingCount() const;

  // Signals the worker to recover persisted events after SD boot preparation is complete.
  void recoverOutboxFromSD();

private:
  bool _enabled = false;
  char _deviceId[32];
  char _apiKey[80];
  WiFiClientSecure _tls;
  SemaphoreHandle_t _networkMutex = nullptr; // serialize cloud HTTPS and explicit Cloud OTA
  CloudCommandCallback _commandCallback = nullptr;
  CloudSettingsCallback _settingsCallback = nullptr;
  CloudCommandAckSentCallback _ackSentCallback = nullptr;

  // The background worker exclusively owns the TLS client, RAM outbox and acks.
  // The Arduino loop communicates with it only through bounded queues.
  QueueHandle_t _enqueueQueue = nullptr;
  QueueHandle_t _heartbeatQueue = nullptr;
  QueueHandle_t _commandDispatchQueue = nullptr;
  QueueHandle_t _settingsDispatchQueue = nullptr;
  QueueHandle_t _ackUpdateQueue = nullptr;
  TaskHandle_t _workerTask = nullptr;
  volatile bool _recoveryRequested = false;
  bool _outboxReady = false;
  bool _heartbeatPending = false;
  bool _preferOutbox = true;
  CloudHeartbeatRequest _latestHeartbeat{};
  volatile int _pendingCountSnapshot = 0;
  unsigned long _lastSyncMs = 0;
  unsigned long _ackRetryAtMs = 0;
  uint8_t _ackRetryCount = 0;
  unsigned long _lastOfflineLogMs = 0;

  CloudOutboxItem _outbox[CLOUD_OUTBOX_MAX];
  int _outboxCount = 0;
  CloudPendingCommand _pendingAcks[CLOUD_MAX_PENDING_CMDS];
  int _pendingAckCount = 0;

  static void workerTaskEntry(void* argument);
  void workerTaskLoop();
  void serviceQueuedEvents();
  void serviceAckUpdates();
  bool hasReadyAck() const;
  void sendHeartbeatRequest(const CloudHeartbeatRequest& request);
  bool postJson(CloudJsonBodyBuilder bodyBuilder, const void* builderContext, String* responseOut, int* httpStatusOut);
  void processOutbox();
  void generateEventUid(char* buf, size_t bufLen, const char* date, const char* timeStr, int fpId);
  void processHeartbeatResponse(const String& response);
  void sendCommandAcks();

  // SD persistent outbox helpers
  bool persistOutboxItem(const CloudOutboxItem& item, uint32_t lockTimeoutMs = 1000);
  bool compactOutboxFile();
  bool recoverOutboxFiles();
  bool loadOutboxFromSD();
};

extern CloudSyncClass CloudSync;
