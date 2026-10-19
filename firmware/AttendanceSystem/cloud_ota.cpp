#include "app.h"
#include "cloud_ota.h"
#include "cloud_sync.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <ctype.h>
#include <string.h>

// Releases are stored in a private bucket. Signed URLs are short-lived and
// are generated server-side only for the already-authenticated device.
static String cloudOtaSignedUrlPrefix() {
  const String endpoint = String(CLOUD_BASE_URL);
  const int scheme = endpoint.indexOf("://");
  if (scheme < 0) return "";

  const int pathStart = endpoint.indexOf('/', scheme + 3);
  const String origin = pathStart < 0 ? endpoint : endpoint.substring(0, pathStart);
  return origin + "/storage/v1/object/sign/firmware-releases/";
}

static bool validSha256Hex(const char* value) {
  if (!value || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isxdigit((unsigned char)value[i])) return false;
  }
  return true;
}

static bool validReleaseVersion(const char* value) {
  if (!value || value[0] != 'V' || strlen(value) > 64) return false;
  for (size_t i = 0; value[i]; ++i) {
    const char c = value[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '+' || c == '-')) return false;
  }
  return true;
}

static bool failCloudOta(
  const char* reason, HTTPClient& http, WiFiClientSecure& secure, bool updateStarted
) {
  Serial.printf("[CLOUD OTA] FAILED: %s\n", reason ? reason : "unknown");
  if (updateStarted) Update.abort();
  http.end();
  secure.stop();
  return false;
}

bool performCloudFirmwareUpdate(
  const char* targetVersion,
  const char* firmwareUrl,
  const char* expectedSha256,
  size_t expectedSize
) {
  if (!wifiDashboardReady()) {
    Serial.println("[CLOUD OTA] Wi-Fi is not connected; update rejected.");
    return false;
  }
  if (!validReleaseVersion(targetVersion) || !validSha256Hex(expectedSha256)) {
    Serial.println("[CLOUD OTA] Invalid version or SHA-256 metadata.");
    return false;
  }
  if (!firmwareUrl || !firmwareUrl[0] || expectedSize == 0 || expectedSize > 2097152UL) {
    Serial.printf("[CLOUD OTA] Invalid firmware URL or size: %u bytes.\n", (unsigned)expectedSize);
    return false;
  }

  const String url = String(firmwareUrl);
  const String allowedPrefix = cloudOtaSignedUrlPrefix();
  if (!allowedPrefix.length() || !url.startsWith(allowedPrefix) ||
      url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0 || url.indexOf('#') >= 0) {
    Serial.println("[CLOUD OTA] Rejected URL outside the trusted Supabase Storage bucket.");
    return false;
  }

  const esp_partition_t* nextPartition = esp_ota_get_next_update_partition(nullptr);
  if (!nextPartition) {
    Serial.println("[CLOUD OTA] No OTA update partition; a one-time USB flash with OTA partitions is required.");
    return false;
  }
  if (expectedSize > nextPartition->size) {
    Serial.printf("[CLOUD OTA] Image too large for partition %s: image=%u partition=%u.\n",
                  nextPartition->label, (unsigned)expectedSize, (unsigned)nextPartition->size);
    return false;
  }

  Serial.printf("[CLOUD OTA] Starting verified update: version=%s size=%u partition=%s\n",
                targetVersion, (unsigned)expectedSize, nextPartition->label);
  Serial.println("[CLOUD OTA] HTTPS certificate verification is ON; SHA-256 checked before reboot.");
  oledBig("CLOUD OTA", "Preparing update", targetVersion);

  WiFiClientSecure secure;
  secure.setHandshakeTimeout(20);
  secure.setTimeout(20000);
  secure.useBuiltinCACertBundle();

  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  if (!http.begin(secure, url)) {
    Serial.println("[CLOUD OTA] HTTP client could not initialize signed URL.");
    http.end();
    secure.stop();
    return false;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[CLOUD OTA] Download returned HTTP %d.\n", status);
    return failCloudOta("HTTPS download failed", http, secure, false);
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0 || (size_t)contentLength != expectedSize) {
    Serial.printf("[CLOUD OTA] Content-Length mismatch: response=%d expected=%u.\n",
                  contentLength, (unsigned)expectedSize);
    return failCloudOta("firmware size mismatch", http, secure, false);
  }

  if (!Update.begin(expectedSize, U_FLASH)) {
    Serial.printf("[CLOUD OTA] Update.begin failed: %s\n", Update.errorString());
    return failCloudOta("no space or OTA write initialization failed", http, secure, false);
  }
  if (!Update.setSHA256(expectedSha256)) {
    Serial.println("[CLOUD OTA] Update library rejected the expected SHA-256.");
    return failCloudOta("invalid SHA-256 checksum", http, secure, true);
  }

  Stream* stream = http.getStreamPtr();
  if (!stream) return failCloudOta("HTTPS stream is unavailable", http, secure, true);

  uint8_t buffer[1024];
  size_t received = 0;
  int lastShownPercent = -5;
  uint32_t lastDataAt = millis();
  bool streamFailed = false;

  while (received < expectedSize) {
    const int availableBytes = stream->available();
    if (availableBytes > 0) {
      size_t toRead = (size_t)availableBytes;
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
      if (toRead > expectedSize - received) toRead = expectedSize - received;

      const size_t got = stream->readBytes(buffer, toRead);
      if (got == 0) {
        streamFailed = true;
        break;
      }
      const size_t written = Update.write(buffer, got);
      if (written != got) {
        Serial.printf("[CLOUD OTA] Flash write failed: %s\n", Update.errorString());
        streamFailed = true;
        break;
      }

      received += got;
      lastDataAt = millis();
      const int percent = (int)((received * 100ULL) / expectedSize);
      if (percent >= lastShownPercent + 5 || percent >= 100) {
        lastShownPercent = percent;
        Serial.printf("[CLOUD OTA] Progress: %d%% (%u/%u bytes)\n",
                      percent, (unsigned)received, (unsigned)expectedSize);
        const String progress = String(percent) + "% downloaded";
        oledBig("CLOUD OTA", progress.c_str(), targetVersion);
      }
    } else {
      if (!http.connected() && !stream->available()) break;
      if (millis() - lastDataAt > 30000UL) {
        Serial.println("[CLOUD OTA] No download progress for 30 seconds.");
        streamFailed = true;
        break;
      }
      delay(1);
    }
  }

  if (streamFailed || received != expectedSize) {
    Serial.printf("[CLOUD OTA] Incomplete image: received=%u expected=%u.\n",
                  (unsigned)received, (unsigned)expectedSize);
    return failCloudOta("download interrupted; partial image discarded", http, secure, true);
  }

  // Close TLS before finalization/command acknowledgement to reduce peak RAM.
  http.end();
  secure.stop();

  if (!Update.end()) {
    Serial.printf("[CLOUD OTA] Update.end failed (checksum/image validation): %s\n", Update.errorString());
    Update.abort();
    oledErrorAnimation("OTA FAILED", "Checksum/image invalid");
    return false;
  }
  if (!Update.isFinished()) {
    Serial.println("[CLOUD OTA] Update finalized without a complete image.");
    Update.abort();
    oledErrorAnimation("OTA FAILED", "Image incomplete");
    return false;
  }

  Serial.printf("[CLOUD OTA] Image %s verified and staged; reboot required.\n", targetVersion);
  oledBig("OTA VERIFIED", "Restarting", targetVersion);
  return true;
}
