# Bolt AI Brief — MVP MABOSYTECH Attendance Dashboard

You are working in an existing ESP32 attendance system repository. **Inspect the firmware and its documentation before making code changes.** This is an integration task, not a greenfield attendance app.

## Main goal

Build a modern, responsive, hosted web dashboard in a new top-level folder such as `web-dashboard/`, using Bolt's supported web stack and Supabase for cloud database/authentication/API. The dashboard must be designed to integrate with the existing hardware firmware, while the ESP32 firmware remains responsible for fingerprint recognition, attendance decisions, RTC/local time, SD-card operations, local warning buzzer, offline recording and device-level control.

Do not replace the firmware's embedded dashboard initially. Build the cloud dashboard beside it so the old local dashboard remains a fallback during development.

## Required first step: inspect, don't assume

Read at least:

- `firmware/AttendanceSystem/README_ULTRALITE_FINAL.md`
- `firmware/AttendanceSystem/api.cpp`
- `firmware/AttendanceSystem/AttendanceSystem.ino`
- `firmware/AttendanceSystem/config.h`
- `firmware/AttendanceSystem/attendance.cpp`
- `firmware/AttendanceSystem/sd_manager.cpp`
- `firmware/AttendanceSystem/storage.cpp`
- `firmware/AttendanceSystem/reports.cpp`
- `firmware/AttendanceSystem/wifi_manager.cpp`
- `firmware/AttendanceSystem/dashboard/index.html`

Document actual routes, payload shapes, CSV fields, existing settings and roles before defining cloud data models. Do not guess the current API response contracts; preserve what the source actually implements. The local device routes include existing authentication/status, teachers, attendance/history/report/export, SD status, system logs/settings and device operations. Keep current routes unchanged during the first cloud-dashboard implementation.

## Dashboard pages

Implement a clean admin experience with:

1. Overview — online/offline/last-seen device state, SD-card state when last reported, attendance totals and sync health.
2. Attendance — searchable/filterable table, date range, record detail, pagination and export.
3. People/teachers — records and profile fields based on the existing firmware's actual data model; do not break fingerprint-slot identity.
4. Reports — daily/range summaries and CSV export.
5. Device management — firmware version, Wi-Fi/cloud status, SD status, last sync, errors and pending unsynced records.
6. Audit/settings — authenticated role-aware settings and an audit trail for actions.

Avoid showing fabricated values. Show clear loading, offline, stale-data, empty and error states. Make the UI accessible, responsive, and suitable for an office manager.

## Architecture rules

- Hosted browser UI talks to Supabase through properly authorized APIs; it must not depend on reaching a private ESP32 IP from the public web app.
- ESP32 initiates outbound HTTPS communication to the cloud when connected to a router with internet. Do not switch the existing `bosco` Wi-Fi configuration, enable AP-only mode, or change networking without a documented decision.
- Cloud database must use row-level security and role-based access. Never put a Supabase service-role key or any privileged secret in browser code.
- ESP32 device authentication must be independent from a user's dashboard login; design per-device credentials and server-side validation/rotation. Do not hardcode cloud secrets in committed source.
- Use a durable outbox on the device (or another explicitly justified queue) so records are still saved to SD during internet outages. Sync only after local save succeeds; mark synced only after server acknowledgement.
- Give each record a stable unique ID / idempotency key. Retries must not duplicate records. Define and document timestamp/time-zone handling.
- Dashboard commands must be stored as pending commands, authenticated, constrained to an allowlist, checked by the device, and acknowledged with success/failure. No arbitrary firmware/code execution.
- Device status should include freshness/last-seen timestamp. An absent heartbeat is not proof that the device is broken; show offline/stale.
- Existing SD CSV formats and local attendance behavior must remain intact. Do not format, erase, migrate or rewrite SD files as part of this task.
- Existing local login/session and routes must continue working during rollout. Keep cloud dashboard deployment independent from firmware upload.
- Do not make any OTA upload, firmware flash or physical-device changes.

## Security and delivery

- Add safe SQL migrations/schema definitions, indexes and RLS policies.
- Validate all inputs server-side and log administrative actions.
- Keep secrets in environment variables / managed secrets only; include `.env.example` without real values.
- Add tests for authorization boundaries, duplicate sync retries, offline queue behavior, command acknowledgement and basic dashboard flows.
- Provide a short architecture diagram and setup instructions.
- Work in small commits and show the implementation plan before making major firmware changes. Do not overwrite or delete the existing firmware files.

## Acceptance criteria

1. A manager can securely sign in and see real synced data without directly connecting to the ESP32's private address.
2. A sample record can be posted by a device-authenticated API and shown by the dashboard.
3. Retrying the same record does not create duplicates.
4. Local attendance remains functional with no internet, and pending records can sync later.
5. A dashboard command is recorded, delivered by device polling, and visibly acknowledged.
6. Existing local dashboard/API and SD data remain unaffected until a separately tested migration is explicitly approved.
