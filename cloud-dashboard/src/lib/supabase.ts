import { createClient } from "@supabase/supabase-js";

const supabaseUrl = import.meta.env.VITE_SUPABASE_URL;
const supabaseAnonKey = import.meta.env.VITE_SUPABASE_ANON_KEY;

export const supabase = createClient(supabaseUrl, supabaseAnonKey, {
  auth: {
    persistSession: true,
    autoRefreshToken: true,
  },
});

export type AttendanceEvent = {
  id: string;
  event_uid: string;
  device_id: string;
  event_date: string;
  event_time: string;
  fingerprint_id: number;
  name: string;
  college_id: string;
  role: string;
  department: string;
  event_type: "CHECK_IN" | "CHECK_OUT";
  status: "ON_TIME" | "LATE" | "EARLY" | "LEFT_EARLY" | "OVERTIME";
  synced_at: string;
  created_at: string;
};

export type DeviceHeartbeat = {
  id: string;
  device_id: string;
  firmware_version: string | null;
  wifi_connected: boolean;
  ip_address: string | null;
  rssi: number | null;
  free_heap: number | null;
  sd_available: boolean;
  rtc_available: boolean;
  rtc_time_valid: boolean;
  templates: number;
  capacity: number;
  entry_count: number;
  exit_count: number;
  pending_count: number;
  checkin_time: string | null;
  checkout_time: string | null;
  device_name: string | null;
  is_enrolling: boolean;
  fingerprint_ok: boolean;
  reported_at: string;
  received_at: string;
};

export type Device = {
  id: string;
  device_id: string;
  name: string;
  firmware_version: string | null;
  last_seen_at: string | null;
  created_at: string;
  updated_at: string;
};

export type DeviceCommand = {
  id: string;
  device_id: string;
  command: "restart" | "sync_time";
  payload: Record<string, unknown>;
  status: "pending" | "acknowledged" | "failed";
  created_by: string | null;
  created_at: string;
  acknowledged_at: string | null;
};

export type AuditLogEntry = {
  id: string;
  user_id: string | null;
  action: string;
  target: string | null;
  details: Record<string, unknown>;
  created_at: string;
};

export const STATUS_LABELS: Record<string, string> = {
  ON_TIME: "On Time",
  LATE: "Late",
  EARLY: "Early",
  LEFT_EARLY: "Left Early",
  OVERTIME: "Overtime",
};

export const STATUS_COLORS: Record<string, string> = {
  ON_TIME: "bg-emerald-100 text-emerald-700 border-emerald-200",
  LATE: "bg-amber-100 text-amber-700 border-amber-200",
  EARLY: "bg-sky-100 text-sky-700 border-sky-200",
  LEFT_EARLY: "bg-orange-100 text-orange-700 border-orange-200",
  OVERTIME: "bg-violet-100 text-violet-700 border-violet-200",
};

export const EVENT_LABELS: Record<string, string> = {
  CHECK_IN: "Check In",
  CHECK_OUT: "Check Out",
};
