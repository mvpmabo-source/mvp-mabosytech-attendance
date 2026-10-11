#pragma once

// Copy to secrets_local.h and fill in credentials locally.
// Keep secrets_local.h excluded from Git.

// ---- Wi-Fi (unchanged) ----
#define WIFI_SSID_VALUE "bosco"
#define WIFI_PASSWORD_VALUE "REPLACE_WITH_YOUR_WIFI_PASSWORD"
#define OTA_PASSWORD "REPLACE_WITH_A_UNIQUE_RANDOM_OTA_PASSWORD"

// ---- Cloud Sync (NEW — for hosted dashboard) ----
// The device ID must match what you registered in the cloud dashboard's
// Settings page. The API key is generated when you add a device there —
// copy it immediately (it is shown only once).
#define CLOUD_DEVICE_ID "mbs-ba-mvp"
#define CLOUD_API_KEY "REPLACE_WITH_API_KEY_FROM_DASHBOARD_SETTINGS"
