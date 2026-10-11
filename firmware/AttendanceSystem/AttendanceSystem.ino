#include "app.h"
#include <esp_system.h>
#include "mbedtls/sha256.h"

void setup() {
  Serial.begin(115200);
  Serial.printf("[BUILD] Firmware %s\n", FIRMWARE_VERSION);
  delay(150);

  connectWiFi();
  wifiWasConnected = wifiDashboardReady();
  Serial.printf("[WIFI] boot network state: %s ip=%s freeHeap=%u\n",
                wifiDashboardReady() ? "CONNECTED" : "WAITING",
                wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                (unsigned)ESP.getFreeHeap());

  pinMode(BTN_CANCEL, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
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
  attendanceQueue  = xQueueCreate(16, sizeof(AttendanceEvent));
  sdRequestQueue   = xQueueCreate(2,  sizeof(SdBulkRequest));
  lessonAppendQueue= xQueueCreate(8,  sizeof(LessonAppend));
  systemLogQueue   = xQueueCreate(32, sizeof(SystemLogEvent));
  browserTimeSyncQueue = xQueueCreate(1, sizeof(uint32_t));

  prefs.begin("fps_db", false);
  loadAttendanceJournal();
  loadPresenceState();
  loadAuthAccount();
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

  initializeSD();
  ensureLessonsCsv();
  ensureSystemLog();

  Serial.printf("[WIFI] before HTTP routes: connected=%s ip=%s freeHeap=%u\n",
                wifiDashboardReady() ? "YES" : "NO",
                wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                (unsigned)ESP.getFreeHeap());

  dashboardBootReady = false;
  attendanceBootReady = false;
  initFastStatusCache();
  initBootSnapshotCache();
  initSdHttpBridge();
  setupDashboardRoutes();
  xTaskCreatePinnedToCore(sdTaskFunc,  "sdTask",  6144, nullptr, 1, &sdTaskHandle,  1);
  Serial.printf("[WIFI] after full network setup: connected=%s ip=%s freeHeap=%u\n",
                wifiDashboardReady() ? "YES" : "NO",
                wifiDashboardReady() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                (unsigned)ESP.getFreeHeap());

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
    pinMode(FP_TOUCH_PIN, INPUT);
    fpTouchSelfTest();
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
  Serial.println("[BOOT] AsyncWebServer active; SD task running.");
}

void loop() {
  serviceOTA();
  if (otaInProgress()) {
    delay(1);
    return;
  }

  serviceWiFi();
  serviceBrowserTimeSync();
  serviceDeviceSetupPersistence();
  serviceFastStatusCache();

  serviceLedBuzzerTimers();
  serviceWeekendAttendanceFeedback();
  serviceOledKeepAlive();
  serviceBootSnapshotRefresh();
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

  if (!attendanceBootReady) {
    servicePendingRestart();
    delay(1);
    return;
  }

  bool startRequested = false;
  if (lockState()) { if (!isEnrolling && dashboardEnrollRequest) startRequested = true; unlockState(); }

  if (startRequested) {
    String eName, eCollege, eRole, eDept, eNote;
    uint16_t targetId;
    if (lockState()) {
      dashboardEnrollRequest = false;
      isEnrolling = true; cancelEnroll = false; duplicateFound = false;
      eName=pendingName; eCollege=pendingCollegeId;
      eRole=pendingRole; eDept=pendingDepartment; eNote=pendingNote;
      targetId = nextEnrollID;
      unlockState();
    } else targetId = 0;

    if (targetId == 0) {
      addLog("ERROR", -1, "Fingerprint database full (200/200). Delete a record to free a slot.");
      oledErrorAnimation("DATABASE FULL", "Delete a record");
      smartDelay(1500);
      if (lockState()) { pendingName=pendingCollegeId=pendingRole=pendingDepartment=pendingNote=""; isEnrolling=false; unlockState(); }
      oledScanScreen();
    } else {
      addLog("ENROLL", -1, eName.length()>0 ? ("Enrollment started for " + eName) : "Enrollment started");
      fpWake();
      uint8_t result = enrollFingerprint(targetId);
      bool wasCancelled=false, wasDuplicate=false;
      if (lockState()) { wasCancelled=cancelEnroll; wasDuplicate=duplicateFound; unlockState(); }

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
        uint16_t nextFree = findNextFreeFrom(targetId + 1);
        if (lockState()) {
          if (nextFree == 0xFFFF) {
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
      fpWake();
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
      if (fpShouldScanNow()) {
        if (lockFinger(50)) { scanFingerprint(); unlockFinger(); }
      }
    } else {
      serviceScanResultPhase();
      fpNoteActivity();
    }
  }

  // ---- Cloud sync (non-blocking) ----
  CloudSync.loop();

  // Send a heartbeat every ~60s with current device telemetry.
  static unsigned long lastCloudHb = 0;
  if (CloudSync.isEnabled() && millis() - lastCloudHb >= 60000UL) {
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
  static bool active = false;
  static bool outputOn = false;
  static unsigned long nextTransitionAt = 0;

  const unsigned long now = millis();

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
