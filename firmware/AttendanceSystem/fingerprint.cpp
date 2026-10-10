#include "app.h"

static bool attendanceBlockedForWeekend(const struct tm &timeinfo) {
  // Rwanda local tm_wday: 0=Sunday ... 6=Saturday.
  // Weekend attendance is blocked for the entire Saturday and entire Sunday because attendance operates Monday-Friday only.
  if (timeinfo.tm_wday == 6) return true;
  if (timeinfo.tm_wday == 0) return true;
  return false;
}

enum WeekendFeedbackPhase : uint8_t {
  WEEKEND_FB_IDLE = 0,
  WEEKEND_FB_PULSE,
  WEEKEND_FB_GAP
};

static WeekendFeedbackPhase weekendFeedbackPhase = WEEKEND_FB_IDLE;
static uint8_t weekendFeedbackPulseCount = 0;
static unsigned long weekendFeedbackNextMs = 0;
static int weekendWarningLatchedId = -1;

static void startWeekendAttendanceFeedback() {
  // Non-blocking: the fingerprint mutex is held by scanFingerprint(), so this
  // routine only schedules the three pulses and returns immediately.
  ledOffAt = 0;
  buzzerOffAt = 0;
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  weekendFeedbackPulseCount = 1;
  weekendFeedbackPhase = WEEKEND_FB_PULSE;
  weekendFeedbackNextMs = millis() + 120UL;
}

void serviceWeekendAttendanceFeedback() {
  if (weekendFeedbackPhase == WEEKEND_FB_IDLE) return;
  const unsigned long now = millis();
  if ((long)(now - weekendFeedbackNextMs) < 0) return;

  if (weekendFeedbackPhase == WEEKEND_FB_PULSE) {
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_RED, LOW);
    if (weekendFeedbackPulseCount >= 3) {
      weekendFeedbackPhase = WEEKEND_FB_IDLE;
      weekendFeedbackNextMs = 0;
      return;
    }
    weekendFeedbackPhase = WEEKEND_FB_GAP;
    weekendFeedbackNextMs = now + 160UL;
    return;
  }

  // GAP -> start the next short pulse.
  ++weekendFeedbackPulseCount;
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  weekendFeedbackPhase = WEEKEND_FB_PULSE;
  weekendFeedbackNextMs = now + 120UL;
}

static String attendanceStatusRwLabel(const String &event) {
  String e = event;
  e.toUpperCase();

  // Check-out has a different Rwandan wording for the exact on-time
  // boundary (17:00) than check-in does. Keep the action prefix in the
  // internal event and translate the user-visible status here.
  if (e.indexOf("CHECK OUT") >= 0) {
    if (e.indexOf("LEFT EARLY") >= 0) return "Utashe kare";
    if (e.indexOf("OVERTIME") >= 0) return "Utashe nyuma y' igihe";
    if (e.indexOf("ON TIME") >= 0) return "Utahiye k' ugihe";
    return "Wavuye";
  }

  if (e.indexOf("CHECK IN") >= 0) {
    if (e.indexOf("EARLY") >= 0) return "Wazindutse";
    if (e.indexOf("ON TIME") >= 0) return "Uziye k' ugihe";
    if (e.indexOf("LATE") >= 0) return "Wakerewe";
    return "Kwinjiye";
  }

  if (e.indexOf("TIME UNKNOWN") >= 0) return "Igihe ntikizwi";
  return "";
}

static void oledWeekendAttendanceBlocked() {
  display.clearDisplay();
  display.setTextWrap(false);
  drawTitleBar("ATTENDANCE");
  drawCenteredText("Muri weekend", 22, 1);
  drawCenteredText("ntabwo attendance", 36, 1);
  drawCenteredText("Ikorwa:", 50, 1);
  display.display();
}

String clipText(String text, uint8_t maxChars) {
  text.trim();
  if (text.length() <= maxChars) return text;
  if (maxChars <= 1) return text.substring(0, maxChars);
  return text.substring(0, maxChars - 1) + ".";
}

void drawCenteredText(String text, int y, uint8_t textSize) {
  text.trim();
  uint8_t maxChars = (textSize == 2 ? 10 : 21);
  text = clipText(text, maxChars);
  int charWidth = 6 * textSize;
  int textWidth = text.length() * charWidth;
  int x = (SCREEN_WIDTH - textWidth) / 2;
  if (x < 0) x = 0;
  display.setTextSize(textSize);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(x, y);
  display.print(text);
}

void drawWiFiIcon(bool connected) {
  int x = 113, y = 2;
  if (connected) {
    display.drawCircle(x + 7, y + 11, 1, SH110X_WHITE);
    display.drawLine(x + 2, y + 8, x + 7, y + 4, SH110X_WHITE);
    display.drawLine(x + 7, y + 4, x + 12, y + 8, SH110X_WHITE);
    display.drawLine(x + 4, y + 10, x + 7, y + 8, SH110X_WHITE);
    display.drawLine(x + 7, y + 8, x + 10, y + 10, SH110X_WHITE);
  } else {
    display.drawRect(x + 1, y + 3, 12, 9, SH110X_WHITE);
    display.drawLine(x, y + 2, x + 14, y + 13, SH110X_WHITE);
    display.drawLine(x + 14, y + 2, x, y + 13, SH110X_WHITE);
  }
}

void drawTitleBar(String title) {
  display.fillRect(0, 0, 128, 16, SH110X_BLACK);
  String shortTitle = clipText(title, 16);
  int textWidth = shortTitle.length() * 6;
  int x = (112 - textWidth) / 2;
  if (x < 0) x = 0;
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(x, 2);
  display.print(shortTitle);
  drawWiFiIcon(wifiDashboardReady());
  display.drawLine(0, 15, 127, 15, SH110X_WHITE);
}

void oledScreen(String title, String line1, String line2, String line3) {
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar(title);
  if (line1.length() > 0) drawCenteredText(line1, 22, 1);
  if (line2.length() > 0) drawCenteredText(line2, 36, 1);
  if (line3.length() > 0) drawCenteredText(line3, 50, 1);
  display.display();
}

void oledBig(String title, String line1, String line2) {
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar(title);
  if (line1.length() > 0) drawCenteredText(line1, 24, 1);
  if (line2.length() > 0) drawCenteredText(line2, 42, 1);
  display.display();
}

void drawMiniFingerprint(int x, int y) {
  display.drawCircle(x + 18, y + 18, 17, SH110X_WHITE);
  display.drawCircle(x + 18, y + 18, 12, SH110X_WHITE);
  display.drawCircle(x + 18, y + 18, 7,  SH110X_WHITE);
  display.fillRect(x - 2, y + 28, 42, 14, SH110X_BLACK);
  display.fillRect(x - 2, y - 2,  7,  44, SH110X_BLACK);
  display.fillRect(x + 32, y - 2, 8,  44, SH110X_BLACK);
  display.drawLine(x + 18, y + 18, x + 18, y + 36, SH110X_WHITE);
  display.drawLine(x + 12, y + 21, x + 10, y + 35, SH110X_WHITE);
  display.drawLine(x + 24, y + 21, x + 28, y + 34, SH110X_WHITE);
}

void drawLoadingDots(int x, int y, uint8_t frame) {
  display.setTextSize(1); display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(x, y); display.print("Please wait");
  for (uint8_t i = 0; i < frame % 4; i++) display.print(".");
}

void drawMovingBar(int y, uint8_t frame) {
  int boxX = 12, boxW = 104, fillW = 22;
  int pos = (frame * 8) % (boxW - fillW);
  display.drawRect(boxX, y, boxW, 8, SH110X_WHITE);
  display.fillRect(boxX + 2 + pos, y + 2, fillW, 4, SH110X_WHITE);
}

void drawProgress(uint8_t percent) {
  if (percent > 100) percent = 100;
  int barX = 10, barY = 52, barW = 108, barH = 9;
  display.drawRect(barX, barY, barW, barH, SH110X_WHITE);
  int fillW = map(percent, 0, 100, 0, barW - 4);
  display.fillRect(barX + 2, barY + 2, fillW, barH - 4, SH110X_WHITE);
  display.setTextSize(1); display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(52, 41); display.print(percent); display.print("%");
}

void oledScanScreen() {
  static uint8_t idleFrame = 0;
  idleFrame++;
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar("SCAN MODE");
  drawMiniFingerprint(8, 22);
  display.setTextSize(1); display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(56, 24); display.print("Place finger");
  display.setCursor(56, 38); display.print("Waiting");
  for (uint8_t i = 0; i < idleFrame % 4; i++) display.print(".");
  int r = 3 + (idleFrame % 4);
  display.drawCircle(103, 50, r, SH110X_WHITE);
  display.display();
}

void oledScanAnimation(String title, String line1, uint16_t durationMs) {
  unsigned long startTime = millis(); uint8_t frame = 0;
  while (millis() - startTime < durationMs) {
    serviceLedBuzzerTimers();
    display.clearDisplay(); display.setTextWrap(false);
    drawTitleBar(title); drawMiniFingerprint(8, 21);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE, SH110X_BLACK);
    display.setCursor(56, 24); display.print(clipText(line1, 11));
    drawLoadingDots(56, 38, frame);
    int scanY = 22 + ((frame * 4) % 32);
    display.drawLine(8, scanY, 45, scanY, SH110X_WHITE);
    display.drawLine(8, scanY + 1, 45, scanY + 1, SH110X_WHITE);
    drawMovingBar(54, frame);
    display.display(); frame++; smartDelay(80);
  }
}

void oledProgressBar(String title, String line1, uint8_t percent) {
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar(title);
  drawCenteredText(line1, 24, 1);
  drawProgress(percent);
  display.display();
}

void oledSuccessAnimation(String line1, String line2) {
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar("SUCCESS");
  display.drawCircle(64, 34, 14, SH110X_WHITE);
  display.drawLine(56, 34, 62, 41, SH110X_WHITE);
  display.drawLine(62, 41, 74, 27, SH110X_WHITE);
  display.drawLine(56, 35, 62, 42, SH110X_WHITE);
  display.drawLine(62, 42, 74, 28, SH110X_WHITE);
  display.display();
  if (line2.length() > 0) oledBig("SUCCESS", line1, line2);
  else oledBig("SUCCESS", line1);
}

void oledErrorAnimation(String line1, String line2) {
  display.clearDisplay(); display.setTextWrap(false);
  drawTitleBar("NOTICE");
  display.drawCircle(64, 34, 13, SH110X_WHITE);
  display.drawLine(56, 26, 72, 42, SH110X_WHITE);
  display.drawLine(72, 26, 56, 42, SH110X_WHITE);
  display.drawLine(57, 26, 73, 42, SH110X_WHITE);
  display.drawLine(73, 26, 57, 42, SH110X_WHITE);
  display.display();
  if (line2.length() > 0) oledBig("TRY AGAIN", line1, line2);
  else oledBig("TRY AGAIN", line1);
}

// =====================================================
// BUTTONS
// =====================================================
bool buttonPressed(uint8_t pin, unsigned long &lastTime) {
  if (digitalRead(pin) == LOW) { if (millis() - lastTime > DEBOUNCE_MS) { lastTime = millis(); return true; } }
  return false;
}

bool cancelButtonPressed() {
  static bool waitingForRelease = false;
  if (waitingForRelease) { if (digitalRead(BTN_CANCEL) == HIGH) waitingForRelease = false; return false; }
  if (buttonPressed(BTN_CANCEL, lastCancelBtnTime)) { waitingForRelease = true; return true; }
  return false;
}

// -----------------------------------------------------------------
// PHYSICAL BUTTONS NO LONGER CONTROL ENROLLMENT.
// Enrollment can only be started or cancelled from the authorized
// dashboard/admin workflow now. This function only watches the
// dashboard-driven cancelEnroll flag (set by the /api/cancel HTTP
// route) - BTN_CANCEL is not read here at all, so a physical button
// press has no power to start or stop enrollment.
static void refreshTemplateCountCacheLocked();

bool checkCancelDuringEnroll() {
  serviceLedBuzzerTimers();
  bool cancelledExternally = false;
  if (lockState()) { cancelledExternally = cancelEnroll; unlockState(); }
  if (cancelledExternally) {
    oledBig("CANCELLED", "Back to scan");
    smartDelay(600);
    return true;
  }
  return false;
}

// =====================================================
// FINGERPRINT SLEEP / WAKE
// =====================================================
// NOTE ON LOCKING: fpSetLed() takes fingerMutex. It must therefore only
// ever be called from code that does NOT already hold that mutex
// (fingerMutex is a plain, non-recursive mutex -- nesting would
// deadlock). scanFingerprint() runs *inside* the lock, so it calls
// fpNoteActivity(), which only touches a timestamp.
void fpSetLed(bool on) {
  if (!fingerprintSensorOk || !fpLedSupported) return;
  if (!lockFinger(200)) return;
  uint8_t r = finger.LEDcontrol(on);
  unlockFinger();

  // Some R30x clones do not implement the LED command. Give up only after
  // three consecutive failures so a single noisy UART reply cannot
  // permanently disable it.
  static uint8_t ledFails = 0;
  if (r == FINGERPRINT_OK) { ledFails = 0; return; }
  if (++ledFails >= 3) {
    fpLedSupported = false;
    Serial.println("[FP] LEDcontrol unsupported - sleep will throttle polling only.");
  }
}

// Called from inside the fingerMutex (hot path). Timestamp only.
void fpNoteActivity() {
  fpAsleep = false;
  fpWakingUp = false;
  fpProbePhase = FP_PROBE_IDLE;
  fpLastActivityMs = millis();
}

// Full wake: safe to call only when fingerMutex is NOT held.
void fpWake() {
  bool was = fpAsleep;
  fpAsleep = false;
  fpWakingUp = false;
  fpProbePhase = FP_PROBE_IDLE;
  fpLastActivityMs = millis();
  if (was) { fpSetLed(true); }
}

void fpSleepNow() {
  if (fpAsleep) return;
  fpAsleep = true;
  fpWakingUp = false;
  fpProbePhase = FP_PROBE_IDLE;
  fpLastProbeMs = millis();
  fpSetLed(false);
  Serial.println(fpTouchMode ? "[FP] SENSOR SLEEP (touch pin, LED off)"
                             : "[FP] SENSOR SLEEP (strobe, LED off)");
}

// ---------------------------------------------------------------------
// Boot-time self test for the JM-101 / R307 touch line.
//
// ROOT CAUSE OF "sensor wakes by itself":
//   GPIO35 is input-only with no internal pull resistor. If the VT /
//   TOUCH-OUT wire is missing or lands on a floating pin, the input
//   is undefined and drifts HIGH on long traces. The old code trusted
//   the pin unconditionally.
//
// We sample the pin for 400 ms at boot (no finger can be present
// during startup). If the pin is active the whole time, it is a
// miswire or a floating pin, so we disable touch mode for this boot.
// ---------------------------------------------------------------------
void fpTouchSelfTest() {
  if (!fpTouchMode) return;

  pinMode(FP_TOUCH_PIN, INPUT);   // no pull-up on GPIO34-39

  int activeSamples = 0;
  const int TOTAL = 40;
  for (int i = 0; i < TOTAL; i++) {
    if (digitalRead(FP_TOUCH_PIN) == FP_TOUCH_ACTIVE) activeSamples++;
    delay(10);
  }

  if (activeSamples >= TOTAL - 2) {
    // Pin active the whole time with no finger present.
    fpTouchMode = false;
    Serial.println("[FP] TOUCH self-test FAILED (pin stuck active).");
    Serial.println("[FP] Check VT/TVcc wiring. Falling back to strobe mode.");
  } else if (activeSamples <= 1) {
    Serial.println("[FP] TOUCH self-test OK (idle LOW, will wake on HIGH).");
  } else {
    Serial.print("[FP] TOUCH self-test: ");
    Serial.print(activeSamples);
    Serial.println("/40 samples active - noisy, but keeping touch mode.");
  }
}

// Returns true when scanFingerprint() should run on this loop pass.
// Never blocks: the LED exposure wait is spread over loop() passes.
bool fpShouldScanNow() {
  if (!fingerprintSensorOk) return false;
  unsigned long now = millis();

  if (!fpAsleep) {
    if (fpWakingUp) {
      // Settling after a touch-triggered wake: LED is already on (set by
      // fpWake() below) but not yet given time to stabilize. Do not scan
      // yet - this is what was missing before.
      if (now - fpWakeStartMs < FP_PROBE_EXPOSURE_MS) return false;
      fpWakingUp = false;
    }
    if (now - fpLastActivityMs >= FP_ACTIVE_TIMEOUT_MS) { fpSleepNow(); return false; }
    return true;                               // ACTIVE: full-speed polling
  }

  // ---- asleep, TOUCH-PIN mode: LED stays fully off, zero blinking ----
  if (fpTouchMode) {
    bool touched = (digitalRead(FP_TOUCH_PIN) == FP_TOUCH_ACTIVE);

    if (!touched) {
      if (fpTouchHighSince != 0) {
        Serial.println("[FP] TOUCH idle state");
      }
      fpTouchHighSince = 0;
      return false;
    }

    // A real touch is transient. If the pin sits active for half a
    // minute the wire is miswired, so we permanently fall back to
    // strobe for this boot rather than fake-waking forever.
    if (fpTouchHighSince == 0) {
      fpTouchHighSince = now;
      Serial.println("[FP] TOUCH detected");
    } else if (now - fpTouchHighSince >= FP_TOUCH_STUCK_MS) {
      fpTouchMode = false;
      fpTouchHighSince = 0;
      Serial.println("[FP] TOUCH stuck - falling back to strobe mode.");
      return false;
    }

    // ORDER MATTERS: fpWake() clears fpWakingUp, so set it AFTER the
    // call. Do not swap these two lines.
    fpWake();
    fpWakingUp = true;
    fpWakeStartMs = now;
    Serial.println("[FP] SENSOR WAKE");
    return false;                   // settle window before first capture
  }

  // ---- asleep, STROBE mode: brief LED pulse to expose an image ----
  if (fpProbePhase == FP_PROBE_IDLE) {
    if (now - fpLastProbeMs < FP_SLEEP_PROBE_MS) return false;
    fpSetLed(true);
    fpProbeLitMs = now;
    fpProbePhase = FP_PROBE_LIT;
    return false;                              // let the LED settle
  }

  if (now - fpProbeLitMs < FP_PROBE_EXPOSURE_MS) return false;

  fpProbePhase = FP_PROBE_IDLE;
  fpLastProbeMs = now;

  uint8_t p = FINGERPRINT_NOFINGER;
  if (lockFinger(50)) { p = finger.getImage(); unlockFinger(); }
  else return false;                           // sensor busy elsewhere

  if (p == FINGERPRINT_NOFINGER) { fpSetLed(false); return false; }

  fpWake();
  return true;                                 // finger present -> real scan
}

void scanFingerprint() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) {
    // Allow the next physical finger to receive a fresh weekend warning.
    weekendWarningLatchedId = -1;
    return;
  }
  fpNoteActivity();   // real finger contact -> stay ACTIVE
  if (p != FINGERPRINT_OK) { addLog("ERROR", -1, "Could not read fingerprint"); oledErrorAnimation("Could not read", "Place properly"); beginScanResultHold(500); return; }
  Serial.println("[FP] SENSOR READY");

  oledScreen("SCANNING", "Hold finger", "Reading...");
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { addLog("ERROR", -1, "Fingerprint image not clear"); oledErrorAnimation("Finger not clear", "Try again"); beginScanResultHold(500); return; }

  if (cachedTemplateCount == 0) refreshTemplateCountCacheLocked();
  if (cachedTemplateCount == 0) { addLog("NOT FOUND", -1, "No fingerprints stored"); oledBig("NOT FOUND", "No saved print"); beginScanResultHold(900); return; }

  oledScreen("SEARCHING", "Please wait", "Checking...");
  p = finger.fingerSearch(1);
  if (p != FINGERPRINT_OK) p = finger.fingerFastSearch();

  if (p == FINGERPRINT_OK) {
    int id = finger.fingerID;
    PersonProfile profile = loadProfile(id);
    String fullName = profile.name;
    String displayName = firstNameOnly(fullName);
    if (displayName.length() == 0) displayName = "ID " + String(id);
    String logName = fullName.length() ? fullName : ("ID " + String(id));

    if (id >= 1 && id <= MAX_FINGERPRINTS) {
      // ---- Section 4: TIME VALIDATION ------------------------------
      // Never create an attendance record without a verified RTC/NTP
      // date. If time is unavailable, log the failure to system.log
      // (via addLog -> non-attendance routing) and stop - no record,
      // no state change, no invented timestamp.
      struct tm nowTi;
      if (!getCurrentDateTime(nowTi)) {
        addLog("TIME INVALID", id, logName + ": time source unavailable, attendance not recorded");
        oledErrorAnimation("Time unknown", "Not recorded");
        beginScanResultHold(700);
        return;
      }

      // ---- Section 5: WEEKEND ATTENDANCE BLOCK ---------------------
      // Weekend scans never create attendance.csv rows and never change
      // insideState[]. The user gets the exact weekend notice plus 3 beeps.
      if (attendanceBlockedForWeekend(nowTi)) {
        const String weekendText = "Muri weekend ntabwo attendance Ikorwa:";
        if (weekendWarningLatchedId != id) {
          weekendWarningLatchedId = id;
          addLog(weekendText, id, weekendText);
          startWeekendAttendanceFeedback();
        }
        oledWeekendAttendanceBlocked();
        beginScanResultHold(1200);
        return;
      }

      // ---- Section 6: DUPLICATE PROTECTION -------------------------
      // Ignore a second scan from the same finger within the debounce
      // window so an accidental double-tap can't create a spurious
      // CHECK_IN immediately followed by a CHECK_OUT.
      unsigned long nowMs = millis();
      unsigned long lastMs = 0;
      if (lockState()) { lastMs = lastAttendanceActionMs[id]; unlockState(); }
      if (lastMs != 0 && (nowMs - lastMs) < ATTENDANCE_DEBOUNCE_MS) {
        oledBig("ALREADY", "Recorded", displayName);
        beginScanResultHold(600);
        return;
      }

      if (!storageCanAcceptAttendance()) {
        addLog("ERROR", id, logName + ": SD storage is not ready, attendance not recorded");
        attendanceFailureFeedback();
        oledErrorAnimation("SD STORAGE", "Not ready");
        beginScanResultHold(900);
        return;
      }

      bool wasInside = false;
      if (lockState()) { wasInside = insideState[id]; unlockState(); }
      if (!wasInside) {
        String attendanceStatus = getAttendanceStatus(true);
        String attendanceTime = currentDateTimeText();
        bool saved=addLog(attendanceStatus, id, logName + " checked in at " + attendanceTime + " - " + attendanceStatus);
        if (!saved) {
          attendanceFailureFeedback();
          oledErrorAnimation("SD SAVE", "Failed / pending");
          beginScanResultHold(1200);
          return;
        }
        attendanceSuccessFeedback();
        oledSuccessAnimation(attendanceStatusRwLabel(attendanceStatus), displayName);
      } else {
        String attendanceStatus = getAttendanceStatus(false);
        String attendanceTime = currentDateTimeText();
        bool saved=addLog(attendanceStatus, id, logName + " checked out at " + attendanceTime + " - " + attendanceStatus);
        if (!saved) {
          attendanceFailureFeedback();
          oledErrorAnimation("SD SAVE", "Failed / pending");
          beginScanResultHold(1200);
          return;
        }
        attendanceSuccessFeedback();
        oledSuccessAnimation(attendanceStatusRwLabel(attendanceStatus), displayName);
      }
    } else {
      addLog("MATCH", id, logName + " matched");
      oledSuccessAnimation("Matched", displayName);
    }
    Serial.println("[FP] MATCH");
    beginScanResultHold(700);
    return;
  }

  if (p == FINGERPRINT_NOTFOUND) {
    attendanceFailureFeedback();
    addLog("NOT FOUND", -1, "No matching fingerprint");
    oledErrorAnimation("Not found", "No match");
    Serial.println("[FP] NO MATCH");
    beginScanResultHold(600);
    return;
  }

  attendanceFailureFeedback();
  addLog("ERROR", -1, "Fingerprint search failed");
  oledErrorAnimation("Search failed", "Try again");
  Serial.println("[FP] NO MATCH");
  beginScanResultHold(600);
}

// =====================================================
// ENROLL
// =====================================================
uint8_t enrollFingerprint(uint16_t id) {
  int p = -1;
  oledProgressBar("ENROLLING", "Place finger", 10);
  while (p != FINGERPRINT_OK) {
    if (checkCancelDuringEnroll()) return 255;
    p = finger.getImage();
    if (p == FINGERPRINT_OK) { oledProgressBar("ENROLLING", "Checking", 20); smartDelay(120); break; }
    else if (p == FINGERPRINT_NOFINGER) { /* waiting */ }
    else { oledErrorAnimation("Could not read", "Try again"); return p; }
    smartDelay(40);
  }
  if (checkCancelDuringEnroll()) return 255;
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { oledErrorAnimation("Finger not clear", "Try again"); return p; }

  if (cachedTemplateCount == 0) refreshTemplateCountCache();
  if (cachedTemplateCount > 0) {
    oledProgressBar("CHECKING", "Existing", 30);
    uint8_t searchResult = finger.fingerSearch(1);
    if (searchResult != FINGERPRINT_OK) searchResult = finger.fingerFastSearch();
    if (searchResult == FINGERPRINT_OK) {
      if (lockState()) { duplicateFound = true; unlockState(); }
      PersonProfile existingProfile = loadProfile(finger.fingerID);
      String existingName = firstNameOnly(existingProfile.name);
      if (existingName.length() == 0) existingName = "ID " + String(finger.fingerID);
      addLog("DUPLICATE", finger.fingerID, "Already enrolled");
      oledBig("ALREADY", existingName);
      smartDelay(2000);
      waitForFingerRemove();
      return 255;
    }
  }

  oledProgressBar("ENROLLING", "First scan OK", 40);
  smartDelay(120);
  oledProgressBar("ENROLLING", "Remove finger", 50);
  while (finger.getImage() != FINGERPRINT_NOFINGER) { if (checkCancelDuringEnroll()) return 255; smartDelay(40); }
  smartDelay(100);
  p = -1;
  oledProgressBar("ENROLLING", "Same finger", 65);
  while (p != FINGERPRINT_OK) {
    if (checkCancelDuringEnroll()) return 255;
    p = finger.getImage();
    if (p == FINGERPRINT_OK) { oledProgressBar("ENROLLING", "Second OK", 75); smartDelay(120); break; }
    else if (p == FINGERPRINT_NOFINGER) { /* waiting */ }
    else { oledErrorAnimation("Could not read", "Try again"); return p; }
    smartDelay(40);
  }
  if (checkCancelDuringEnroll()) return 255;
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { oledErrorAnimation("Finger not clear", "Try again"); return p; }

  oledProgressBar("PROCESSING", "Creating", 85);
  p = finger.createModel();
  if (p == FINGERPRINT_ENROLLMISMATCH) { oledErrorAnimation("Mismatch", "Use same finger"); return p; }
  if (p != FINGERPRINT_OK) { oledErrorAnimation("Could not save", "Try again"); return p; }

  oledProgressBar("SAVING", "Please wait", 95);
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) { oledProgressBar("SAVING", "Complete", 100); smartDelay(200); return FINGERPRINT_OK; }
  oledErrorAnimation("Memory problem", "Not saved");
  return p;
}

// =====================================================
// FEEDBACK / PHASE
// =====================================================
void attendanceSuccessFeedback() {
  digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, HIGH); digitalWrite(BUZZER_PIN, HIGH);
  unsigned long now = millis();
  ledOffAt = now + 1000; buzzerOffAt = now + 1000;
}
void attendanceFailureFeedback() {
  digitalWrite(LED_GREEN, LOW); digitalWrite(LED_RED, HIGH); digitalWrite(BUZZER_PIN, HIGH);
  unsigned long now = millis();
  ledOffAt = now + 400; buzzerOffAt = now + 400;
}
void beginScanResultHold(unsigned long holdMs) {
  scanResultPhase = SCAN_RESULT_HOLD;
  scanResultUntil = millis() + holdMs;
}
void serviceScanResultPhase() {
  if (scanResultPhase == SCAN_RESULT_NONE) return;
  unsigned long now = millis();
  if (scanResultPhase == SCAN_RESULT_HOLD) {
    if ((long)(now - scanResultUntil) >= 0) { scanResultPhase = SCAN_RESULT_WAIT_LIFT; scanResultUntil = now + 5000; }
    return;
  }
  if (scanResultPhase == SCAN_RESULT_WAIT_LIFT) {
    uint8_t img = 0xFF;
    if (!lockFinger(50)) return;
    img = finger.getImage();
    unlockFinger();
    if (img == FINGERPRINT_NOFINGER) {
      // The result-hold state consumes the lift-off read itself, so clear the
      // weekend warning latch here as well. Otherwise the same person could
      // lift and immediately re-scan without receiving the required warning.
      weekendWarningLatchedId = -1;
      scanResultPhase = SCAN_RESULT_NONE;
      oledScanScreen();
    } else if ((long)(now - scanResultUntil) >= 0) {
      // Timeout alone does not prove the finger was lifted; keep the latch so
      // a held finger cannot retrigger three beeps every few seconds.
      scanResultPhase = SCAN_RESULT_NONE;
      oledScanScreen();
    }
  }
}

// =====================================================
// HELPERS
// =====================================================
void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) { serviceLedBuzzerTimers(); delay(1); }
}
void waitForFingerRemove() {
  uint8_t p = finger.getImage();
  while (p != FINGERPRINT_NOFINGER) { serviceLedBuzzerTimers(); delay(50); p = finger.getImage(); }
}
static constexpr uint8_t FP_SLOT_EMPTY_CODE = 0x0C; // sensor: invalid/empty template at location

static bool isFingerprintTransportError(uint8_t code) {
  return code == FINGERPRINT_PACKETRECIEVEERR ||
         code == FINGERPRINT_BADPACKET ||
         code == FINGERPRINT_TIMEOUT;
}

static uint8_t loadFingerprintModelChecked(uint16_t id) {
  uint8_t code = finger.loadModel(id);
  if (!isFingerprintTransportError(code)) return code;

  // One bounded retry separates a transient UART failure from a real slot
  // result. Never classify a transport failure as an empty slot.
  delay(8);
  const uint8_t retry = finger.loadModel(id);
  if (isFingerprintTransportError(retry)) {
    Serial.printf("[FP] Slot scan transport error at ID %u (0x%02X); stopping free-slot scan safely.\n",
                  (unsigned)id, (unsigned)retry);
  }
  return retry;
}

uint16_t findNextFreeFrom(uint16_t start) {
  if (start < 1) start = 1;
  if (start > MAX_FINGERPRINTS) return 0;
  for (uint16_t id = start; id <= MAX_FINGERPRINTS; id++) {
    const uint8_t p = loadFingerprintModelChecked(id);
    if (p == FINGERPRINT_OK) continue;
    if (p == FP_SLOT_EMPTY_CODE) return id;
    if (isFingerprintTransportError(p)) return 0xFFFF;
    // Unknown sensor status is not safe evidence of an empty slot.
    Serial.printf("[FP] ID %u returned slot status 0x%02X; treating as occupied/unknown.\n",
                  (unsigned)id, (unsigned)p);
  }
  return 0;
}
void refreshTemplateCountSerialOnly() {
  uint8_t p = finger.getTemplateCount();
  if (p == FINGERPRINT_OK) { cachedTemplateCount = finger.templateCount; Serial.print("Stored fingerprints: "); Serial.println(cachedTemplateCount); }
  else Serial.println("Could not read template count.");
}

static void refreshTemplateCountCacheLocked() {
  uint8_t p = finger.getTemplateCount();
  if (p == FINGERPRINT_OK) cachedTemplateCount = finger.templateCount;
}

void refreshTemplateCountCache() {
  if (!lockFinger(100)) return;
  refreshTemplateCountCacheLocked();
  unlockFinger();
}

void rebuildRegisteredCache() {
  String json = "[";
  bool first = true;
  uint16_t firstFree = 0;
  for (uint16_t id = 1; id <= MAX_FINGERPRINTS; id++) {
    const uint8_t p = loadFingerprintModelChecked(id);
    if (p == FINGERPRINT_OK) {
      PersonProfile profile = loadProfile(id);
      if (!first) json += ",";
      json += "{";
      json += "\"id\":" + String(id) + ",";
      json += "\"name\":\"" + jsonEscape(profile.name) + "\",";
      json += "\"collegeId\":\"" + jsonEscape(profile.collegeId) + "\",";
      json += "\"role\":\"" + jsonEscape(profile.role) + "\",";
      json += "\"department\":\"" + jsonEscape(profile.department) + "\",";
      json += "\"inside\":" + String(insideState[id] ? "true" : "false");
      json += "}";
      first = false;
    } else if (p == FP_SLOT_EMPTY_CODE && firstFree == 0) {
      // Only the sensor's known empty/invalid-template code is accepted as
      // evidence that a slot is available. Communication/unknown errors are
      // deliberately fail-closed to avoid overwriting a real fingerprint.
      firstFree = id;
    } else if (isFingerprintTransportError(p)) {
      Serial.printf("[FP] Registered-cache scan stopped safely at ID %u due to transport error 0x%02X.\n",
                    (unsigned)id, (unsigned)p);
      scanFirstFreeId = 0;
      return;
    }
  }
  json += "]";
  cachedRegisteredJson = json;
  scanFirstFreeId = firstFree;
}

void appendRegisteredEntry(uint16_t id, PersonProfile p) {
  if (!lockState(500)) return;
  if (cachedRegisteredJson.endsWith("]")) cachedRegisteredJson = cachedRegisteredJson.substring(0, cachedRegisteredJson.length() - 1);
  if (cachedRegisteredJson.length() > 1 && cachedRegisteredJson.endsWith("}")) cachedRegisteredJson += ",";
  cachedRegisteredJson += "{";
  cachedRegisteredJson += "\"id\":" + String(id) + ",";
  cachedRegisteredJson += "\"name\":\"" + jsonEscape(p.name) + "\",";
  cachedRegisteredJson += "\"collegeId\":\"" + jsonEscape(p.collegeId) + "\",";
  cachedRegisteredJson += "\"role\":\"" + jsonEscape(p.role) + "\",";
  cachedRegisteredJson += "\"department\":\"" + jsonEscape(p.department) + "\",";
  cachedRegisteredJson += "\"inside\":" + String(insideState[id] ? "true" : "false");
  cachedRegisteredJson += "}]";
  unlockState();
}

void removeRegisteredEntry(uint16_t id) {
  if (!lockState(500)) return;
  String needle = "{\"id\":" + String(id) + ",";
  int pos = cachedRegisteredJson.indexOf(needle);
  if (pos < 0) { unlockState(); return; }
  int end = cachedRegisteredJson.indexOf('}', pos);
  if (end < 0) { unlockState(); return; }
  int removeStart = pos, removeEnd = end + 1;
  if (removeEnd < (int)cachedRegisteredJson.length() && cachedRegisteredJson[removeEnd] == ',') removeEnd++;
  else if (removeStart > 1 && cachedRegisteredJson[removeStart - 1] == ',') removeStart--;
  cachedRegisteredJson = cachedRegisteredJson.substring(0, removeStart) + cachedRegisteredJson.substring(removeEnd);
  unlockState();
}


void updateRegisteredInsideCache(uint16_t id, bool inside) {
  if(id<1 || id>MAX_FINGERPRINTS) return;
  if(!lockState(150)) return;
  String needle = "{\"id\":" + String(id) + ",";
  int pos = cachedRegisteredJson.indexOf(needle);
  if(pos >= 0) {
    int valuePos = cachedRegisteredJson.indexOf("\"inside\":", pos);
    int endPos = valuePos >= 0 ? cachedRegisteredJson.indexOf('}', valuePos) : -1;
    if(valuePos >= 0 && endPos > valuePos) {
      const int boolStart = valuePos + 9;
      const String replacement = inside ? "true" : "false";
      cachedRegisteredJson = cachedRegisteredJson.substring(0, boolStart) +
                            replacement + cachedRegisteredJson.substring(endPos);
    }
  }
  unlockState();
}

void setMatchingSecuritySerialOnly() {
  int lvl; if (lockState()) { lvl = cfgSecurityLevel; unlockState(); } else lvl = MATCH_SECURITY_LEVEL;
  Serial.print("Setting security level to "); Serial.println(lvl);
  uint8_t p = finger.setSecurityLevel(lvl);
  if (p == FINGERPRINT_OK) Serial.println("Security level updated.");
  else Serial.println("Could not update security level.");
  delay(50);
  finger.getParameters();
  Serial.print("Current security level: "); Serial.println(finger.security_level);
}

void printSensorParameters() {
  finger.getParameters();
  Serial.println();
  Serial.println("Sensor parameters:");
  Serial.print("Status: 0x"); Serial.println(finger.status_reg, HEX);
  Serial.print("System ID: 0x"); Serial.println(finger.system_id, HEX);
  Serial.print("Capacity: "); Serial.println(finger.capacity);
  Serial.print("Security level: "); Serial.println(finger.security_level);
  Serial.print("Device address: "); Serial.println(finger.device_addr, HEX);
  Serial.print("Packet length: "); Serial.println(finger.packet_len);
  Serial.print("Baud rate: "); Serial.println(finger.baud_rate);
  Serial.println();

}