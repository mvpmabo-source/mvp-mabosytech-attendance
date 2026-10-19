# Cloud OTA rollout guide

## What this adds

The Device page can upload an ESP32 application image to the private `firmware-releases` Storage bucket, record the release version and SHA-256 digest, and queue a `cloud_ota` command. The authenticated `device-ingest` function creates a short-lived signed HTTPS URL for the device. Firmware checks the trusted Storage origin, release version, download length, update-partition capacity, SHA-256 and ESP image before activating the staged update.

Firmware binaries and privileged keys must never be public. Do not add `secrets_local.h`, Wi-Fi passwords, OTA passwords, a service-role key, or a device API key to Git.

## Important: initial bootstrap is required

Firmware that is already deployed (V2.9.45) does not understand the new `cloud_ota` command. Install the new cloud-OTA-enabled firmware once with the existing ArduinoOTA LAN workflow or USB. After that, future compatible releases can be deployed through the Cloud Firmware OTA section in the Device page.

Confirm the installed partition table has an OTA update partition. A build made with the repository workflow uses `PartitionScheme=min_spiffs`, but its placeholder-secrets artifact is for compile checks only and must not be flashed to production hardware. Build the first deployable image locally using your own private `secrets_local.h`; upload only the application image (usually `AttendanceSystem.ino.bin`), not bootloader or partition-table binaries.

## Backend deployment (Bolt-managed project)

The existing `cloud-dashboard/supabase/config.toml` documents that the live backend is managed by the Bolt project. Do not deploy to an unrelated Supabase project. Apply migration `cloud-dashboard/supabase/migrations/003_cloud_ota.sql` to that existing backend, then deploy the updated `cloud-dashboard/supabase/functions/device-ingest/index.ts` and updated Device page through the existing Bolt workflow. Verify the private bucket exists and anonymous users cannot read or upload its objects. This repository commit does not itself apply the migration or deploy the hosted dashboard/edge function.

## Release procedure

1. Build the intended firmware version locally with the real private secrets and OTA-capable partition scheme. Confirm the compiled `FIRMWARE_VERSION` value matches the version entered in the dashboard.
2. From Device → Cloud Firmware OTA, upload the application `.bin`, enter the exact version, optionally add notes, and upload the release. The dashboard calculates SHA-256; upload alone does not touch the hardware.
3. Check the listed release version, size and SHA-256, then choose **Send to ESP32** only when the correct device is online and safe to update.
4. Watch Serial Monitor/OLED. The device reports download progress; it refuses non-HTTPS/untrusted storage URLs, incorrect sizes, images bigger than the OTA partition, failed SHA-256/image validation, or incomplete downloads. It only acknowledges a successful staged image and then restarts.
5. After reboot, confirm the latest heartbeat reports the expected firmware version, and test fingerprint check-in/check-out, RTC time, SD attendance logs, and cloud outbox sync.
6. For failed OTA tests, confirm the running firmware remains online and attendance records are still intact before retrying.

## Safe rollout checklist

- Test first with a spare ESP32 or during a maintenance window; attendance pauses while firmware downloads/verifies.
- Keep USB/recovery access available for the first rollout and until OTA is proven on the actual device.
- Do not merge this feature branch until CI compile and hardware regression tests pass.
- This initial implementation verifies the image and stages it for restart; it does not include an independent cloud command to roll back a firmware version that boots but malfunctions. Keep a known-good binary and a recovery method.
