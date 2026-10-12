#pragma once
#include <Arduino.h>

// ESP32 Arduino Core 3.x: keep AsyncTCP service work on the same core as
// the ESP-IDF TCP/IP stack instead of allowing task migration. The full
// application already pins the SD task and Arduino loop work to core 1;
// keeping AsyncTCP on core 0 prevents ACK/response service from competing
// with those application tasks. Respect any explicit build-system override.
#ifndef CONFIG_ASYNC_TCP_RUNNING_CORE
#define CONFIG_ASYNC_TCP_RUNNING_CORE 0
#endif

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_Fingerprint.h>
#include <RTClib.h>

#include "native_http_compat.h"
#include <ESPmDNS.h>
#include <NetworkUdp.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include "config.h"
#include "cloud_sync.h"

// ---------- Core data types (single source of truth) ----------
struct PersonProfile { String name, collegeId, role, department, note; };
struct AttendanceLog {
  String timeText, event, name, collegeId, role, department, message;
  int id = -1;
  uint32_t seq = 0;
  bool savedToSd = false;
};
struct AttendanceEvent { char row[320]; uint32_t seq; };
struct SystemLogEvent { char row[320]; uint32_t seq; };
struct LessonAppend {
  char date[12]; char time[10]; int fingerprintId;
  char name[64]; char collegeId[24]; char role[16];
  char department[40]; char type[24]; char lesson[260];
  uint8_t journalSlot = 0xFF;
};
struct SdBulkRequest {
  uint32_t id;
  uint8_t type;
  char date[12];
  char dateTo[12];
  QueueHandle_t replyQueue = nullptr;
};
struct SdBulkChunk {
  uint32_t id;
  char data[512];
  size_t len;
  bool done;
  bool ok;
};
struct PendingRow { String csv; uint32_t seq; bool recovery=false; };
enum FpProbePhase { FP_PROBE_IDLE, FP_PROBE_LIT };
enum ScanResultPhase { SCAN_RESULT_NONE, SCAN_RESULT_HOLD, SCAN_RESULT_WAIT_LIFT };

// ---------- Shared globals ----------
extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;
extern const char* TIME_ZONE;
extern AsyncWebServer server;
extern Preferences prefs;
extern Adafruit_SH1106G display;
extern HardwareSerial fingerSerial;
extern Adafruit_Fingerprint finger;
extern RTC_DS3231 rtc;
extern volatile bool rtcAvailable, rtcTimeValid, wifiWasConnected;
extern unsigned long lastEnrollBtnTime, lastCancelBtnTime;
extern volatile bool sdAvailable;
extern volatile bool mainSdBuzzerWarningPending;
extern uint8_t sdConsecutiveFailures;
extern uint8_t sdWriteConsecutiveFailures;
extern uint8_t sdHealthConsecutiveFailures;
extern const uint8_t SD_FAILURE_THRESHOLD;
extern uint32_t sdGeneration;
extern unsigned long sdBackoffMs, lastSdRemountAttempt, lastSdWriteAttempt, sdLastOpenFailMs;
extern const unsigned long SD_BACKOFF_MAX_MS, SD_WRITE_INTERVAL_MS, SD_OPEN_RETRY_DELAY_MS;
extern uint32_t sdRecordCount, sdLessonCount;
extern volatile bool attendanceJournalActive;
extern volatile uint32_t attendanceJournalSeq;
extern volatile bool attendanceCommitAck;
extern volatile bool attendanceJournalRecovery;
extern volatile uint32_t attendanceStateAppliedSeq;
extern uint64_t sdTotalBytesCache, sdUsedBytesCache, sdCsvBytesCache, sdLessonsBytesCache;
extern unsigned long sdStatAtMs, sdUsedBytesAtMs;
extern bool sdStatValid;
extern uint32_t sdMountedClockHz;
extern const unsigned long SD_STAT_TTL_MS, SD_USEDBYTES_TTL_MS;
extern PendingRow pendingRows[MAX_PENDING];
extern int pendingHead, pendingTail, pendingUsed, pendingDropped, pendingQueueDropped;
extern int cfgCheckinMinutes, cfgCheckoutMinutes, cfgSecurityLevel;
extern String cfgDeviceName, cfgTimezone;
extern bool fingerprintSensorOk, ntpSynced;
extern volatile bool fingerprintSecurityDirty;
extern volatile bool deviceSetupPersistPending;
extern bool fpAsleep, fpLedSupported, fpWakingUp, fpTouchMode;
extern FpProbePhase fpProbePhase;
extern unsigned long fpLastActivityMs, fpLastProbeMs, fpProbeLitMs, fpTouchHighSince, fpWakeStartMs;
extern uint16_t nextEnrollID, cachedTemplateCount, scanFirstFreeId;
extern String cachedRegisteredJson;
void updateRegisteredInsideCache(uint16_t id, bool inside);
void initFastStatusCache();
extern int entryCount, exitCount, lastEntryId, ramDropped, tickLastRamDroppedSeen;
extern bool insideState[MAX_FINGERPRINTS+1];
extern unsigned long lastAttendanceActionMs[MAX_FINGERPRINTS+1];
extern int cachedLastEntryJsonForId;
extern String cachedLastEntryJson;
extern String pendingName, pendingCollegeId, pendingRole, pendingDepartment, pendingNote;
#define AUTH_MAX_SESSIONS 4
#define AUTH_IDLE_TIMEOUT_MS (8UL * 60UL * 60UL * 1000UL)
struct AuthSession { char token[65]; uint32_t issuedAtMs; uint32_t lastSeenMs; bool active; };
extern String authUsername, authPasswordHash, activeSessionToken;
extern AuthSession authSessions[AUTH_MAX_SESSIONS];
extern uint8_t loginFailedAttempts;
extern unsigned long loginBlockedUntilMs;
extern bool isEnrolling, cancelEnroll, duplicateFound, dashboardEnrollRequest;
extern ScanResultPhase scanResultPhase;
extern unsigned long scanResultUntil, ledOffAt, buzzerOffAt;
extern AttendanceLog logs[MAX_LOGS];
extern int logCount, logWriteIndex;
extern uint32_t logSeqCounter;
extern SemaphoreHandle_t sharedStateMutex, prefsMutex, fingerMutex, ramLogSnapshotMutex, rtcMutex, authSessionMutex;
extern QueueHandle_t attendanceQueue, systemLogQueue, lessonAppendQueue, sdRequestQueue, browserTimeSyncQueue;
extern SystemLogEvent systemLogFallback[4];
extern uint8_t systemLogFallbackCount;
extern TaskHandle_t sdTaskHandle;
extern bool pendingRestart;
extern volatile bool bootSnapshotDirty;
extern volatile bool dashboardBootReady;
extern volatile bool attendanceBootReady;
extern unsigned long pendingRestartAtMs;

inline bool wifiDashboardReady() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}
inline IPAddress wifiDashboardIP() { return WiFi.localIP(); }
inline bool rowMatchesDate(const String &timeText, const char *ymd) {
  if(!ymd || !ymd[0]) return true;
  if(timeText.length()<10) return false;
  return strncmp(timeText.c_str(), ymd, 10)==0;
}
inline bool lockState(unsigned long ms=250){ return sharedStateMutex && xSemaphoreTake(sharedStateMutex,pdMS_TO_TICKS(ms))==pdTRUE; }
inline void unlockState(){ if(sharedStateMutex) xSemaphoreGive(sharedStateMutex); }
inline bool lockPrefs(unsigned long ms=500){ return prefsMutex && xSemaphoreTake(prefsMutex,pdMS_TO_TICKS(ms))==pdTRUE; }
inline void unlockPrefs(){ if(prefsMutex) xSemaphoreGive(prefsMutex); }
inline bool lockFinger(unsigned long ms=3000){ return fingerMutex && xSemaphoreTake(fingerMutex,pdMS_TO_TICKS(ms))==pdTRUE; }
inline void unlockFinger(){ if(fingerMutex) xSemaphoreGive(fingerMutex); }

inline uint32_t nextBulkId(){
  static uint32_t counter=0;
  static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);
  uint32_t id=++counter;
  if(id==0) id=++counter;
  portEXIT_CRITICAL(&mux);
  return id;
}
inline SdBulkRequest makeBulkReq(uint32_t id,uint8_t type,const char* dateStr="",const char* dateToStr=""){
  SdBulkRequest r{}; r.id=id; r.type=type;
  if(dateStr&&dateStr[0]){ strncpy(r.date,dateStr,sizeof(r.date)-1); r.date[sizeof(r.date)-1]='\0'; }
  if(dateToStr&&dateToStr[0]){ strncpy(r.dateTo,dateToStr,sizeof(r.dateTo)-1); r.dateTo[sizeof(r.dateTo)-1]='\0'; }
  return r;
}

// ---------- Shared function contracts ----------
void setupDashboardRoutes();
void servicePendingRestart();
void registerApiRoutes();
void registerDashboardRoutes();
void connectWiFi();
void serviceWiFi();
void setupOTA();
void serviceOTA(); bool otaInProgress();
void setupTime(); void serviceBrowserTimeSync();
void initializeRTC();
void syncRTCFromNTP();
bool syncRTCFromBrowserEpoch(uint32_t utcEpoch);
bool getCurrentDateTime(struct tm &outTimeinfo);
String uptimeText(); String currentDateTimeText(); String currentDateOnlyText(); bool isValidDateStr(const String &s);
String jsonEscape(String s); String firstNameOnly(String fullName); String getAttendanceStatus(bool isCheckIn); String formatBytes(uint64_t bytes);
bool addLog(String event,int id,String message);
String buildAttendanceCsvRow(const String&,const String&,int,const String&,const String&,const String&,const String&,const String&,const String&);
String buildSystemLogRow(const String&,const String&,const String&,const String&,const String&);
String attendanceEventTypeOf(const String&); String attendanceStatusEnumOf(const String&); String attendanceStatusDisplayOf(const String&); bool splitDateTimeText(const String&,String&,String&);
String buildStatusJson(); String fastStatusJsonCopy(); void initFastStatusCache(); void serviceFastStatusCache(); String buildSystemJson(); String buildSetupJson();
void initBootSnapshotCache();
void refreshBootSnapshot(); String bootSnapshotCopy(); void serviceBootSnapshotRefresh();
int reserveLessonJournal(LessonAppend &entry); bool loadLessonJournal(uint8_t slot, LessonAppend &entry); bool clearLessonJournal(uint8_t slot);
String buildRamLogsJson(int limit,const char* dateFilter=nullptr);
String prefsGetString(const char*,const String&); bool prefsPutString(const char*,const String&); bool prefsGetBool(const char*,bool); bool prefsPutBool(const char*,bool); int prefsGetInt(const char*,int); bool prefsPutInt(const char*,int); bool prefsRemove(const char*); bool prefsClearAll();
String csvEscape(String); String profileKey(uint16_t,const char*); void saveProfile(uint16_t,PersonProfile); PersonProfile loadProfile(uint16_t); void deleteProfile(uint16_t);
String presenceKey(uint16_t); void loadPresenceState(); void serviceDeviceSetupPersistence(); bool savePresenceState(uint16_t); bool savePresenceStateValue(uint16_t,bool); void clearPresenceState(uint16_t); void loadDeviceSetup(); int hhmmToMinutes(String); String minutesToHHMM(int);
void smartDelay(unsigned long);
void oledScreen(String,String line1="",String line2="",String line3=""); void oledBig(String,String line1="",String line2=""); void oledScanScreen(); void oledScanAnimation(String,String,uint16_t); void oledProgressBar(String,String,uint8_t); void oledSuccessAnimation(String,String line2=""); void oledErrorAnimation(String,String line2=""); String clipText(String,uint8_t); void drawCenteredText(String,int,uint8_t); void drawTitleBar(String); void drawMiniFingerprint(int,int); void drawLoadingDots(int,int,uint8_t); void drawMovingBar(int,uint8_t); void drawProgress(uint8_t); void drawWiFiIcon(bool);
bool buttonPressed(uint8_t,unsigned long&); bool cancelButtonPressed(); bool checkCancelDuringEnroll();
void scanFingerprint(); void fpSetLed(bool); void fpNoteActivity(); void fpWake(); void fpSleepNow(); bool fpShouldScanNow(); void fpTouchSelfTest(); uint8_t enrollFingerprint(uint16_t); void waitForFingerRemove(); uint16_t findNextFreeFrom(uint16_t); void printSensorParameters(); void refreshTemplateCountSerialOnly(); void refreshTemplateCountCache(); void rebuildRegisteredCache(); void appendRegisteredEntry(uint16_t,PersonProfile); void removeRegisteredEntry(uint16_t); void setMatchingSecuritySerialOnly(); void attendanceSuccessFeedback(); void attendanceFailureFeedback();
void initializeSD(); bool tryQuickSdMount(); bool resetAllLocalDataOnSd(); bool clearAllTeacherProfilesAndPresence();
void setSdAvailableState(bool available, const char* reason = nullptr);
void loadAttendanceJournal();
bool storageCanAcceptAttendance();
bool persistAttendanceJournal(uint32_t seq,const String& row);
bool waitForAttendanceCommit(uint32_t seq,uint32_t timeoutMs=SD_COMMIT_WAIT_MS);
bool clearAttendanceJournal();
bool finalizeAttendanceCommit(uint32_t seq,const String& row,bool recovery);
void serviceRecoveredAttendanceJournal();
bool readAttendanceJournalRow(String& row);
 void sdReaderAttach(File&); bool readCsvRecord(File&,String[],int); void loadTodayFromSdToRam(); bool ensureLessonsCsv(); bool ensureSystemLog(); void migrateLessonsHeaderIfNeeded(); void migrateStorageIfNeeded(); void serviceLessonQueueTask(); void serviceLessonJournalRecoveryTask(); void serviceSystemLogQueueTask(); void serviceSdQueueTask(); void serviceBackgroundStatsTask(); void scanSdHistoryOnBootTask(); void serviceSdBulkRequest(SdBulkRequest&); void sdTaskFunc(void*); void markLogSaved(uint32_t);
void serviceLedBuzzerTimers(); void serviceWeekendAttendanceFeedback(); void serviceMainSdBuzzer(); void serviceOledKeepAlive(); void beginScanResultHold(unsigned long); void serviceScanResultPhase();
void requestRestart(bool factory=false);

// Auth
bool accountExists();
void loadAuthAccount();
String sha256Hex(const String&);
String generateSessionToken();
String createPasswordRecord(const String& password);
bool verifyPasswordRecord(const String& password, const String& storedRecord, bool *legacyOut=nullptr);
bool migrateLegacyPasswordRecordIfValid(const String& password, const String& storedRecord);
String cookieValue(const String&,const String&);
bool isAuthenticated(AsyncWebServerRequest*);
void requireAuthentication(AsyncWebServerRequest*);
void setLoginCookie(AsyncWebServerResponse*, const String &token);
void clearLoginCookieForRequest(AsyncWebServerRequest*, AsyncWebServerResponse*);
void clearLoginCookie(AsyncWebServerResponse*);
void sendLoginPage(AsyncWebServerRequest*);
void sendCreateAccountPage(AsyncWebServerRequest*);
void sendLogoutPage(AsyncWebServerRequest*);
int allocateAuthSession(const String& token);
bool removeAuthSessionForToken(const String& token);
void clearAllAuthSessions();

// SD async HTTP bridge
struct SdHttpStream;
bool initSdHttpBridge();
bool sdHttpStreamMatches(uint32_t id, QueueHandle_t queue);

// Request helpers
bool hasParam(AsyncWebServerRequest*,const char*,bool post=false); String getParamValue(AsyncWebServerRequest*,const char*,bool post=false);