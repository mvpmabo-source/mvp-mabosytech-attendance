#pragma once
#include "secrets_local.h"  // local-only credentials; never commit this file

// Hardware / network configuration preserved from the verified source.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SCLK 22
#define OLED_MOSI 21
#define OLED_RES 25
#define OLED_DC 26
#define OLED_CS 27
#define FINGER_RX 16
#define FINGER_TX 17
#define BTN_ENROLL 34
#define BTN_CANCEL 5
#define DEBOUNCE_MS 250
#define LED_GREEN 12
#define LED_RED 13
#define BUZZER_PIN 14
// Dedicated main/system warning buzzer. Hardware is wired active-low:
// 3V3 -> buzzer (+), buzzer (-) -> GPIO15. Keep OFF during boot by default.
#define MAIN_BUZZER_PIN 15
#define MAIN_SD_BEEP_ON_MS 120UL
#define MAIN_SD_BEEP_INTERVAL_MS 3000UL
#define RTC_SDA 33
#define RTC_SCL 32
#define RTC_MIN_VALID_YEAR 2024
#define SD_SCLK 23
#define SD_MOSI 18
#define SD_MISO 19
#define SD_CS 4
#define SD_LOG_FILE "/attendance.csv"
#define SD_LESSONS_FILE "/lessons.csv"
#define SD_SYSLOG_FILE "/system.log"
#define SD_ATTENDANCE_TMP_FILE "/attendance_new.csv"
#define SD_LEGACY_LOG_FILE "/attendance_legacy.csv"
#define ATTENDANCE_CSV_HEADER "date,time,fingerprint_id,name,college_id,role,department,event_type,status"
#define LESSONS_CSV_HEADER "date,time,fingerprint_id,name,college_id,role,department,reason_type,reason"
#define SYSTEM_LOG_HEADER "date,time,level,event,message"
#define STORAGE_SCHEMA_KEY "sto_schema_v"
#define STORAGE_SCHEMA_CURRENT 2
#define ATTENDANCE_DEBOUNCE_MS 8000UL
#define MATCH_SECURITY_LEVEL 2
#define MAX_FINGERPRINTS 200
#define MAX_LOGS 200
#define MAX_PENDING 32
#define CHECKIN_ON_TIME_MINUTES 475
#define CHECKOUT_ON_TIME_MINUTES 1020
#define CFG_CHECKIN_KEY "cfg_checkin"
#define CFG_CHECKOUT_KEY "cfg_checkout"
#define CFG_DEVNAME_KEY "cfg_devname"
#define CFG_TZ_KEY "cfg_tz"
#define CFG_SECLVL_KEY "cfg_seclvl"
#define FP_USE_TOUCH_PIN 1
#define FP_TOUCH_PIN 35
#define FP_TOUCH_ACTIVE HIGH
#define FP_ACTIVE_TIMEOUT_MS 20000UL
#define FP_SLEEP_PROBE_MS 400UL
#define FP_PROBE_EXPOSURE_MS 60UL
#define FP_TOUCH_STUCK_MS 30000UL
#define FIRMWARE_VERSION "V2.9.45"

// =====================================================
// ROUTER / STA WI-FI CREDENTIALS
// =====================================================
// Edit ONLY these two values for the Wi-Fi network that should host
// the attendance dashboard. The ESP32 joins that network as a station.
#define WIFI_HOSTNAME "mabosy-attendance"
#define WIFI_CONNECT_TIMEOUT_MS 12000UL
#define WIFI_RECONNECT_INTERVAL_MS 5000UL
// Separate OTA credential. This is intentionally independent of the salted
// dashboard password because ArduinoOTA expects its own plain password/hash.
// Change this value only when you deliberately want to rotate OTA access.
#define AUTH_SESSION_COOKIE "mbs_session"
#define AUTH_USERNAME_KEY "auth_user"
#define AUTH_PASSWORD_KEY "auth_hash"
#define SD_CHUNK_CAP 511
#define SD_HTTP_STREAM_BACKPRESSURE_MS 30000UL
#define SD_HTTP_STREAM_IDLE_MS 30000UL
#define SD_ATTENDANCE_JOURNAL_KEY "att_journal_v1"
#define SD_COMMIT_WAIT_MS 30000UL
#define SD_JOURNAL_MAX_PAYLOAD 420

// SD async stream slots. This is an implementation resource, not a business limit.
#define SD_HTTP_STREAM_SLOTS 1
#define SD_HTTP_STREAM_QUEUE_DEPTH 16
#define SD_LESSON_JOURNAL_SLOTS 8
#define SD_HEALTHCHECK_INTERVAL_MS 2000UL
#define SD_HEALTH_FAILURE_THRESHOLD 3