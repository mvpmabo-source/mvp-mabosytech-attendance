#include "app.h"

// =====================================================
// WIFI SETTINGS
// =====================================================
// The ESP32 joins the existing office/router Wi-Fi as a STA.
// The two credential macros live in config.h so they are easy to change
// without touching the network implementation.
const char* WIFI_SSID     = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;
// Rwanda local time: Central Africa Time, UTC+2, no DST.
// Kept as POSIX TZ form because the ESP32 time APIs use that representation.
const char* TIME_ZONE     = "CAT-2";

AsyncWebServer server(80);
Preferences prefs;

// =====================================================
// OLED (SOFTWARE SPI)
// =====================================================

Adafruit_SH1106G display(
  SCREEN_WIDTH, SCREEN_HEIGHT,
  OLED_MOSI, OLED_SCLK, OLED_DC, OLED_RES, OLED_CS
);

// =====================================================
// FINGERPRINT SENSOR
// =====================================================
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

// =====================================================
// BUTTONS / LED / BUZZER
// =====================================================
// -----------------------------------------------------------------
// BTN_ENROLL is GPIO 34. On real ESP32 silicon, GPIO 34-39 are
// INPUT-ONLY and have NO internal pull-up/pull-down resistors at all
// - pinMode(INPUT_PULLUP) on this pin is silently ignored by the SoC.
// If the button is wired the usual way (one leg to GND, relying on an
// internal pull-up), the pin FLOATS when the button is not pressed and
// will trigger enrollment at random.
//
// This pin needs an EXTERNAL pull-up resistor to work correctly:
//   3V3 --[10k ohm]-- GPIO34 -- button -- GND
//
// NOTE (this revision): physical buttons no longer control enrollment
// at all. See loop() below. This pin remains defined only so the
// hardware pin map is unchanged.
unsigned long lastEnrollBtnTime = 0;
unsigned long lastCancelBtnTime = 0;


// =====================================================
// RTC (DS3231)
// =====================================================
RTC_DS3231 rtc;
volatile bool rtcAvailable = false;      // chip responded to rtc.begin()
volatile bool rtcTimeValid = false;      // stored year is plausible (>= 2024)
volatile bool wifiWasConnected = false;

// Network helpers in app.h expose the currently assigned STA IP to the rest
// of the dashboard without coupling application code to Wi-Fi setup details.
// =====================================================
// microSD (DEDICATED HW SPI)
// =====================================================


// =====================================================
// STORAGE ARCHITECTURE (attendance.csv / lessons.csv / system.log)
// -----------------------------------------------------------------
// /attendance.csv  -> ATTENDANCE EVENTS ONLY (CHECK_IN / CHECK_OUT)
// /lessons.csv     -> reason/explanation records (Impamvu)
// /system.log      -> everything else (INFO/ERROR/WARNING/ENROLL/...)
// See addLog() for the routing decision and migrateStorageIfNeeded()
// for one-time migration of a pre-existing mixed attendance.csv.
// =====================================================


// Ignore a second attendance action from the same finger within this
// window so an accidental double-tap on the sensor cannot create a
// spurious CHECK_IN immediately followed by a CHECK_OUT (or vice
// versa). Short enough that it never blocks a legitimate later
// CHECK_OUT.

volatile bool sdAvailable = false;
volatile bool mainSdBuzzerWarningPending = false;
uint8_t sdConsecutiveFailures = 0;
uint8_t sdWriteConsecutiveFailures = 0;
uint8_t sdHealthConsecutiveFailures = 0;
const uint8_t SD_FAILURE_THRESHOLD = 3;
uint32_t sdGeneration = 0;

unsigned long sdBackoffMs = 5000;
const unsigned long SD_BACKOFF_MAX_MS = 60000;
unsigned long lastSdRemountAttempt = 0;
unsigned long lastSdWriteAttempt = 0;

const unsigned long SD_WRITE_INTERVAL_MS = 0;
const unsigned long SD_OPEN_RETRY_DELAY_MS = 500;
unsigned long sdLastOpenFailMs = 0;

uint32_t sdRecordCount = 0;
uint32_t sdLessonCount = 0;

// Transactional attendance journal. A scan is not acknowledged until its
// exact CSV row has been verified on the SD card and the journal entry has
// been removed successfully.
volatile bool attendanceJournalActive = false;
volatile uint32_t attendanceJournalSeq = 0;
volatile bool attendanceCommitAck = false;
volatile bool attendanceJournalRecovery = false;
volatile uint32_t attendanceStateAppliedSeq = 0;

uint64_t sdTotalBytesCache = 0;
uint64_t sdUsedBytesCache  = 0;
uint64_t sdCsvBytesCache   = 0;
uint64_t sdLessonsBytesCache = 0;
unsigned long sdStatAtMs      = 0;
unsigned long sdUsedBytesAtMs = 0;
bool sdStatValid = false;
uint32_t sdMountedClockHz = 1000000UL;   // actual negotiated SD SPI clock
const unsigned long SD_STAT_TTL_MS      = 15000;
const unsigned long SD_USEDBYTES_TTL_MS = 300000;

// =====================================================
// sdTask write-staging ring
// =====================================================

PendingRow pendingRows[MAX_PENDING];
int pendingHead = 0, pendingTail = 0, pendingUsed = 0;
int pendingDropped = 0, pendingQueueDropped = 0;

// =====================================================
// CONSTANTS
// =====================================================


int cfgCheckinMinutes  = CHECKIN_ON_TIME_MINUTES;
int cfgCheckoutMinutes = CHECKOUT_ON_TIME_MINUTES;
String cfgDeviceName = "MBS BA MVP";
String cfgTimezone   = TIME_ZONE;
int cfgSecurityLevel = MATCH_SECURITY_LEVEL;

bool fingerprintSensorOk = false;
bool ntpSynced = false;

// =====================================================
// FINGERPRINT SENSOR IDLE / SLEEP MANAGEMENT
// =====================================================
// Why this exists:
//   loop() used to call finger.getImage() on every pass. Each call is a
//   full 12-byte UART transaction at 57600 baud plus the reply, so the
//   optical sensor was being driven several hundred times per second,
//   24/7, with its illumination LED permanently lit. That burns power,
//   shortens LED life, and keeps core 1 busy with UART work that is
//   almost always going to answer "no finger".
//
// How the sleep works (this is the correct way for an optical sensor):
//   An optical scanner CANNOT see a finger with its LED off, so "sleep"
//   cannot simply mean "stop lighting it and keep polling" -- that would
//   never wake up. Instead we strobe:
//
//     ACTIVE  : LED on, poll at full speed (instant recognition)
//        | no finger for FP_ACTIVE_TIMEOUT_MS
//        v
//     SLEEP   : LED off, UART idle
//        | every FP_SLEEP_PROBE_MS:
//        |    LED on -> wait FP_PROBE_EXPOSURE_MS for exposure
//        |    -> one getImage()
//        |    -> NOFINGER ? LED off, stay asleep
//        |    -> otherwise wake to ACTIVE and hand over to scanFingerprint()
//
//   The probe is split across loop() passes, so NOTHING here blocks:
//   there is no delay() and no vTaskDelay() in the wake path. Worst-case
//   added latency when a teacher touches a sleeping sensor is one probe
//   interval (400 ms), and the very next scan is full speed again.
//
//   Duty cycle while asleep: 60 ms lit out of every 400 ms (~15%).
//
// ---------------------------------------------------------------------
// ZERO-BLINK SLEEP (what you asked for: LED fully off, no flashing)
// ---------------------------------------------------------------------
// Physics first, because it decides the wiring: an OPTICAL sensor cannot
// see a finger while its illumination LED is off. So on a 2-wire (TX/RX
// only) hookup, "LED off AND still wakes on touch" is impossible - the
// strobe below is the best that can be done, and it blinks.
//
// Your module reports capacity 300, i.e. an R307/AS608. That module has
// a dedicated TOUCH-OUT pin (often labelled VT / WAKEUP / IRQ) plus a
// touch-supply pin (TVcc / 3V3T). That pin is a capacitive touch sensor
// that works with the LED completely dark. Wire it and you get exactly
// what you want: LED off, no blinking at all, instant wake on contact.
//
//   R307 wire        ESP32
//   ---------------  -----------------------------
//   VT / WAKEUP  ->  GPIO 35 (input-only, free here)
//   TVcc / 3V3T  ->  3V3
//
// Then set FP_USE_TOUCH_PIN to 1 below and re-flash. That is the ONLY
// change needed - everything else adapts automatically.
//
// Leave it at 0 and behaviour is the previous strobe (LED pulses briefly
// every 400 ms). Nothing else in the firmware depends on which mode is
// active.




bool          fpAsleep          = false;
bool          fpLedSupported    = true;   // cleared if the module rejects LEDcontrol
FpProbePhase  fpProbePhase      = FP_PROBE_IDLE;
unsigned long fpLastActivityMs  = 0;
unsigned long fpLastProbeMs     = 0;
unsigned long fpProbeLitMs      = 0;
bool          fpTouchMode       = (FP_USE_TOUCH_PIN != 0);
unsigned long fpTouchHighSince  = 0;

// -----------------------------------------------------------------
// WAKE-UP FIX: the optical sensor's illumination LED needs a brief
// moment to physically light up and stabilize before an image capture
// will succeed - this is exactly why STROBE mode already waits
// FP_PROBE_EXPOSURE_MS after turning the LED on before its first
// getImage() attempt. TOUCH-PIN mode was missing that same wait: it
// turned the LED on and attempted to scan in the very same instant,
// so the first (and, since ACTIVE mode never re-checks this, every
// following) capture was taken against a still-dark sensor and never
// succeeded - matching exactly "sleep works, wake does not".
// fpWakingUp/fpWakeStartMs give touch-mode the same settle window,
// non-blocking, spread across loop() passes like everything else here.
//
// ORDERING NOTE (this revision): fpWake() clears fpWakingUp as a
// side-effect. The touch branch therefore sets fpWakingUp = true AFTER
// calling fpWake(). Do not swap those two lines.
// -----------------------------------------------------------------
bool          fpWakingUp        = false;
unsigned long fpWakeStartMs     = 0;



// =====================================================
// RAM LOG RING BUFFER
// =====================================================

AttendanceLog logs[MAX_LOGS];
int logCount = 0, logWriteIndex = 0, ramDropped = 0;
uint32_t logSeqCounter = 0;

uint16_t nextEnrollID = 1;
bool isEnrolling = false, cancelEnroll = false, duplicateFound = false;
bool dashboardEnrollRequest = false;


ScanResultPhase scanResultPhase = SCAN_RESULT_NONE;
unsigned long scanResultUntil = 0;

unsigned long ledOffAt = 0, buzzerOffAt = 0;

uint16_t cachedTemplateCount = 0;
String cachedRegisteredJson = "[]";
uint16_t scanFirstFreeId = 1;
int entryCount = 0, exitCount = 0, lastEntryId = -1, tickLastRamDroppedSeen = -1;
bool insideState[MAX_FINGERPRINTS + 1];
// Section 6 (duplicate protection): last time each fingerprint ID produced
// an attendance record, used to debounce accidental repeated scans.
unsigned long lastAttendanceActionMs[MAX_FINGERPRINTS + 1];

// --- cached last-entry JSON (PERF) ---
int cachedLastEntryJsonForId = -1;
String cachedLastEntryJson = "null";

String pendingName, pendingCollegeId, pendingRole, pendingDepartment, pendingNote;

String authUsername = "", authPasswordHash = "", activeSessionToken = "";
AuthSession authSessions[AUTH_MAX_SESSIONS] = {};

// Small in-memory login throttle. It resets after a successful login or
// after the short lock window; it does not persist credentials or state.
uint8_t loginFailedAttempts = 0;
unsigned long loginBlockedUntilMs = 0;

// =====================================================
// FREERTOS PRIMITIVES
// =====================================================
SemaphoreHandle_t sharedStateMutex;
SemaphoreHandle_t prefsMutex;
SemaphoreHandle_t fingerMutex;
SemaphoreHandle_t ramLogSnapshotMutex;
SemaphoreHandle_t rtcMutex;
SemaphoreHandle_t authSessionMutex;
SemaphoreHandle_t cloudSdMutex = nullptr;

QueueHandle_t attendanceQueue;

// Non-attendance diagnostic/system events (INFO, ERROR, WARNING, ENROLL,
// CANCEL, DELETE, DUPLICATE, MATCH, NOT FOUND, SYSTEM_READY, WIFI/SD
// diagnostics, ...) are routed here instead of attendanceQueue, so they
// land in /system.log rather than /attendance.csv. Only sdTask ever
// touches the SD card for either queue.

QueueHandle_t systemLogQueue = nullptr;
SystemLogEvent systemLogFallback[4]{};
uint8_t systemLogFallbackCount = 0;


QueueHandle_t lessonAppendQueue = nullptr;



QueueHandle_t sdRequestQueue = nullptr;
QueueHandle_t browserTimeSyncQueue = nullptr;

TaskHandle_t sdTaskHandle  = nullptr;


bool pendingRestart=false;
volatile bool factoryResetSdRequested = false;
volatile bool factoryResetSdDone = false;
volatile bool factoryResetSdOk = false;

unsigned long pendingRestartAtMs=0;

volatile bool dashboardBootReady = false;
volatile bool attendanceBootReady = false;
volatile bool bootSnapshotDirty = false;

volatile bool fingerprintSecurityDirty=false;
volatile bool deviceSetupPersistPending=false;