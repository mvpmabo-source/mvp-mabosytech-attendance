# MVP MABOSYTECH Attendance System

This repository contains the existing ESP32 attendance-device firmware and its current embedded dashboard. It is the baseline for the planned Bolt.new + Supabase cloud-dashboard migration.

## Important status

- Firmware version in this source: V2.9.45.
- The current dashboard is served by the ESP32's existing HTTP server and authenticates through the current firmware routes.
- Attendance history and related logs currently use files on the microSD card; profile/settings storage also relies on existing ESP32-side storage.
- The cloud dashboard, cloud database, device-to-cloud sync and command queue are **not implemented yet**.
- The firmware source has not been target-compiled or hardware-verified after the latest HTTP reliability changes. Host tests described in `firmware/AttendanceSystem/README_ULTRALITE_FINAL.md` do not replace ESP32 build/hardware testing.
- The baseline Wi-Fi SSID `bosco`, CSV schema, fingerprint slot mappings, existing authentication behavior and OTA code must not be changed casually.

## Repository layout

- `firmware/AttendanceSystem/` — Arduino ESP32 sketch and all associated source files. The sketch directory and `AttendanceSystem.ino` filename intentionally match.
- `BOLT_AI_BRIEF.md` — implementation requirements for building the cloud dashboard without breaking the existing device.
- `firmware/AttendanceSystem/README_ULTRALITE_FINAL.md` — details of current firmware/dashboard behavior, API compatibility and tests.
- `firmware/AttendanceSystem/tools/` — host-side checks and source verifiers.

## Local firmware setup

1. Install Arduino IDE or Arduino CLI and the compatible Arduino-ESP32 board package.
2. Copy `firmware/AttendanceSystem/secrets.example.h` to `firmware/AttendanceSystem/secrets_local.h`.
3. Replace the placeholder OTA password in the local file with a unique strong password. Never commit `secrets_local.h`, Wi-Fi passwords, database service-role keys, or other secrets.
4. Open/compile the `firmware/AttendanceSystem/AttendanceSystem.ino` sketch folder using the board settings documented in `README_ULTRALITE_FINAL.md`.
5. Compile first; do not upload until compile output and hardware/partition assumptions are reviewed. Never format the SD card as part of dashboard work.

## Cloud migration principle

Build the new browser dashboard as a separate application and use Supabase (or another explicitly approved backend) for cloud authentication, database and secure APIs. The ESP32 should initiate outbound HTTPS requests to sync records and poll commands; do not assume a cloud-hosted browser can directly reach the ESP32's private LAN address.

The device must remain offline-capable: keep writing attendance locally when internet/cloud is unavailable, queue unsynced records, and retry safely when connectivity returns. Cloud records must use a stable device/event identifier so retries do not create duplicate attendance. Commands from the dashboard must be authorized, acknowledged by the device, and logged.

Do not remove or rewrite existing firmware routes/storage behavior until the new cloud bridge is implemented and tested. Keep the device's local UI/API compatible during transition.
