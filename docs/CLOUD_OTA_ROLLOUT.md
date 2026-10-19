# Cloud OTA rollout guide

## What this adds

The Device page can upload an ESP32 application image to the private `firmware-releases` Storage bucket, record the release version and SHA-256 digest, and queue a `cloud_ota` command. The authenticated `device-ingest` function creates a short-lived signed HTTPS URL for the device. Firmware checks the trusted Storage origin, release version, download length, update-partition capacity, SHA-256 and ESP image before activating the staged update.

Firmware binaries and privileged keys must never be public. Do not add `secrets_local.h`, Wi-Fi passwords, OTA passwords, a service-role key, or a device API key to Git.

## Important: initial bootstrap is required

Firmware that is already deployed (V2.9.45) does not understand the new `cloud_ota` command. Install the new combined firmware `V2.9.47` once with the existing ArduinoOTA LAN workflow or USB. This release includes cloud OTA, strict CA-bundle TLS verification, background cloud networking, and date-scoped check-in/check-out limits. After that bootstrap, future compatible releases can be deployed through the Cloud Firmware OTA section in the Device page.

Confirm the installed partition table has an OTA update partition. A build made with the repository workflow uses `PartitionScheme=min_spiffs`, but its placeholder-secrets artifact is for compile checks only and must not be flashed to production hardware. Build the first deployable image locally using your own private `secrets_local.h`; upload only the application image (usually `AttendanceSystem.ino.bin`), not bootloader or partition-table binaries.

## Backend deployment (Bolt-managed project)

The existing `cloud-dashboard/supabase/config.toml` documents that the live backend is managed by the Bolt project. Do not deploy to an unrelated Supabase project. Apply migration `cloud-dashboard/supabase/migrations/003_cloud_ota.sql` to that existing backend, then deploy the updated `cloud-dashboard/supabase/functions/device-ingest/index.ts` and updated Device page through the existing Bolt workflow. Verify the private bucket exists and anonymous users cannot read or upload its objects. This repository commit does not itself apply the migration or deploy the hosted dashboard/edge function.

## TLS/certificate and attendance responsiveness

- Cloud heartbeat, command acknowledgements and outbox HTTPS requests run on a dedicated FreeRTOS worker pinned to core 0. The Arduino attendance loop queues work rather than waiting for a network call that can take up to 15 seconds.
- TLS verification remains enabled with the Arduino-ESP32 built-in CA bundle. No `setInsecure()` fallback is used. TLS preflight logs report total and contiguous internal heap, and request bodies are allocated after the handshake to reduce fragmentation.
- The cloud worker and explicit OTA download share a single TLS semaphore so RSA/X.509 verification cannot be running concurrently in two clients. If the TLS slot is busy, a queued OTA attempt is rejected with a retry message rather than opening a second TLS handshake.
- ESP-IDF documents that error `0x4290` can indicate an MPI allocation failure during RSA public-key verification, not necessarily an untrusted certificate. The changes reduce overlapping TLS and heap pressure, but real hardware must still be tested against the active Bolt endpoint; never bypass verification to silence the message.
- Outside an operator-triggered firmware update, fingerprint attendance keeps running while heartbeat/outbox calls are in progress. During a selected OTA update, the scanner is intentionally paused while the new image is downloaded and verified, then restarts.
- Daily attendance state is rebuilt from `attendance.csv` for the current Rwanda date on boot. Yesterday's unclosed check-in does not turn today's first scan into a checkout.

## Required cloud migration

Apply both migrations to the same Bolt-managed backend: `003_cloud_ota.sql` (private firmware release bucket/table and command type) and `004_daily_attendance_limits.sql` (serializes and skips same-day duplicate CHECK_IN/CHECK_OUT insertions without rewriting historical rows). Also deploy the updated `device-ingest` function and Overview/Device dashboard. Repository commits do not automatically apply SQL or deploy the live Bolt project.

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
