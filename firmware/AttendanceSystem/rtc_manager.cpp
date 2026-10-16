#include "app.h"
#include <sys/time.h>
#include <stdlib.h>

// DS3231 is the persistent wall-clock source.  The ESP32 system clock is
// seeded from it once at boot and whenever a browser supplies a trusted
// timestamp.  Runtime date/time reads use the system clock, not the I2C bus,
// so web callbacks and the main/SD tasks cannot concurrently hit Wire/RTC.
static volatile bool systemClockValid = false;
static uint32_t lastKnownUtcEpoch = 0;
static const char* TIME_LAST_SYNC_KEY = "time_last_sync";
static const uint32_t RWANDA_UTC_OFFSET = 7200UL;

static time_t rtcLocalToUtc(const DateTime& dt) {
  const uint32_t encodedAsUtc = dt.unixtime();
  return (time_t)(encodedAsUtc - RWANDA_UTC_OFFSET);
}

static bool setSystemClockUtc(time_t utc) {
  if (utc < (time_t)1704067200 || utc > (time_t)4102444800) return false;
  struct timeval tv{};
  tv.tv_sec = utc;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) return false;
  systemClockValid = true;
  return true;
}

static void persistLastKnownUtc(uint32_t utcEpoch) {
  if (!lockPrefs(1000)) return;
  prefs.putUInt(TIME_LAST_SYNC_KEY, utcEpoch);
  unlockPrefs();
  lastKnownUtcEpoch = utcEpoch;
}

static uint32_t loadLastKnownUtc() {
  uint32_t value = 0;
  if (lockPrefs(1000)) {
    value = prefs.getUInt(TIME_LAST_SYNC_KEY, 0);
    unlockPrefs();
  }
  return value;
}

void initializeRTC() {
  if (rtcMutex && xSemaphoreTake(rtcMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[RTC] Mutex unavailable during initialization.");
    rtcAvailable = false;
    rtcTimeValid = false;
    return;
  }

  Wire.begin(RTC_SDA, RTC_SCL);
  Wire.setClock(400000);

  if (!rtc.begin()) {
    rtcAvailable = false;
    rtcTimeValid = false;
    if (rtcMutex) xSemaphoreGive(rtcMutex);
    Serial.println("[RTC] Not detected - falling back to stored/system time.");
    return;
  }

  rtcAvailable = true;
  DateTime now = rtc.now();
  if (now.year() >= RTC_MIN_VALID_YEAR && now.year() <= 2099) {
    rtcTimeValid = true;
    const time_t utc = rtcLocalToUtc(now);
    setSystemClockUtc(utc);
    if (rtcMutex) xSemaphoreGive(rtcMutex);
    persistLastKnownUtc((uint32_t)utc);

    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    Serial.print("[RTC] Detected. Stored time is VALID: ");
    Serial.println(buf);
  } else {
    rtcTimeValid = false;
    if (rtcMutex) xSemaphoreGive(rtcMutex);
    Serial.print("[RTC] Detected, but stored time is INVALID (year=");
    Serial.print(now.year());
    Serial.println("). Waiting for a one-time client sync; a valid RTC will then keep time without the dashboard.");
  }
}

// Only ever called after setupTime() confirms a real NTP answer.
void syncRTCFromNTP() {
  struct tm timeinfo{};
  if (!getLocalTime(&timeinfo, 800)) {
    Serial.println("[RTC] NTP sync failed - no valid network time.");
    return;
  }
  int y = timeinfo.tm_year + 1900;
  if (y < RTC_MIN_VALID_YEAR || y > 2099) {
    Serial.println("[RTC] NTP returned an invalid calendar year.");
    return;
  }

  if (rtcMutex && xSemaphoreTake(rtcMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[RTC] NTP sync skipped - RTC mutex busy.");
    return;
  }
  if (!rtcAvailable) {
    if (rtcMutex) xSemaphoreGive(rtcMutex);
    Serial.println("[RTC] NTP sync skipped - DS3231 unavailable.");
    return;
  }
  rtc.adjust(DateTime(y,
                      timeinfo.tm_mon + 1,
                      timeinfo.tm_mday,
                      timeinfo.tm_hour,
                      timeinfo.tm_min,
                      timeinfo.tm_sec));
  if (rtcMutex) xSemaphoreGive(rtcMutex);
  rtcTimeValid = true;
  Serial.print("[RTC] Synchronized from NTP: ");
  Serial.print(y); Serial.print("-");
  Serial.print(timeinfo.tm_mon + 1); Serial.print("-");
  Serial.print(timeinfo.tm_mday); Serial.print(" ");
  Serial.print(timeinfo.tm_hour); Serial.print(":");
  Serial.print(timeinfo.tm_min); Serial.print(":");
  Serial.println(timeinfo.tm_sec);
}

void setupTime() {
  // Rwanda: Central Africa Time (UTC+2), no DST.
  setenv("TZ", TIME_ZONE, 1);
  tzset();

  // initializeRTC() already seeded the system clock from the DS3231.
  if (rtcTimeValid && systemClockValid) {
    Serial.println("[TIME] Rwanda CAT (UTC+2) active; DS3231 is the persistent clock source.");
    return;
  }

  const uint32_t saved = loadLastKnownUtc();
  if (saved >= 1704067200UL && saved <= 4102444800UL) {
    lastKnownUtcEpoch = saved;
    setSystemClockUtc((time_t)saved);
    Serial.println("[TIME] RTC invalid at boot; restored last-known UTC anchor from Preferences.");
    Serial.println("[TIME] This fallback is accurate only while the device remains powered; repair/sync the RTC for power-cycle accuracy.");
  } else {
    systemClockValid = false;
    Serial.println("[TIME] No absolute time is stored yet. A one-time browser/serial sync is required.");
  }
}

// Cloud command path: runs from the Arduino main loop, not an async HTTP
// callback. Unlike the legacy browser queue, this path performs and verifies
// the full DS3231 + system clock update before reporting command success.
bool syncRTCFromCloudEpoch(uint32_t utcEpoch) {
  if (utcEpoch < 1704067200UL || utcEpoch > 4102444800UL) {
    Serial.printf("[RTC] Cloud sync rejected invalid UTC epoch: %lu\n", (unsigned long)utcEpoch);
    return false;
  }

  if (!rtcAvailable) {
    Serial.println("[RTC] Cloud sync failed: DS3231 is unavailable.");
    return false;
  }

  const time_t utc = (time_t)utcEpoch;
  const time_t rwandaWallEpoch = utc + (time_t)RWANDA_UTC_OFFSET;
  struct tm localTime{};
  if (gmtime_r(&rwandaWallEpoch, &localTime) == nullptr ||
      localTime.tm_year + 1900 < RTC_MIN_VALID_YEAR ||
      localTime.tm_year + 1900 > 2099) {
    Serial.println("[RTC] Cloud sync failed: UTC-to-Rwanda conversion invalid.");
    return false;
  }

  if (rtcMutex && xSemaphoreTake(rtcMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[RTC] Cloud sync failed: RTC mutex busy.");
    return false;
  }

  rtc.adjust(DateTime(localTime.tm_year + 1900,
                      localTime.tm_mon + 1,
                      localTime.tm_mday,
                      localTime.tm_hour,
                      localTime.tm_min,
                      localTime.tm_sec));
  const DateTime written = rtc.now();
  const bool rtcWriteVerified =
      written.year() == localTime.tm_year + 1900 &&
      written.month() == localTime.tm_mon + 1 &&
      written.day() == localTime.tm_mday &&
      written.hour() == localTime.tm_hour &&
      written.minute() == localTime.tm_min &&
      abs((int)written.second() - (int)localTime.tm_sec) <= 2;

  if (rtcMutex) xSemaphoreGive(rtcMutex);

  if (!rtcWriteVerified) {
    Serial.println("[RTC] Cloud sync failed: DS3231 read-back verification did not match.");
    return false;
  }

  if (!setSystemClockUtc(utc)) {
    Serial.println("[RTC] Cloud sync failed: ESP32 system clock update failed.");
    return false;
  }

  rtcTimeValid = true;
  persistLastKnownUtc(utcEpoch);
  bootSnapshotDirty = true;

  Serial.printf("[RTC] Cloud sync verified Rwanda time: %04d-%02d-%02d %02d:%02d:%02d\n",
                localTime.tm_year + 1900,
                localTime.tm_mon + 1,
                localTime.tm_mday,
                localTime.tm_hour,
                localTime.tm_min,
                localTime.tm_sec);
  return true;
}

// Browser supplies UTC epoch.  HTTP callbacks MUST stay lightweight: the
// actual DS3231/I2C, system-clock, and Preferences operations are performed by
// loop() in serviceBrowserTimeSync(). The queue is length 1 and xQueueOverwrite()
// means the newest browser timestamp always wins without blocking the network.
bool syncRTCFromBrowserEpoch(uint32_t utcEpoch) {
  if (utcEpoch < 1704067200UL || utcEpoch > 4102444800UL) {
    Serial.printf("[RTC] Browser epoch rejected: %lu\n", (unsigned long)utcEpoch);
    return false;
  }
  if (!browserTimeSyncQueue) {
    Serial.println("[RTC] Browser sync queue unavailable.");
    return false;
  }
  if (xQueueOverwrite(browserTimeSyncQueue, &utcEpoch) != pdPASS) {
    Serial.println("[RTC] Browser sync queue write failed.");
    return false;
  }
  return true;
}

void serviceBrowserTimeSync() {
  if (!browserTimeSyncQueue) return;

  uint32_t utcEpoch = 0;
  if (xQueueReceive(browserTimeSyncQueue, &utcEpoch, 0) != pdTRUE) return;
  if (utcEpoch < 1704067200UL || utcEpoch > 4102444800UL) return;

  const time_t browserUtc = (time_t)utcEpoch;
  const time_t currentUtc = time(nullptr);

  // If the ESP32 system clock is already close to the browser clock, there is
  // nothing to correct. This avoids unnecessary I2C/NVS writes on every login.
  if (systemClockValid && currentUtc > 0) {
    long long diff = (long long)currentUtc - (long long)browserUtc;
    if (diff < 0) diff = -diff;
    if (diff <= 5LL) {
      Serial.println("[RTC] Browser sync skipped: system clock already within 5 seconds.");
      return;
    }
  }

  if (rtcAvailable) {
    if (rtcMutex && xSemaphoreTake(rtcMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      Serial.println("[RTC] Browser sync deferred - RTC mutex busy.");
      xQueueOverwrite(browserTimeSyncQueue, &utcEpoch);
      return;
    }

    const time_t rwandaWallEpoch = browserUtc + (time_t)RWANDA_UTC_OFFSET;
    struct tm localTime{};
    const bool converted = gmtime_r(&rwandaWallEpoch, &localTime) != nullptr;
    if (!converted) {
      if (rtcMutex) xSemaphoreGive(rtcMutex);
      Serial.println("[RTC] Browser sync conversion failed.");
      return;
    }

    rtc.adjust(DateTime(localTime.tm_year + 1900,
                        localTime.tm_mon + 1,
                        localTime.tm_mday,
                        localTime.tm_hour,
                        localTime.tm_min,
                        localTime.tm_sec));

    const bool clockOk = setSystemClockUtc(browserUtc);
    if (rtcMutex) xSemaphoreGive(rtcMutex);

    if (!clockOk) {
      Serial.println("[RTC] Browser sync failed while setting system clock.");
      return;
    }

    rtcTimeValid = true;
    persistLastKnownUtc(utcEpoch);
    bootSnapshotDirty = true;

    Serial.printf("[RTC] Browser synchronized Rwanda time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  localTime.tm_year + 1900,
                  localTime.tm_mon + 1,
                  localTime.tm_mday,
                  localTime.tm_hour,
                  localTime.tm_min,
                  localTime.tm_sec);
    return;
  }

  // No DS3231: keep the system clock usable while powered and retain the last
  // known UTC anchor for the normal fallback path on next boot.
  if (setSystemClockUtc(browserUtc)) {
    persistLastKnownUtc(utcEpoch);
    bootSnapshotDirty = true;
    Serial.println("[RTC] Browser synchronized system clock; DS3231 unavailable.");
  }
}

// Runtime reads use the ESP32 system clock after it has been seeded from the
// RTC. This avoids continuous I2C traffic and, importantly, keeps async web
// callbacks from racing Wire against the Arduino loop. RTC is only consulted
// as a fallback when the system clock is not valid.
bool getCurrentDateTime(struct tm &outTimeinfo) {
  struct tm t{};
  if (systemClockValid && getLocalTime(&t, 5) && (t.tm_year + 1900) >= RTC_MIN_VALID_YEAR) {
    outTimeinfo = t;
    outTimeinfo.tm_isdst = 0;
    return true;
  }

  if (rtcAvailable && rtcTimeValid) {
    if (rtcMutex && xSemaphoreTake(rtcMutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;
    DateTime n = rtc.now();
    if (rtcMutex) xSemaphoreGive(rtcMutex);
    if (n.year() < RTC_MIN_VALID_YEAR || n.year() > 2099) {
      rtcTimeValid = false;
      return false;
    }
    memset(&outTimeinfo, 0, sizeof(outTimeinfo));
    outTimeinfo.tm_year = n.year() - 1900;
    outTimeinfo.tm_mon  = n.month() - 1;
    outTimeinfo.tm_mday = n.day();
    outTimeinfo.tm_hour = n.hour();
    outTimeinfo.tm_min  = n.minute();
    outTimeinfo.tm_sec  = n.second();
    // RTClib uses the same 0=Sunday..6=Saturday convention as struct tm.
    // Populate it explicitly so the weekend gate remains correct even if the
    // system-clock read temporarily fails and getCurrentDateTime() falls back
    // to the DS3231 path.
    outTimeinfo.tm_wday = n.dayOfTheWeek();
    outTimeinfo.tm_isdst = 0;
    return true;
  }

  return false;
}

String uptimeText() { return currentDateTimeText(); }

String currentDateTimeText() {
  struct tm timeinfo{};
  if (getCurrentDateTime(timeinfo)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
  }
  return "UPTIME " + String(millis()/1000);
}

String currentDateOnlyText() {
  struct tm timeinfo{};
  if (!getCurrentDateTime(timeinfo)) return "";
  char buf[12];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &timeinfo);
  return String(buf);
}

// Shared utility helpers. These are declared in app.h and used by the API,
// attendance, fingerprint, reports, and SD modules. Keep them in one place
// so every translation unit links against the same implementation.
String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\n");
  s.replace("\r", "");
  s.replace("</", "<\\/");
  return s;
}

String firstNameOnly(String fullName) {
  fullName.trim();
  if (fullName.length() == 0) return "";
  int spaceIndex = fullName.indexOf(' ');
  if (spaceIndex > 0) return fullName.substring(0, spaceIndex);
  return fullName;
}

String getAttendanceStatus(bool isCheckIn) {
  struct tm timeinfo{};
  if (!getCurrentDateTime(timeinfo)) {
    return isCheckIn ? "CHECK IN - TIME UNKNOWN" : "CHECK OUT - TIME UNKNOWN";
  }

  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int checkinMin = CHECKIN_ON_TIME_MINUTES;
  int checkoutMin = CHECKOUT_ON_TIME_MINUTES;

  // Configuration is shared with the dashboard/runtime state. Snapshot the
  // two values under the state mutex, then classify using the local copy.
  if (lockState(100)) {
    checkinMin = cfgCheckinMinutes;
    checkoutMin = cfgCheckoutMinutes;
    unlockState();
  }

  if (isCheckIn) {
    if (currentMinutes < checkinMin) return "CHECK IN - EARLY";
    if (currentMinutes == checkinMin) return "CHECK IN - ON TIME";
    return "CHECK IN - LATE";
  }

  if (currentMinutes < checkoutMin) return "CHECK OUT - LEFT EARLY";
  if (currentMinutes == checkoutMin) return "CHECK OUT - ON TIME";
  return "CHECK OUT - OVERTIME";
}

String attendanceEventTimeText(bool isCheckIn) {
  // Kept as a compatibility wrapper so every caller uses the same authoritative
  // classifier as the live attendance path. The exact default boundaries are
  // 07:55 for check-in and 17:00 for check-out.
  return getAttendanceStatus(isCheckIn);
}