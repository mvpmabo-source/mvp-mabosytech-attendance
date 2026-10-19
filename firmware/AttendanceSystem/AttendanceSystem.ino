#include "app.h"
#include <esp_system.h>
#include "mbedtls/sha256.h"
#include "cloud_ota.h"
#include <ArduinoJson.h>

static void handleCloudCommand(const char* commandId, const char* command, const char* payload);
static void handleCloudSettings(const char* settingsJson);
static char activeCloudEnrollCommandId[40] = "";
static uint16_t pendingCloudEnrollTargetId = 0;
static constexpr uint8_t CLOUD_FP_SLOT_EMPTY_CODE = 0x0C;

static portMUX_TYPE cloudAckSentMux = portMUX_INITIALIZER_UNLOCKED;
static char cloudAckSentCommandId[40] = "";
static char cloudRestartCommandId[40] = "";
static bool cloudRestartAfterAck = false;
static unsigned long cloudRestartFallbackAtMs = 0;

static void handleCloudCommandAckSent(const char* commandId) {
  if (!commandId || !commandId[0]) return;
  portENTER_CRITICAL(&cloudAckSentMux);
  strncpy(cloudAckSentCommandId, commandId, sizeof(cloudAckSentCommandId)-1);
  cloudAckSentCommandId[sizeof(cloudAckSentCommandId)-1] = '\0';
  portEXIT_CRITICAL(&cloudAckSentMux);
}

static void serviceCloudRestartAfterAck() {
  char acknowledgedId[sizeof(cloudAckSentCommandId)] = "";
  portENTER_CRITICAL(&cloudAckSentMux);
  strncpy(acknowledgedId, cloudAckSentCommandId, sizeof(acknowledgedId)-1);
  cloudAckSentCommandId[0] = '\0';
  portEXIT_CRITICAL(&cloudAckSentMux);
  if (cloudRestartAfterAck && acknowledgedId[0] &&
      strcmp(acknowledgedId, cloudRestartCommandId) == 0) {
    cloudRestartAfterAck = false;
    cloudRestartCommandId[0] = '\0';
    Serial.println("[CLOUD] Restart ACK confirmed; scheduling device restart.");
    requestRestart(false);
    return;
  }
  if (cloudRestartAfterAck && (long)(millis() - cloudRestartFallbackAtMs) >= 0) {
    cloudRestartAfterAck = false;
    cloudRestartCommandId[0] = '\0';
    Serial.println("[CLOUD] Restart ACK did not confirm before timeout; proceeding with bounded restart fallback.");
    requestRestart(false);
  }
}

// Fail closed: a dashboard-selected fingerprint slot may only be used when the
// sensor positively reports that the slot is empty. UART errors are not "empty".
static uint8_t cloudFingerprintSlotStatus(uint16_t id) {
  if (id < 1 || id > MAX_FINGERPRINTS) return 0xFF;
  if (!lockFinger(500)) return 0xFF;
  const uint8_t status = finger.loadModel(id);
  unlockFinger();
  return status;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("[BUILD] Firmware %s\n", FIRMWARE_VERSION);
  delay(150);

  connectWiFi();
  wifiWasConnected = wifiDashboardReady();
  // The STA connection has already had a short initial wait in connectWiFi().
  // Keep boot moving even when the router is temporarily unavailable;
  // serviceWiFi() reconnects in the main loop without blocking SD/fingerprint.
  Serial.printf("[WIFI] boot network state: %s ip=%s freeHeap=%u\n",
                wifiDashboardReady() ? "CONNECTED" : "WAITING",
                wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                (unsigned)ESP.getFreeHeap());

  pinMode(BTN_CANCEL, INPUT_PULLUP);
  // BTN_ENROLL (GPIO34) is intentionally left unconfigured and unread:
  // physical buttons no longer have any control over enrollment mode.
  // The pin definition stays so the hardware pin map is unchanged; it
  // is simply not wired into any logic anymore.
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  // Dedicated main/system buzzer is active-low and fed from 3V3:
  // 3V3 -> buzzer (+), buzzer (-) -> GPIO15. Default HIGH keeps it quiet.
  pinMode(MAIN_BUZZER_PIN, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(MAIN_BUZZER_PIN, HIGH);

  for (int i=0;i<=MAX_FINGERPRINTS;i++) { insideState[i]=false; lastAttendanceActionMs[i]=0; }

  sharedStateMutex = xSemaphoreCreateMutex();
  prefsMutex       = xSemaphoreCreateMutex();
  fingerMutex      = xSemaphoreCreateMutex();
  ramLogSnapshotMutex = xSemaphoreCreateMutex();
  rtcMutex            = xSemaphoreCreateMutex();
  authSessionMutex    = xSemaphoreCreateMutex();
  cloudSdMutex        = xSemaphoreCreateMutex();
  attendanceQueue  = xQueueCreate(16, sizeof(AttendanceEvent));
  sdRequestQueue   = xQueueCreate(2,  sizeof(SdBulkRequest));
  // Reply buffering gives the async HTTP bridge room to absorb SD output
  // without making the SD task wait for every network write.
  lessonAppendQueue= xQueueCreate(8,  sizeof(LessonAppend));
  systemLogQueue   = xQueueCreate(32, sizeof(SystemLogEvent));
  // The legacy browser-time-sync queue is unused in cloud-only mode.

  prefs.begin("fps_db", false);
  loadAttendanceJournal();
  loadPresenceState();
  // OTA authentication uses its dedicated OTA_PASSWORD. It is independent
  // of the dashboard account's salted password record.
  setupOTA();
  loadDeviceSetup();
  initializeRTC();

  pinMode(OLED_CS, OUTPUT); digitalWrite(OLED_CS, HIGH);
  pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  bool oledOk = display.begin(0, true);
  if (!oledOk) Serial.println("[OLED] begin() reported failure.");
  display.clearDisplay(); display.setTextWrap(false); display.display();
  oledBig("STARTING", "Fingerprint", "System");

  setupTime();

  // Restore the proven legacy SD mount context: the SD bus is brought up and
  // the card is mounted before the large AsyncWebServer routing state is
  // allocated. All filesystem operations after setup remain in sdTask.
  initializeSD();
  ensureLessonsCsv();
  ensureSystemLog();

  // CLOUD-ONLY MODE:
  // Do not start the legacy local HTTP dashboard/API. Its HTTPD task, route
  // handlers, SD stream queues and JSON bootstrap caches consume fragmented
  // heap needed by mbedTLS certificate parsing and HTTPS cloud sync.
  // Enrollment, restart and time sync are handled by authenticated cloud commands.
  dashboardBootReady = false;
  attendanceBootReady = false;
  Serial.printf("[MEM] Cloud-only mode: local dashboard disabled; freeHeap=%u maxAlloc=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  xTaskCreatePinnedToCore(sdTaskFunc, "sdTask", 6144, nullptr, 1, &sdTaskHandle, 1);

  // SD history/migration and subsequent filesystem I/O remain owned by sdTask.

  fingerSerial.begin(57600, SERIAL_8N2, FINGER_RX, FINGER_TX);
  finger.begin(57600);
  smartDelay(150);
  oledScanAnimation("CHECKING", "Sensor", 400);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found!");
    fingerprintSensorOk = true;
    addLog("INFO", -1, "Fingerprint sensor ready");
    oledSuccessAnimation("Sensor ready");
    smartDelay(350);
  } else {
    Serial.println("Fingerprint sensor not found!");
    fingerprintSensorOk = false;
    addLog("ERROR", -1, "Fingerprint sensor not found");
    oledErrorAnimation("Sensor not found", "Check wiring");
  }

  if (fingerprintSensorOk) {
    printSensorParameters();
    setMatchingSecuritySerialOnly();
    refreshTemplateCountSerialOnly();
    rebuildRegisteredCache();
  }
  nextEnrollID = scanFirstFreeId;

  Serial.print("Next enroll ID: ");
  Serial.println(nextEnrollID == 0 ? "FULL (200/200)" : String(nextEnrollID));

  fpLastActivityMs = millis();
  fpLastProbeMs    = millis();

  if (fpTouchMode) {
    pinMode(FP_TOUCH_PIN, INPUT);   // GPIO34-39 have no internal pull resistors
    fpTouchSelfTest();              // may disable fpTouchMode on miswire
    if (fpTouchMode) {
      Serial.println("[FP] Sleep mode: TOUCH PIN (LED fully off, no blinking).");
    } else {
      Serial.println("[FP] Sleep mode: STROBE (LED pulses briefly while idle).");
    }
  } else {
    Serial.println("[FP] Sleep mode: STROBE (LED pulses briefly while idle).");
  }

  if (fingerprintSensorOk) fpSetLed(true);

  CloudSync.begin(CLOUD_DEVICE_ID, CLOUD_API_KEY);
  CloudSync.onCommand(handleCloudCommand);
  CloudSync.onSettings(handleCloudSettings);
  CloudSync.onCommandAckSent(handleCloudCommandAckSent);
  if (CloudSync.isEnabled())
    addLog("INFO", -1, "Cloud sync enabled");
  else
    Serial.println("[CLOUD] Cloud sync disabled — set CLOUD_DEVICE_ID and CLOUD_API_KEY in secrets_local.h");

  addLog("INFO", -1, "System ready");
  oledScanScreen();

  Serial.printf("[WIFI] final setup: connected=%s ip=%s freeHeap=%u\n",
                wifiDashboardReady() ? "YES" : "NO",
                wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                (unsigned)ESP.getFreeHeap());
  Serial.println("[BOOT] Cloud-only mode active; local HTTP dashboard disabled; SD task running.");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Keep ArduinoOTA serviced continuously so IDE firmware uploads can be
  // accepted while the device remains connected to the configured router LAN.
  serviceOTA();
  if (otaInProgress()) {
    delay(1);
    return;
  }

  // Maintain the router/STA link without blocking the real-time application.
  serviceWiFi();

  // Keep Wire/DS3231 and Preferences out of AsyncTCP callbacks. The network
  // callback only enqueues the browser epoch; loop() owns the actual write.
  serviceDeviceSetupPersistence();

  serviceLedBuzzerTimers();
  serviceWeekendAttendanceFeedback();
  serviceOledKeepAlive();
  bool applyFingerprintSecurity = false;
  if (lockState(20)) {
    applyFingerprintSecurity = fingerprintSecurityDirty && fingerprintSensorOk && !isEnrolling;
    if (applyFingerprintSecurity) fingerprintSecurityDirty = false;
    unlockState();
  }
  if (applyFingerprintSecurity && lockFinger(50)) {
    setMatchingSecuritySerialOnly();
    unlockFinger();
  }

  // Do not accept fingerprint attendance or enrollment while sdTask is still
  // building the authoritative boot history/cache.
  if (!attendanceBootReady) {
    servicePendingRestart();
    delay(1);
    return;
  }

  // One-shot: recover pending cloud outbox events from SD after boot is ready.
  static bool cloudOutboxRecovered = false;
  if (!cloudOutboxRecovered) {
    cloudOutboxRecovered = true;
    if (CloudSync.isEnabled()) {
      CloudSync.recoverOutboxFromSD();
      Serial.printf("[CLOUD] Outbox recovery requested asynchronously. Pending: %d\n", CloudSync.pendingCount());
    }
  }

  // -----------------------------------------------------------------
  // PHYSICAL BUTTON ENROLLMENT CONTROL REMOVED.
  // Per the production-stability requirement, the physical ENROLL and
  // CANCEL buttons must have no permission to start, join, or influence
  // enrollment mode. BTN_ENROLL is left wired and defined (the hardware
  // pin map is unchanged) but is never read anywhere in this firmware.
  // Enrollment is authorized exclusively through the dashboard's
  // Start Enrollment / Cancel Enrollment workflow (dashboardEnrollRequest
  // and cancelEnroll, both set only from the HTTP API). The device boots
  // straight into, and always returns to, normal Attendance Mode.
  // -----------------------------------------------------------------

  bool startRequested = false;
  if (lockState()) { if (!isEnrolling && dashboardEnrollRequest) startRequested = true; unlockState(); }

  if (startRequested) {
    String eName, eCollege, eRole, eDept, eNote;
    uint16_t targetId;
    uint16_t requestedCloudTargetId = 0;
    if (lockState()) {
      dashboardEnrollRequest = false;
      isEnrolling = true; cancelEnroll = false; duplicateFound = false;
      eName=pendingName; eCollege=pendingCollegeId;
      eRole=pendingRole; eDept=pendingDepartment; eNote=pendingNote;
      requestedCloudTargetId = pendingCloudEnrollTargetId;
      pendingCloudEnrollTargetId = 0;
      targetId = requestedCloudTargetId > 0 ? requestedCloudTargetId : nextEnrollID;
      unlockState();
    } else targetId = 0;

    // If the dashboard supplied a specific slot, verify it is definitely empty
    // before asking the sensor to store there. This prevents accidental overwrite.
    const uint8_t requestedSlotStatus =
      (requestedCloudTargetId > 0 && targetId > 0)
        ? cloudFingerprintSlotStatus(targetId)
        : CLOUD_FP_SLOT_EMPTY_CODE;

    if (targetId == 0) {
      addLog("ERROR", -1, "Fingerprint database full (200/200). Delete a record to free a slot.");
      oledErrorAnimation("DATABASE FULL", "Delete a record");
      smartDelay(1500);
      if (lockState()) { pendingName=pendingCollegeId=pendingRole=pendingDepartment=pendingNote=""; isEnrolling=false; unlockState(); }
      if (activeCloudEnrollCommandId[0]) {
        CloudSync.completeCommand(activeCloudEnrollCommandId, false, "{\"ok\":false,\"error\":\"fingerprint_database_full\"}");
        activeCloudEnrollCommandId[0] = '\0';
        CloudSync.flushCommandAcks();
      }
      oledScanScreen();
    } else if (requestedCloudTargetId > 0 && requestedSlotStatus != CLOUD_FP_SLOT_EMPTY_CODE) {
      const bool occupied = requestedSlotStatus == FINGERPRINT_OK;
      addLog("ERROR", requestedCloudTargetId,
             occupied ? "Requested fingerprint slot is already occupied" :
                        "Could not verify requested fingerprint slot");
      oledErrorAnimation(occupied ? "SLOT OCCUPIED" : "SLOT CHECK FAILED",
                         occupied ? "Choose free FP ID" : "Try command again");
      smartDelay(1200);
      if (lockState()) {
        pendingName=pendingCollegeId=pendingRole=pendingDepartment=pendingNote="";
        isEnrolling=false; cancelEnroll=false; duplicateFound=false;
        unlockState();
      }
      if (activeCloudEnrollCommandId[0]) {
        const char* slotError = occupied ? "fingerprint_slot_occupied" : "fingerprint_slot_check_failed";
        String result = String("{\"ok\":false,\"error\":\"") + slotError + "\"}";
        CloudSync.completeCommand(activeCloudEnrollCommandId, false, result.c_str());
        activeCloudEnrollCommandId[0] = '\0';
        CloudSync.flushCommandAcks();
      }
      oledScanScreen();
    } else {
      addLog("ENROLL", -1, eName.length()>0 ? ("Enrollment started for " + eName) : "Enrollment started");
      fpWake();   // sensor must be lit and responsive before enrolling
      uint8_t result = enrollFingerprint(targetId);
      bool wasCancelled=false, wasDuplicate=false;
      if (lockState()) { wasCancelled=cancelEnroll; wasDuplicate=duplicateFound; unlockState(); }

      bool cloudEnrollSucceeded = false;
      if (wasCancelled) {
        addLog("CANCEL", -1, "Enrollment cancelled");
        oledBig("CANCELLED", "Back to scan"); smartDelay(1000);
      } else if (wasDuplicate) {
        addLog("DUPLICATE", finger.fingerID, "Fingerprint already exists");
        smartDelay(1000);
      } else if (result == FINGERPRINT_OK) {
        PersonProfile p; p.name=eName; p.collegeId=eCollege; p.role=eRole; p.department=eDept; p.note=eNote;
        saveProfile(targetId, p);
        if (lockState()) { insideState[targetId] = false; unlockState(); }
        savePresenceState(targetId);
        String savedName = firstNameOnly(p.name);
        if (savedName.length()==0) savedName = "ID " + String(targetId);
        String enrollMsg = p.name.length()>0 ? (p.name + " enrolled successfully") : "Fingerprint enrolled successfully";
        addLog("ENROLLED", targetId, enrollMsg);
        oledSuccessAnimation("Saved", savedName);
        Serial.print("Enrollment successful. ID: "); Serial.println(targetId);
        smartDelay(900);
        appendRegisteredEntry(targetId, p);
        uint16_t nextFree = findNextFreeFrom(
          requestedCloudTargetId > 0 ? 1 : (uint16_t)(targetId + 1));
        if (lockState()) {
          if (nextFree == 0xFFFF) {
            // A transport failure is never interpreted as an empty slot.
            // Fail closed and require a later rescan instead of risking an overwrite.
            nextEnrollID = 0;
            scanFirstFreeId = 0;
          } else {
            nextEnrollID = nextFree;
            scanFirstFreeId = nextFree;
          }
          unlockState();
        }
        if (nextFree == 0xFFFF) {
          addLog("ERROR", -1, "Fingerprint slot scan failed after enrollment; no slot will be reused until the sensor is rescanned.");
        }
        refreshTemplateCountCache();
        cloudEnrollSucceeded = true;
      } else {
        addLog("ERROR", -1, "Enrollment failed");
        oledErrorAnimation("Not saved", "Try again");
        smartDelay(1200);
      }
      if (lockState()) {
        pendingName=pendingCollegeId=pendingRole=pendingDepartment=pendingNote="";
        isEnrolling=false; cancelEnroll=false; duplicateFound=false;
        unlockState();
      }
      if (activeCloudEnrollCommandId[0]) {
        String enrollResult = cloudEnrollSucceeded
          ? (String("{\"ok\":true,\"message\":\"fingerprint_enrolled\",\"fingerprint_id\":") + String(targetId) + "}")
          : String("{\"ok\":false,\"error\":\"enrollment_failed_or_cancelled\"}");
        CloudSync.completeCommand(activeCloudEnrollCommandId, cloudEnrollSucceeded, enrollResult.c_str());
        activeCloudEnrollCommandId[0] = '\0';
        CloudSync.flushCommandAcks();
      }
      fpWake();   // restart the idle timer after enrollment
      oledScanScreen();
    }
  }

  if (cancelButtonPressed()) {
    bool enrolling=false;
    if (lockState()) { enrolling = isEnrolling; unlockState(); }
    if (!enrolling) oledScanScreen();
  }

  bool enrollingNow=false, dashRequestNow=false;
  if (lockState()) { enrollingNow=isEnrolling; dashRequestNow=dashboardEnrollRequest; unlockState(); }

  if (!enrollingNow && !dashRequestNow) {
    if (scanResultPhase == SCAN_RESULT_NONE) {
      // Sleep-aware gate. fingerMutex serializes sensor UART activity with
      // asynchronous HTTP-triggered fingerprint operations.
      if (fpShouldScanNow()) {
        if (lockFinger(50)) { scanFingerprint(); unlockFinger(); }
      }
    } else {
      serviceScanResultPhase();
      fpNoteActivity();   // showing a result counts as activity
    }
  }

  // CloudSync.loop dispatches callbacks only; TLS/HTTP runs on core 0.
  CloudSync.loop();

  // Submit a lightweight device snapshot to the background network worker every ~15s.
  static unsigned long lastCloudHb = 0;
  if (CloudSync.isEnabled() && millis() - lastCloudHb >= 15000UL) {
    lastCloudHb = millis();
    struct tm tm;
    bool haveTime = getCurrentDateTime(tm);
    char dateBuf[12] = "", timeBuf[10] = "";
    if (haveTime) {
      snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    CloudSync.sendHeartbeat(
      FIRMWARE_VERSION,
      wifiDashboardReady(),
      wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "",
      WiFi.RSSI(),
      (int)ESP.getFreeHeap(),
      (bool)sdAvailable,
      (bool)rtcAvailable,
      (bool)rtcTimeValid,
      (int)cachedTemplateCount,
      MAX_FINGERPRINTS,
      entryCount, exitCount,
      CloudSync.pendingCount(),
      minutesToHHMM(cfgCheckinMinutes).c_str(),
      minutesToHHMM(cfgCheckoutMinutes).c_str(),
      cfgDeviceName.c_str(),
      isEnrolling,
      fingerprintSensorOk
    );
  }

  serviceCloudRestartAfterAck();
  servicePendingRestart();
  delay(1);
}

void serviceOledKeepAlive() {
  bool enrolling=false, dashReq=false;
  if (!lockState()) return;
  enrolling=isEnrolling;
  dashReq=dashboardEnrollRequest;
  unlockState();
  if (enrolling || dashReq || scanResultPhase!=SCAN_RESULT_NONE || !fingerprintSensorOk) return;
  static unsigned long lastRefresh=0;
  unsigned long now=millis();
  if (now-lastRefresh>=400) {
    lastRefresh=now;
    oledScanScreen();
  }
}

void serviceMainSdBuzzer() {
  // Persistent SD warning, but office-friendly: one short beep every few
  // seconds while the previously-mounted SD card remains unavailable.
  // This is fully non-blocking and stops immediately when SD is mounted again.
  static bool active = false;
  static bool outputOn = false;
  static unsigned long nextTransitionAt = 0;

  const unsigned long now = millis();

  // A successful remount always silences the main warning immediately.
  if (sdAvailable) {
    mainSdBuzzerWarningPending = false;
    active = false;
    outputOn = false;
    nextTransitionAt = 0;
    digitalWrite(MAIN_BUZZER_PIN, HIGH);
    return;
  }

  if (!active) {
    if (!mainSdBuzzerWarningPending) {
      digitalWrite(MAIN_BUZZER_PIN, HIGH);
      return;
    }

    // Start persistent warning only after a confirmed mounted -> unavailable
    // transition. If the SD task recovers before we service it, stay silent.
    if (sdAvailable) {
      mainSdBuzzerWarningPending = false;
      digitalWrite(MAIN_BUZZER_PIN, HIGH);
      return;
    }

    mainSdBuzzerWarningPending = false;
    active = true;
    outputOn = true;
    digitalWrite(MAIN_BUZZER_PIN, LOW);
    nextTransitionAt = now + MAIN_SD_BEEP_ON_MS;
    return;
  }

  if ((long)(now - nextTransitionAt) < 0) return;

  if (outputOn) {
    digitalWrite(MAIN_BUZZER_PIN, HIGH);
    outputOn = false;
    nextTransitionAt = now + MAIN_SD_BEEP_INTERVAL_MS;
  } else {
    digitalWrite(MAIN_BUZZER_PIN, LOW);
    outputOn = true;
    nextTransitionAt = now + MAIN_SD_BEEP_ON_MS;
  }
}

void serviceLedBuzzerTimers() {
  unsigned long now=millis();
  if (ledOffAt && (long)(now-ledOffAt)>=0) {
    digitalWrite(LED_GREEN,LOW);
    digitalWrite(LED_RED,LOW);
    ledOffAt=0;
  }
  if (buzzerOffAt && (long)(now-buzzerOffAt)>=0) {
    digitalWrite(BUZZER_PIN,LOW);
    buzzerOffAt=0;
  }

  serviceMainSdBuzzer();
}

void requestRestart(bool factory) {
  (void)factory;
  pendingRestart=true;
  pendingRestartAtMs=millis()+250UL;
}

void servicePendingRestart() {
  if (!pendingRestart || (long)(millis()-pendingRestartAtMs) < 0) return;
  pendingRestart=false;
  ESP.restart();
}


static bool normalizeCloudTimezone(String requested, String& posixTimezone) {
  requested.trim();
  if (requested.length() == 0 || requested.length() > 64) return false;

  // The ESP32 time API uses POSIX TZ strings rather than the full IANA database.
  // Map the project's likely Rwanda/East-Africa settings explicitly.
  if (requested.equalsIgnoreCase("Africa/Kigali") ||
      requested.equalsIgnoreCase("Africa/Maputo") ||
      requested.equalsIgnoreCase("Africa/Harare") ||
      requested.equalsIgnoreCase("Africa/Lusaka") ||
      requested.equalsIgnoreCase("Africa/Blantyre") ||
      requested.equalsIgnoreCase("Africa/Bujumbura")) {
    posixTimezone = "CAT-2";
    return true;
  }
  if (requested.equalsIgnoreCase("Africa/Nairobi") ||
      requested.equalsIgnoreCase("Africa/Kampala") ||
      requested.equalsIgnoreCase("Africa/Dar_es_Salaam")) {
    posixTimezone = "EAT-3";
    return true;
  }
  if (requested.equalsIgnoreCase("UTC") || requested.equalsIgnoreCase("Etc/UTC")) {
    posixTimezone = "UTC0";
    return true;
  }

  // Accept explicit POSIX TZ strings (e.g. CAT-2 or CET-1CEST,M3.5.0/2,M10.5.0/3).
  // Reject unknown IANA-style paths rather than silently applying the wrong offset.
  if (requested.indexOf('/') >= 0 && requested.indexOf(',') < 0) return false;
  bool hasLetter = false;
  bool hasDigit = false;
  for (size_t i = 0; i < requested.length(); ++i) {
    const char ch = requested.charAt(i);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) hasLetter = true;
    else if (ch >= '0' && ch <= '9') hasDigit = true;
    else if (ch != '+' && ch != '-' && ch != ',' && ch != '/' && ch != '.' && ch != ':') return false;
  }
  if (!hasLetter || !hasDigit) return false;
  posixTimezone = requested;
  return true;
}

static bool applyCloudSettingsDocument(JsonDocument& data, bool& changed, String& error) {
  changed = false;
  error = "";

  const char* checkin = data["checkin_time"] | (data["check_in_time"] | "");
  const char* checkout = data["checkout_time"] | (data["check_out_time"] | "");
  String deviceName = data["device_name"] | (data["display_name"] | "");
  deviceName.trim();
  const char* timezoneRaw = data["timezone"] | (data["time_zone"] | "");

  const bool hasCheckin = checkin && checkin[0];
  const bool hasCheckout = checkout && checkout[0];
  const bool hasName = deviceName.length() > 0;
  const bool hasTimezone = timezoneRaw && timezoneRaw[0];

  if (!hasCheckin && !hasCheckout && !hasName && !hasTimezone) {
    error = "no_supported_settings_in_payload";
    return false;
  }

  int checkinMinutes = -1;
  int checkoutMinutes = -1;
  if (hasCheckin) {
    checkinMinutes = hhmmToMinutes(String(checkin));
    if (checkinMinutes < 0 || checkinMinutes >= 1440) {
      error = "invalid_checkin_time";
      return false;
    }
  }
  if (hasCheckout) {
    checkoutMinutes = hhmmToMinutes(String(checkout));
    if (checkoutMinutes < 0 || checkoutMinutes >= 1440) {
      error = "invalid_checkout_time";
      return false;
    }
  }
  if (hasName && deviceName.length() > 48) {
    error = "device_name_too_long";
    return false;
  }

  String posixTimezone;
  if (hasTimezone && !normalizeCloudTimezone(String(timezoneRaw), posixTimezone)) {
    error = "unsupported_timezone_format";
    return false;
  }

  if (!lockState(100)) {
    error = "state_lock_unavailable";
    return false;
  }
  if (hasCheckin && cfgCheckinMinutes != checkinMinutes) {
    cfgCheckinMinutes = checkinMinutes;
    changed = true;
  }
  if (hasCheckout && cfgCheckoutMinutes != checkoutMinutes) {
    cfgCheckoutMinutes = checkoutMinutes;
    changed = true;
  }
  if (hasName && cfgDeviceName != deviceName) {
    cfgDeviceName = deviceName;
    changed = true;
  }
  if (hasTimezone && cfgTimezone != posixTimezone) {
    cfgTimezone = posixTimezone;
    changed = true;
  }
  unlockState();

  if (changed) deviceSetupPersistPending = true;
  return true;
}

static void handleCloudSettings(const char* settingsJson) {
  if (!settingsJson || !settingsJson[0]) return;
  JsonDocument settings;
  DeserializationError err = deserializeJson(settings, settingsJson);
  if (err) {
    Serial.printf("[CLOUD] Ignoring malformed heartbeat settings: %s\n", err.c_str());
    return;
  }

  bool changed = false;
  String error;
  if (!applyCloudSettingsDocument(settings, changed, error)) {
    Serial.printf("[CLOUD] Heartbeat settings not applied: %s\n", error.c_str());
    return;
  }
  Serial.printf("[CLOUD] Heartbeat settings processed; changed=%s\n", changed ? "yes" : "no");
}

static void handleCloudCommand(const char* commandId, const char* command, const char* payload) {
  if (!commandId || !command || !command[0]) return;
  JsonDocument data;
  if (payload && payload[0]) {
    DeserializationError err = deserializeJson(data, payload);
    if (err) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"invalid_payload_json\"}");
      return;
    }
  }

  if (!strcmp(command, "sync_time")) {
    uint32_t epoch = data["epoch"] | (data["unix_epoch"] | (data["timestamp"] | 0UL));
    const bool ok = epoch >= 1704067200UL && syncRTCFromCloudEpoch(epoch);
    CloudSync.completeCommand(commandId, ok,
      ok ? "{\"ok\":true,\"message\":\"rtc_synced_and_verified\"}" :
           "{\"ok\":false,\"error\":\"invalid_epoch_rtc_unavailable_or_verification_failed\"}");
    CloudSync.flushCommandAcks();
    return;
  }

  if (!strcmp(command, "restart")) {
    strncpy(cloudRestartCommandId, commandId, sizeof(cloudRestartCommandId)-1);
    cloudRestartCommandId[sizeof(cloudRestartCommandId)-1] = '\0';
    cloudRestartAfterAck = true;
    cloudRestartFallbackAtMs = millis() + CLOUD_TIMEOUT_MS + 1000UL;
    CloudSync.completeCommand(commandId, true, "{\"ok\":true,\"message\":\"restart_scheduled\"}");
    CloudSync.flushCommandAcks();
    Serial.println("[CLOUD] Restart queued; scanner loop continues while ACK runs in background.");
    return;
  }

  if (!strcmp(command, "cloud_ota")) {
    const String targetVersion = data["version"] | "";
    const String firmwareUrl = data["firmware_url"] | "";
    const String expectedSha256 = data["sha256"] | "";
    const size_t expectedSize = data["size_bytes"] | 0UL;
    const String urlError = data["firmware_url_error"] | "";

    if (targetVersion.length() == 0 || expectedSha256.length() != 64 || expectedSize == 0) {
      Serial.printf("[CLOUD OTA] Invalid release metadata (version=%s size=%u sha_len=%u).\n",
                    targetVersion.c_str(), (unsigned)expectedSize, (unsigned)expectedSha256.length());
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"invalid_firmware_release_metadata\"}");
      CloudSync.flushCommandAcks();
      return;
    }
    if (targetVersion == FIRMWARE_VERSION) {
      Serial.printf("[CLOUD OTA] %s is already installed; skipping download.\n", targetVersion.c_str());
      CloudSync.completeCommand(commandId, true, "{\"ok\":true,\"message\":\"already_on_target_version\"}");
      CloudSync.flushCommandAcks();
      return;
    }
    if (isEnrolling || dashboardEnrollRequest || activeCloudEnrollCommandId[0]) {
      Serial.println("[CLOUD OTA] Rejected: fingerprint enrollment is active.");
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"enrollment_in_progress\"}");
      CloudSync.flushCommandAcks();
      return;
    }
    if (firmwareUrl.length() == 0) {
      Serial.printf("[CLOUD OTA] Firmware URL unavailable: %s\n",
                    urlError.length() ? urlError.c_str() : "missing_signed_url");
      oledErrorAnimation("OTA FAILED", "Release URL unavailable");
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"firmware_download_url_unavailable\"}");
      CloudSync.flushCommandAcks();
      return;
    }

    const bool updated = performCloudFirmwareUpdate(
      targetVersion.c_str(), firmwareUrl.c_str(),
      expectedSha256.c_str(), expectedSize);

    if (updated) {
      Serial.printf("[CLOUD OTA] Update to %s verified and staged; restart waits for ACK.\\n",
                    targetVersion.c_str());
      strncpy(cloudRestartCommandId, commandId, sizeof(cloudRestartCommandId)-1);
      cloudRestartCommandId[sizeof(cloudRestartCommandId)-1] = '\\0';
      cloudRestartAfterAck = true;
      cloudRestartFallbackAtMs = millis() + CLOUD_TIMEOUT_MS + 1000UL;
      CloudSync.completeCommand(commandId, true, "{\\"ok\\":true,\\"message\\":\\"firmware_verified_restart_scheduled\\"}");
      CloudSync.flushCommandAcks();
      Serial.println("[CLOUD OTA] ACK queued; attendance loop remains active while the cloud worker sends it.");
      return;
    }

    CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"firmware_download_or_verification_failed\"}");
    CloudSync.flushCommandAcks();
    oledErrorAnimation("OTA FAILED", "Old firmware kept");
    return;
  }

  if (!strcmp(command, "update_settings")) {
    bool changed = false;
    String error;
    const bool ok = applyCloudSettingsDocument(data, changed, error);

    JsonDocument result;
    result["ok"] = ok;
    if (ok) result["message"] = changed ? "settings_applied" : "settings_already_current";
    else result["error"] = error;

    String resultJson;
    serializeJson(result, resultJson);
    CloudSync.completeCommand(commandId, ok, resultJson.c_str());
    CloudSync.flushCommandAcks();
    return;
  }

  if (!strcmp(command, "enroll_fingerprint") || !strcmp(command, "enroll")) {
    if (!fingerprintSensorOk || isEnrolling || dashboardEnrollRequest || activeCloudEnrollCommandId[0]) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"sensor_unavailable_or_enrollment_busy\"}");
      CloudSync.flushCommandAcks(); return;
    }
    String name = data["name"] | "";
    String college = data["college_id"] | "";
    String role = data["role"] | "Teacher";
    String department = data["department"] | "";
    String note = data["note"] | "";
    const int requestedId = data["fingerprint_id"] | 0;
    if (requestedId < 0 || requestedId > MAX_FINGERPRINTS) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"invalid_fingerprint_id\"}");
      CloudSync.flushCommandAcks(); return;
    }
    if (!name.length()) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"teacher_name_required\"}");
      CloudSync.flushCommandAcks(); return;
    }
    if (!lockState()) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"state_lock_unavailable\"}");
      CloudSync.flushCommandAcks(); return;
    }
    pendingName = name; pendingCollegeId = college; pendingRole = role;
    pendingDepartment = department; pendingNote = note;
    pendingCloudEnrollTargetId = (uint16_t)requestedId; // zero means legacy auto-slot selection
    dashboardEnrollRequest = true;
    strncpy(activeCloudEnrollCommandId, commandId, sizeof(activeCloudEnrollCommandId)-1);
    activeCloudEnrollCommandId[sizeof(activeCloudEnrollCommandId)-1] = '\0';
    unlockState();
    Serial.printf("[CLOUD] Enrollment queued for %s\n", name.c_str());
    return; // acknowledge only after sensor enrollment really finishes
  }

  if (!strcmp(command, "delete_fingerprint") || !strcmp(command, "delete_fingerprint_template")) {
    int id = data["fingerprint_id"] | (data["template_id"] | 0);
    if (id < 1 || id > MAX_FINGERPRINTS || !fingerprintSensorOk || !lockFinger(100)) {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"invalid_id_sensor_unavailable_or_busy\"}");
      CloudSync.flushCommandAcks(); return;
    }
    uint8_t result = finger.deleteModel(id);
    unlockFinger();
    if (result == FINGERPRINT_OK) {
      deleteProfile((uint16_t)id);
      clearPresenceState((uint16_t)id);
      removeRegisteredEntry((uint16_t)id);
      refreshTemplateCountCache();
      if (nextEnrollID == 0 || id < nextEnrollID) nextEnrollID = (uint16_t)id;
      CloudSync.completeCommand(commandId, true, "{\"ok\":true,\"message\":\"fingerprint_deleted\"}");
    } else {
      CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"sensor_delete_failed\"}");
    }
    CloudSync.flushCommandAcks();
    return;
  }

  CloudSync.completeCommand(commandId, false, "{\"ok\":false,\"error\":\"unsupported_command\"}");
  CloudSync.flushCommandAcks();
}
