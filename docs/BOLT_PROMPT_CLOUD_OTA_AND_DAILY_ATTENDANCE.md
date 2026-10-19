# Prompt for Bolt: Cloud OTA + daily attendance reliability

Paste the prompt below into the existing Bolt project that hosts the MABOSYTECH Attendance dashboard. Work in the currently connected project/backend; do not create a separate replacement application.

---

## BOLT TASK PROMPT

You are updating the existing MABOSYTECH Attendance System dashboard and the existing Bolt-managed Supabase-compatible backend. This is a production attendance application. Make a careful, backward-compatible update: preserve all currently working pages, teacher records, attendance history, fingerprint enrollment/deletion, device settings, Sync Time, Restart, authentication, CSV exports, SD/cloud sync semantics, and all existing UI behavior outside the scope below. Do not reset or recreate tables, delete historical attendance, rename existing columns, or replace the project with a starter template.

### 1. Cloud Firmware OTA management

Add/finish a section on the existing **Device** page named **Cloud Firmware OTA**.

- Allow an authenticated operator to upload a compiled ESP32 application `.bin` (not a bootloader or partition-table image), enter the version string exactly as embedded in the firmware (the current target release is `V2.9.47`), and add optional release notes.
- Compute SHA-256 in the browser, store release metadata in `firmware_releases`, and show version, file size, checksum, notes, upload time, and update command history.
- Keep firmware files in the **private** Supabase Storage bucket `firmware-releases`. Never make the bucket public. Never expose or embed the service-role key in frontend code, browser bundles, logs, or prompts.
- Add a “Send to ESP32” action for each uploaded release. Confirm with the operator before queuing an update. Disable deployment if the device is offline, already has that firmware version, or fingerprint enrollment is active.
- Queue `device_commands.command = 'cloud_ota'` with only the saved release version, storage path, SHA-256 and size. Do not trust arbitrary client URLs. In the existing authenticated `device-ingest` edge function, verify the queued release metadata against `firmware_releases` and create a short-lived signed download URL using the service role, then deliver that signed URL only to the authenticated device.
- The firmware performs HTTPS certificate validation, restricts the URL to the project's trusted private Storage object path, checks exact byte length and SHA-256, validates the ESP image/OTA partition, and restarts only after a valid image is staged. Do not disable TLS verification or introduce `setInsecure()`.
- Display safe status text explaining that upload stores a release but does not flash the device, and that an initial one-time bootstrap flash is required for already-installed firmware that does not understand `cloud_ota`. Do not promise that an OTA update succeeded until the device reports its updated version/command result.

### 2. Daily teacher attendance dashboard (Rwanda time)

On the existing **Overview/Dashboard** page, add a section titled **Today's Teacher Attendance** using the existing registered `teachers` table and `attendance_events` table. Use `Africa/Kigali` to decide the current calendar date. Do not derive Rwanda's day from `toISOString().slice(0, 10)`, which is UTC and can roll over at a different local time.

- Show the registered teacher count.
- Show the number of teachers with at least one attendance event today.
- Show the number with no attendance today.
- Show the number with exactly one of today's two event types.
- Show the number who completed both check-in and check-out.
- Show a searchable/scrollable table containing every registered teacher and their status for today's date.
- Row/status colors: **green** only when a teacher has today's `CHECK_IN` and today's `CHECK_OUT`; **yellow/amber** when exactly one of those event types exists; **red** when neither exists. Include text labels/icons as well as colors so status is accessible.
- Calculate status based on the distinct presence of event types, not raw row count: legacy duplicate rows must not turn a one-event teacher green.
- Distinguish “one event only” from “no attendance”. A teacher with only a check-out row from legacy/corrupt data still needs a warning state; do not silently treat that as complete.
- Keep existing overview cards, device connectivity/RTC/SD information, and refresh behavior. Add clear loading/error/empty states, and make the layout responsive.

### 3. Enforce the daily business rule in the cloud

Use the existing migration chain and current backend. Review the repository's `003_cloud_ota.sql` and `004_daily_attendance_limits.sql` and apply the matching SQL safely to this same Bolt-managed backend.

Business rule for each teacher/fingerprint and Kigali calendar day:
- At most one `CHECK_IN` and at most one `CHECK_OUT`.
- A normal complete day consists of one check-in followed by one check-out.
- If a teacher checks in today but does not check out, the next day's first scan must be a new day's **CHECK_IN**. It must NOT create the missing previous-day checkout.
- After both event types are recorded for the current day, additional scans must not create extra attendance records.
- Protect the backend as well as the device against duplicate event types with different event UIDs, while allowing idempotent retries of the same `event_uid`.
- Do not delete, rewrite, or attempt to “repair” historical attendance rows as part of this rule. Keep all past attendance for audit/reporting.
- Ensure the trigger/RPC/edge-function implementation is atomic enough to avoid race-condition duplicates. Do not make an update that silently overwrites the first attendance event.

### 4. Migration and API safety

- Review the current schema before changing anything; do not drop or recreate `attendance_events`, `teachers`, `device_commands`, `device_heartbeats`, or auth tables.
- Use idempotent migrations and preserve all existing command types, including `restart`, `sync_time`, `update_settings`, enrollment, and fingerprint deletion.
- Keep RLS enabled. Only signed-in dashboard operators may upload firmware or manage release metadata. The firmware binary bucket must remain private. Only the authenticated device-ingest edge function (service-role backend side) should create signed download URLs.
- Do not expose API keys or secrets in client-side code. Do not weaken certificate checks.
- Make date comparisons consistently use the Rwanda local calendar date for attendance summaries. Avoid mixing UTC and Kigali dates.

### 5. Test and report before calling it done

Run the frontend build/type checks and the backend/migration checks available in this Bolt project. Test these cases with representative test data or a staging device:
1. A teacher with no attendance today is red and counted as absent.
2. A teacher with only CHECK_IN today is yellow and counted as one event.
3. A teacher with CHECK_IN and CHECK_OUT today is green and counted as complete.
4. Replayed same `event_uid` does not create duplicates.
5. A different event UID attempting the same teacher/date/event type is skipped/rejected without altering the original row.
6. Yesterday's CHECK_IN without CHECK_OUT does not prevent today's CHECK_IN.
7. The third scan after both events on one day creates no extra cloud attendance row.
8. Firmware OTA release upload calculates SHA-256; uploading alone does not send an update.
9. Only authenticated device heartbeat receives a short-lived signed URL for the selected private release.
10. Existing Sync Time, Restart, enrollment/deletion, current records, and dashboard navigation continue working.

If a migration cannot be applied safely or live secrets/settings are missing, stop and report the precise blocker instead of bypassing validation or weakening security. At the end, report the files/migrations changed, build/test results, which migrations were actually applied to this existing backend, and whether the dashboard was deployed. Do not claim production deployment or ESP32 hardware verification unless those actions really succeeded.

---

## Deployment checklist

- Apply `003_cloud_ota.sql` (including the corrected version constraint) and `004_daily_attendance_limits.sql` to the existing Bolt-managed backend.
- Deploy the updated `device-ingest` edge function and dashboard from the same project.
- Confirm `firmware-releases` is private and RLS prevents anonymous reads/uploads.
- Bootstrap the ESP32 once with the cloud-OTA-enabled `V2.9.47` firmware via the existing LAN ArduinoOTA workflow or USB; then test cloud OTA from the dashboard.
- Test local check-in/out and reboot recovery before production rollout.
