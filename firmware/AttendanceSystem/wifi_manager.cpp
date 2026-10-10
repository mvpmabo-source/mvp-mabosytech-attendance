#include "app.h"
#include <esp_wifi.h>
#include <esp_err.h>

// =====================================================
// SECURE ROUTER / STATION WI-FI
// =====================================================
// The ESP32 does NOT create an access point in this build.
// It joins the configured office/router Wi-Fi and exposes the existing
// dashboard on the IP assigned by that router.
//
// Security boundary:
//   Router WPA2/WPA3 -> Wi-Fi association -> dashboard authentication -> OTA auth
// No fallback AP is created, so there is no second unsecured/provisioning path.
//
// Performance:
//   Wi-Fi sleep is disabled to reduce latency/jitter for dashboard polling and
//   near-real-time status monitoring. Reconnect attempts are rate-limited and
//   non-blocking after the initial startup window.
// =====================================================

static unsigned long wifiNextReconnectMs = 0;
static bool wifiBeginIssued = false;
static bool wifiConnectInProgress = false;
static unsigned long wifiConnectDeadlineMs = 0;

static const char* wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "NO_SHIELD";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static void printStaNetworkState(const char* prefix) {
  Serial.print(prefix);
  Serial.print(" status=");
  Serial.print(wifiStatusText(WiFi.status()));
  if (wifiDashboardReady()) {
    Serial.print(" IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" MASK=");
    Serial.print(WiFi.subnetMask());
    Serial.print(" GW=");
    Serial.print(WiFi.gatewayIP());
    Serial.print(" RSSI=");
    Serial.print(WiFi.RSSI());
    Serial.print(" CH=");
    Serial.print(WiFi.channel());
  }
  Serial.println();
}

void connectWiFi() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  // Arduino-ESP32 requires the hostname to be assigned before Wi-Fi is started.
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.mode(WIFI_STA);
  delay(100);

  const size_t ssidLen = strlen(WIFI_SSID);
  const size_t passLen = strlen(WIFI_PASSWORD);
  if (ssidLen == 0 || ssidLen > 32) {
    Serial.printf("[WIFI] FATAL CONFIG: SSID length=%u; must be 1..32 bytes.\n",
                  (unsigned)ssidLen);
    wifiBeginIssued = false;
    wifiWasConnected = false;
    return;
  }
  if (passLen < 8 || passLen > 63) {
    Serial.printf("[WIFI] FATAL CONFIG: password length=%u; WPA/WPA2 PSK must be 8..63 bytes.\n",
                  (unsigned)passLen);
    wifiBeginIssued = false;
    wifiWasConnected = false;
    return;
  }

  Serial.println();
  Serial.println("[WIFI] ===== SECURE ROUTER / STA MODE =====");
  Serial.print("[WIFI] Target SSID: ");
  Serial.println(WIFI_SSID);
  Serial.print("[WIFI] Password length: ");
  Serial.println(passLen);
  Serial.println("[WIFI] Security: inherited from the router (no ESP AP fallback)");
  Serial.println("[WIFI] Power-save: OFF (low-latency dashboard mode)");

  // Do not force a disconnect before the first begin().  The STA mode
  // transition already establishes a clean station context, and avoiding
  // an extra disconnect helps preserve the router's DHCP lease when the
  // ESP is rebooted after a firmware update.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiBeginIssued = true;
  wifiConnectInProgress = true;
  wifiConnectDeadlineMs = millis() + WIFI_CONNECT_TIMEOUT_MS;
  wifiNextReconnectMs = millis() + WIFI_RECONNECT_INTERVAL_MS;

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    printStaNetworkState("[WIFI] Connected:");
    Serial.print("[WIFI] Dashboard: http://");
    Serial.println(WiFi.localIP());
  } else {
    wifiWasConnected = false;
    printStaNetworkState("[WIFI] Initial connection not completed:");
    Serial.println("[WIFI] Background reconnect remains enabled.");
  }
  Serial.println("[WIFI] =====================================");
}

void serviceWiFi() {
  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wifiConnectInProgress = false;
    wifiConnectDeadlineMs = 0;
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      printStaNetworkState("[WIFI] Reconnected:");
      Serial.print("[WIFI] Dashboard: http://");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    printStaNetworkState("[WIFI] Link lost:");
    wifiConnectInProgress = false;
    wifiConnectDeadlineMs = 0;
  }

  if (!wifiBeginIssued) return;

  const unsigned long now = millis();

  // A station may remain in WL_IDLE_STATUS / an internal connecting state for
  // several seconds after the link drops. Never call WiFi.begin()/reconnect()
  // again while a previous association attempt is still active; Core 3.3.x
  // reports "sta is connecting, cannot set config" in that condition.
  if (wifiConnectInProgress) {
    if ((long)(now - wifiConnectDeadlineMs) < 0) return;

    Serial.println("[WIFI] Association timeout; restarting a clean STA attempt.");
    WiFi.disconnect(false, false);
    wifiConnectInProgress = false;
    wifiNextReconnectMs = now + 250UL;
    return;
  }

  if ((long)(now - wifiNextReconnectMs) < 0) return;

  // Only start a fresh association when the driver explicitly reports a
  // terminal/non-connecting state. This prevents repeated config writes while
  // the underlying STA state machine is still progressing.
  switch (status) {
    case WL_DISCONNECTED:
    case WL_CONNECTION_LOST:
    case WL_CONNECT_FAILED:
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      wifiConnectInProgress = true;
      wifiConnectDeadlineMs = now + WIFI_CONNECT_TIMEOUT_MS;
      wifiNextReconnectMs = now + WIFI_RECONNECT_INTERVAL_MS;
      Serial.println("[WIFI] New STA association attempt started.");
      break;
    case WL_NO_SSID_AVAIL:
    case WL_IDLE_STATUS:
    default:
      // Do not touch credentials/configuration while the Wi-Fi state machine
      // is still busy. Re-check shortly.
      wifiNextReconnectMs = now + 1000UL;
      break;
  }
}

// ---------------------------------------------------------------------
// RTC + NTP
// ---------------------------------------------------------------------
// rtcAvailable and rtcTimeValid remain independent so an invalid RTC does not
// become a false source of time.
// ---------------------------------------------------------------------