import { createClient } from "npm:@supabase/supabase-js@2.57.4";

const corsHeaders = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, PUT, DELETE, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization, X-Client-Info, Apikey",
};

async function sha256Hex(input: string): Promise<string> {
  const data = new TextEncoder().encode(input);
  const hashBuffer = await crypto.subtle.digest("SHA-256", data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  return hashArray.map((b) => b.toString(16).padStart(2, "0")).join("");
}

const supabaseUrl = Deno.env.get("SUPABASE_URL")!;
const serviceRoleKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

const adminClient = createClient(supabaseUrl, serviceRoleKey, {
  auth: { persistSession: false },
});

const VALID_EVENT_TYPES = new Set(["CHECK_IN", "CHECK_OUT"]);
const VALID_STATUSES = new Set([
  "ON_TIME",
  "LATE",
  "EARLY",
  "LEFT_EARLY",
  "OVERTIME",
]);

interface AttendancePayload {
  event_uid: string;
  date: string;
  time: string;
  fingerprint_id: number;
  name?: string;
  college_id?: string;
  role?: string;
  department?: string;
  event_type: string;
  status: string;
}

interface HeartbeatPayload {
  firmware_version?: string;
  wifi_connected?: boolean;
  ip_address?: string;
  rssi?: number;
  free_heap?: number;
  sd_available?: boolean;
  rtc_available?: boolean;
  rtc_time_valid?: boolean;
  templates?: number;
  capacity?: number;
  entry_count?: number;
  exit_count?: number;
  pending_count?: number;
  checkin_time?: string;
  checkout_time?: string;
  device_name?: string;
  is_enrolling?: boolean;
  fingerprint_ok?: boolean;
  reported_at?: string;
}

function jsonError(status: number, message: string, extra: Record<string, unknown> = {}) {
  return new Response(JSON.stringify({ ok: false, error: message, ...extra }), {
    status,
    headers: { ...corsHeaders, "Content-Type": "application/json" },
  });
}

function jsonOk(data: Record<string, unknown>) {
  return new Response(JSON.stringify({ ok: true, ...data }), {
    status: 200,
    headers: { ...corsHeaders, "Content-Type": "application/json" },
  });
}

async function authenticateDevice(deviceId: string, apiKey: string) {
  if (!deviceId || !apiKey) return null;
  const { data, error } = await adminClient
    .from("devices")
    .select("id, device_id, api_key_hash, name")
    .eq("device_id", deviceId)
    .maybeSingle();

  if (error || !data) return null;
  const providedHash = await sha256Hex(apiKey);
  if (providedHash !== data.api_key_hash) return null;
  return data;
}

function validateDateString(s: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(s);
}

function validateTimeString(s: string): boolean {
  return /^\d{2}:\d{2}:\d{2}$/.test(s);
}

Deno.serve(async (req: Request) => {
  if (req.method === "OPTIONS") {
    return new Response(null, { status: 200, headers: corsHeaders });
  }

  if (req.method !== "POST") {
    return jsonError(405, "Method not allowed");
  }

  try {
    const body = await req.json();
    const deviceId = body.device_id as string;
    const apiKey = body.api_key as string;
    const type = body.type as string;

    if (!type || !["attendance", "heartbeat", "batch", "ack_command"].includes(type)) {
      return jsonError(400, "Missing or invalid 'type' field");
    }

    const device = await authenticateDevice(deviceId, apiKey);
    if (!device) {
      return jsonError(401, "Invalid device credentials");
    }

    await adminClient
      .from("devices")
      .update({ last_seen_at: new Date().toISOString(), updated_at: new Date().toISOString() })
      .eq("device_id", deviceId);

    if (type === "attendance") {
      const event = body.event as AttendancePayload;
      if (!event) return jsonError(400, "Missing 'event' payload");

      if (!event.event_uid || event.event_uid.length < 4) {
        return jsonError(400, "Missing or invalid event_uid");
      }
      if (!validateDateString(event.date) || !validateTimeString(event.time)) {
        return jsonError(400, "Invalid date or time format");
      }
      if (!event.fingerprint_id || event.fingerprint_id < 1 || event.fingerprint_id > 200) {
        return jsonError(400, "fingerprint_id must be 1-200");
      }
      if (!VALID_EVENT_TYPES.has(event.event_type)) {
        return jsonError(400, "Invalid event_type");
      }
      if (!VALID_STATUSES.has(event.status)) {
        return jsonError(400, "Invalid status value");
      }

      const { error: insertError } = await adminClient
        .from("attendance_events")
        .upsert(
          {
            event_uid: event.event_uid,
            device_id: deviceId,
            event_date: event.date,
            event_time: event.time,
            fingerprint_id: event.fingerprint_id,
            name: event.name || "",
            college_id: event.college_id || "",
            role: event.role || "",
            department: event.department || "",
            event_type: event.event_type,
            status: event.status,
            synced_at: new Date().toISOString(),
          },
          { onConflict: "event_uid", ignoreDuplicates: true }
        );

      if (insertError) {
        return jsonError(500, "Database write failed", { detail: insertError.message });
      }

      return jsonOk({ received: true, event_uid: event.event_uid, duplicate_safe: true });
    }

    if (type === "heartbeat") {
      const hb = body.heartbeat as HeartbeatPayload;
      if (!hb) return jsonError(400, "Missing 'heartbeat' payload");

      const reportedAt = hb.reported_at || new Date().toISOString();

      const { error: hbError } = await adminClient.from("device_heartbeats").insert({
        device_id: deviceId,
        firmware_version: hb.firmware_version || null,
        wifi_connected: hb.wifi_connected ?? false,
        ip_address: hb.ip_address || null,
        rssi: hb.rssi ?? null,
        free_heap: hb.free_heap ?? null,
        sd_available: hb.sd_available ?? false,
        rtc_available: hb.rtc_available ?? false,
        rtc_time_valid: hb.rtc_time_valid ?? false,
        templates: hb.templates ?? 0,
        capacity: hb.capacity ?? 200,
        entry_count: hb.entry_count ?? 0,
        exit_count: hb.exit_count ?? 0,
        pending_count: hb.pending_count ?? 0,
        checkin_time: hb.checkin_time || null,
        checkout_time: hb.checkout_time || null,
        device_name: hb.device_name || null,
        is_enrolling: hb.is_enrolling ?? false,
        fingerprint_ok: hb.fingerprint_ok ?? false,
        reported_at: reportedAt,
        received_at: new Date().toISOString(),
      });

      if (hbError) {
        return jsonError(500, "Heartbeat write failed", { detail: hbError.message });
      }

      if (hb.firmware_version) {
        await adminClient
          .from("devices")
          .update({ firmware_version: hb.firmware_version })
          .eq("device_id", deviceId);
      }

      // Older dashboard builds have stored device_commands.device_id as either
      // devices.device_id (human-readable ESP ID) or devices.id (database UUID).
      // Resolve commands only against the authenticated device's own identifiers.
      const commandTargets = [...new Set([String(device.device_id), String(device.id)].filter(Boolean))];
      const pendingById = new Map<string, {
        id: string;
        command: string;
        payload: Record<string, unknown> | null;
        created_at: string;
      }>();
      let successfulCommandLookup = false;

      for (const targetId of commandTargets) {
        const { data: rows, error: commandQueryError } = await adminClient
          .from("device_commands")
          .select("id, device_id, command, payload, created_at")
          .eq("device_id", targetId)
          .eq("status", "pending")
          .order("created_at", { ascending: true })
          .limit(5);

        if (commandQueryError) {
          // A UUID column rejects the text ESP ID; the UUID lookup below can still succeed.
          console.warn("Pending command lookup failed for one device identifier:", commandQueryError.message);
          continue;
        }

        successfulCommandLookup = true;
        for (const row of rows || []) {
          // Never deliver rows not explicitly assigned to this authenticated device.
          if (!commandTargets.includes(String(row.device_id))) continue;
          if (!pendingById.has(String(row.id))) {
            pendingById.set(String(row.id), {
              id: String(row.id),
              command: String(row.command),
              payload: row.payload && typeof row.payload === "object" ? row.payload as Record<string, unknown> : {},
              created_at: String(row.created_at),
            });
          }
        }
      }

      if (!successfulCommandLookup) {
        return jsonError(500, "Pending command lookup failed");
      }

      const pendingCommands = [...pendingById.values()]
        .sort((a, b) => a.created_at.localeCompare(b.created_at))
        .slice(0, 5)
        .map(({ id, command, payload, created_at }) => ({ id, command, payload, created_at }));

      if (pendingCommands.length > 0) {
        console.log(`Delivering ${pendingCommands.length} pending command(s) to authenticated device ${device.device_id}`);
      }

      return jsonOk({
        received: true,
        pending_commands: pendingCommands,
      });
    }

    if (type === "batch") {
      const events = body.events as AttendancePayload[];
      if (!Array.isArray(events) || events.length === 0) {
        return jsonError(400, "Missing or empty 'events' array");
      }
      if (events.length > 50) {
        return jsonError(400, "Batch too large (max 50 events)");
      }

      const rows: Record<string, unknown>[] = [];
      for (const event of events) {
        if (!event.event_uid || event.event_uid.length < 4) continue;
        if (!validateDateString(event.date) || !validateTimeString(event.time)) continue;
        if (!event.fingerprint_id || event.fingerprint_id < 1 || event.fingerprint_id > 200) continue;
        if (!VALID_EVENT_TYPES.has(event.event_type)) continue;
        if (!VALID_STATUSES.has(event.status)) continue;

        rows.push({
          event_uid: event.event_uid,
          device_id: deviceId,
          event_date: event.date,
          event_time: event.time,
          fingerprint_id: event.fingerprint_id,
          name: event.name || "",
          college_id: event.college_id || "",
          role: event.role || "",
          department: event.department || "",
          event_type: event.event_type,
          status: event.status,
          synced_at: new Date().toISOString(),
        });
      }

      if (rows.length === 0) {
        return jsonError(400, "No valid events in batch after validation");
      }

      const { error: batchError } = await adminClient
        .from("attendance_events")
        .upsert(rows, { onConflict: "event_uid", ignoreDuplicates: true });

      if (batchError) {
        return jsonError(500, "Batch write failed", { detail: batchError.message });
      }

      return jsonOk({ received: true, accepted: rows.length });
    }

    if (type === "ack_command") {
      const commandId = body.command_id as string;
      const ackStatus = body.ack_status as string;
      if (!commandId || !ackStatus || !["acknowledged", "failed"].includes(ackStatus)) {
        return jsonError(400, "Missing command_id or invalid ack_status");
      }

      // Resolve by command ID first, then verify ownership against both identifiers
      // of the already-authenticated device. Using deviceId directly here breaks
      // installations where device_commands.device_id is a foreign-key UUID.
      const { data: commandRow, error: commandLookupError } = await adminClient
        .from("device_commands")
        .select("id, device_id, command, payload, status")
        .eq("id", commandId)
        .maybeSingle();

      if (commandLookupError) {
        return jsonError(500, "Command lookup failed", { detail: commandLookupError.message });
      }
      if (!commandRow) {
        return jsonError(404, "Command not found");
      }

      const authenticatedDeviceIds = new Set([String(device.device_id), String(device.id)]);
      if (!authenticatedDeviceIds.has(String(commandRow.device_id))) {
        return jsonError(403, "Command does not belong to this device");
      }

      // A retry after a lost HTTP response is safe: if the same final status was
      // already stored, acknowledge it idempotently instead of trapping the ESP queue.
      if (commandRow.status !== "pending") {
        if (commandRow.status === ackStatus) {
          return jsonOk({ acknowledged: true, already_processed: true });
        }
        return jsonError(409, "Command already finalized with a different status");
      }

      const result = (body.result && typeof body.result === "object")
        ? body.result as Record<string, unknown>
        : {};
      const fingerprintId = Number(result.fingerprint_id || 0);
      const payload = (commandRow.payload && typeof commandRow.payload === "object")
        ? commandRow.payload as Record<string, unknown>
        : {};

      // On successful enrollment, link the sensor-assigned template ID to the
      // teacher created in the dashboard. A missing optional teacher_id must
      // never cause an unrelated profile to be updated.
      if (ackStatus === "acknowledged" &&
          ["enroll_fingerprint", "enroll"].includes(commandRow?.command || "") &&
          Number.isInteger(fingerprintId) && fingerprintId >= 1 && fingerprintId <= 200 &&
          typeof payload.teacher_id === "string" && payload.teacher_id.length > 0) {
        const { error: teacherError } = await adminClient
          .from("teachers")
          .update({ fingerprint_id: fingerprintId, enrollment_status: "enrolled", updated_at: new Date().toISOString() })
          .eq("id", payload.teacher_id);
        if (teacherError) {
          console.error("Teacher fingerprint link update failed:", teacherError.message);
        }
      }

      const { error: ackError } = await adminClient
        .from("device_commands")
        .update({ status: ackStatus, acknowledged_at: new Date().toISOString() })
        .eq("id", commandId)
        .eq("device_id", commandRow.device_id)
        .eq("status", "pending");

      if (ackError) {
        return jsonError(500, "Command acknowledgment failed", { detail: ackError.message });
      }

      return jsonOk({ acknowledged: true });
    }

    return jsonError(400, "Unhandled request type");
  } catch (err) {
    return jsonError(500, "Internal server error", { detail: err.message });
  }
});
