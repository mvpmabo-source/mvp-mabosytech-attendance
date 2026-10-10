#include "app.h"

// =====================================================
// OTA / NETWORK FIRMWARE UPDATE
// =====================================================
// The device runs in router/STA mode. ArduinoOTA uses the same local LAN
// as the dashboard; no Internet connection is required.
//
// OTA authentication is intentionally SEPARATE from the dashboard password.
// Dashboard passwords are stored as salted records (v1$salt$digest), while
// ArduinoOTA expects a plaintext password or a raw SHA-256(password) hash and
// then performs its own PBKDF2-HMAC-SHA256 challenge/response.
// Using setPassword() lets ArduinoOTA hash the dedicated OTA password in the
// supported way. This prevents the dashboard's salted storage format from
// breaking OTA authentication.
//
// IMPORTANT: OTA requires an OTA-capable partition table (at least two app
// partitions). If no update partition exists, OTA is left disabled and the
// existing USB workflow remains unchanged.
static bool otaInitialized = false;
static volatile bool otaBusy = false;
static String otaHostname;
static unsigned long otaLastProgressMs = 0;
static unsigned long otaNextInitAttemptMs = 0;

static bool otaNetworkReady() {
  return wifiDashboardReady();
}


void setupOTA() {
  if (otaInitialized) return;
  const unsigned long now = millis();
  if ((long)(now - otaNextInitAttemptMs) < 0) return;
  if (!otaNetworkReady()) {
    otaNextInitAttemptMs = now + 1000UL;
    return;
  }

  const esp_partition_t *nextPartition = esp_ota_get_next_update_partition(nullptr);
  if (!nextPartition) {
    Serial.println("[OTA] DISABLED: no OTA update partition found.");
    Serial.println("[OTA] Select an OTA-capable partition scheme and perform one USB upload.");
    otaNextInitAttemptMs = now + 10000UL;
    return;
  }

  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char host[32];
  snprintf(host, sizeof(host), "mabosy-attendance-%02x%02x%02x",
           mac[3], mac[4], mac[5]);
  otaHostname = String(host);

  ArduinoOTA
    .setPort(3232)
    .setHostname(otaHostname.c_str())
    .setPassword(OTA_PASSWORD)
    .onStart([]() {
      otaBusy = true;
      digitalWrite(MAIN_BUZZER_PIN, HIGH);
      otaLastProgressMs = millis();
      Serial.println();
      Serial.println("[OTA] ===============================");
      Serial.println("[OTA] Firmware update starting...");
      Serial.println("[OTA] Dashboard/fingerprint work will pause during upload.");
      Serial.println("[OTA] ===============================");
      oledBig("OTA UPDATE", "Receiving", "firmware...");
    })
    .onEnd([]() {
      otaBusy = false;
      Serial.println();
      Serial.println("[OTA] Firmware upload complete. Rebooting...");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      const unsigned long now = millis();
      if (now - otaLastProgressMs >= 500 || progress == total) {
        otaLastProgressMs = now;
        unsigned int pct = total ? (progress * 100U) / total : 0U;
        Serial.printf("[OTA] Progress: %u%%\n", pct);
      }
    })
    .onError([](ota_error_t error) {
      otaBusy = false;
      digitalWrite(MAIN_BUZZER_PIN, HIGH);
      Serial.printf("[OTA] Error[%u]: ", error);
      switch (error) {
        case OTA_AUTH_ERROR:    Serial.println("Authentication failed"); break;
        case OTA_BEGIN_ERROR:   Serial.println("Begin failed (check OTA partition/space)"); break;
        case OTA_CONNECT_ERROR: Serial.println("Data connection failed"); break;
        case OTA_RECEIVE_ERROR: Serial.println("Firmware receive failed"); break;
        case OTA_END_ERROR:     Serial.println("Firmware finalization failed"); break;
        default:                Serial.println("Unknown OTA error"); break;
      }
      oledErrorAnimation("OTA FAILED", "Try again");
    });

  // ArduinoOTA.begin() starts the ESPmDNS responder and advertises the
  // dedicated _arduino._tcp service with the configured OTA port.
  // Do not add an unrelated HTTP mDNS service here: Arduino IDE's network
  // uploader expects the Arduino OTA service metadata (including port).
  ArduinoOTA.begin();
  otaInitialized = true;
  otaNextInitAttemptMs = 0;

  Serial.println("[OTA] ===== OTA READY =====");
  Serial.print("[OTA] Hostname: ");
  Serial.println(ArduinoOTA.getHostname());
  Serial.print("[OTA] IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[OTA] Local hostname: http://");
  Serial.print(ArduinoOTA.getHostname());
  Serial.println(".local");
  Serial.println("[OTA] mDNS service: _arduino._tcp port=3232");
  Serial.print("[OTA] Port: 3232");
  Serial.println();
  Serial.println("[OTA] Authentication: dedicated OTA password (separate from dashboard account)");
  Serial.print("[OTA] Target partition: ");
  Serial.println(nextPartition->label);
  Serial.println("[OTA] Arduino IDE -> Tools -> Port -> Network Ports");
  Serial.println("[OTA] =====================");
}

bool otaInProgress() {
  return otaBusy;
}

void serviceOTA() {
  // If the dashboard account was created after boot, initialize OTA without
  // requiring a USB upload or reboot.
  if (!otaInitialized && !otaBusy) {
    setupOTA();
  }

  if (otaInitialized) {
    ArduinoOTA.handle();
  }
}