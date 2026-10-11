#pragma once
/**
 * cloud_sync.h — Cloud synchronization for MABOSYTECH ESP32 Attendance System
 *
 * Sends attendance events and periodic heartbeats to the hosted cloud
 * dashboard via HTTPS. Uses an outbox pattern: events are queued and
 * retried with exponential backoff. The cloud endpoint deduplicates
 * by event_uid so retries are safe.
 *
 * Heartbeat responses carry pending commands from the dashboard. A
 * callback is invoked so the main app can execute them (sync RTC,
 * restart) and the ack is sent automatically on the next heartbeat.
 */
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include "config.h"

// Cloud endpoint — the deployed Supabase Edge Function for this project.
// Override in secrets_local.h if you ever move the project.
#ifndef CLOUD_BASE_URL
#define CLOUD_BASE_URL "https://mfzskhrwepiyqzwrzeow.supabase.co/functions/v1/device-ingest"
#endif

#define CLOUD_SYNC_INTERVAL_MS 15000UL
#define CLOUD_HEARTBEAT_INTERVAL_MS 60000UL
#define CLOUD_TIMEOUT_MS 10000UL
#define CLOUD_BATCH_SIZE 10
#define CLOUD_OUTBOX_MAX 64
#define CLOUD_MAX_PENDING_CMDS 4

struct CloudOutboxItem {
  char eventUid[48];
  char date[12];        // YYYY-MM-DD
  char timeStr[10];     // HH:MM:SS
  int  fingerprintId;
  char name[32];
  char collegeId[24];
  char role[16];
  char department[24];
  char eventType[12];   // CHECK_IN / CHECK_OUT
  char status[16];      // ON_TIME / LATE / EARLY / LEFT_EARLY / OVERTIME
  int  retryCount;
  unsigned long nextAttemptMs;
};

struct CloudPendingCommand {
  int  id;
  char command[16];     // "sync_time" / "restart"
  char payload[64];
};

typedef bool (*CloudCommandCallback)(const char* command, const char* payload);

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

  int pendingCount() const { return _outboxHead; }

private:
  bool _enabled = false;
  char _deviceId[32];
  char _apiKey[80];
  WiFiClientSecure _tls;
  CloudCommandCallback _commandCallback = nullptr;

  CloudOutboxItem _outbox[CLOUD_OUTBOX_MAX];
  int _outboxHead = 0;

  CloudPendingCommand _pendingAcks[CLOUD_MAX_PENDING_CMDS];
  int _pendingAckCount = 0;

  unsigned long _lastSyncMs = 0;

  bool postJson(const String& body, String* responseOut = nullptr);
  void processOutbox();
  void generateEventUid(char* buf, size_t bufLen, const char* date, const char* timeStr, int fpId);
  void processHeartbeatResponse(const String& response);
  void sendCommandAcks();
};

extern CloudSyncClass CloudSync;
