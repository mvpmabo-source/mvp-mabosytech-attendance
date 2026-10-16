/*
# ESP32 Attendance Cloud Schema

Creates the cloud database tables for the MVP MABOSYTECH ESP32 Attendance System.
The ESP32 device uploads attendance events and heartbeats to an edge function
which writes to these tables. The hosted dashboard reads from them.

## Security
- RLS enabled on all tables.
- Dashboard tables scoped to authenticated role (dashboard login required).
- Device writes go through the edge function using the service role key.
- The browser never sees the service role key.
*/

CREATE TABLE IF NOT EXISTS devices (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id text UNIQUE NOT NULL,
  api_key_hash text NOT NULL,
  name text NOT NULL DEFAULT 'MBS BA MVP',
  firmware_version text,
  last_seen_at timestamptz,
  created_at timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now()
);

ALTER TABLE devices ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "select_devices_authenticated" ON devices;
CREATE POLICY "select_devices_authenticated" ON devices
  FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_devices_authenticated" ON devices;
CREATE POLICY "insert_devices_authenticated" ON devices
  FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_devices_authenticated" ON devices;
CREATE POLICY "update_devices_authenticated" ON devices
  FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_devices_authenticated" ON devices;
CREATE POLICY "delete_devices_authenticated" ON devices
  FOR DELETE TO authenticated USING (true);

CREATE TABLE IF NOT EXISTS attendance_events (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  event_uid text UNIQUE NOT NULL,
  device_id text NOT NULL,
  event_date date NOT NULL,
  event_time time NOT NULL,
  fingerprint_id integer NOT NULL CHECK (fingerprint_id >= 1 AND fingerprint_id <= 200),
  name text NOT NULL DEFAULT '',
  college_id text NOT NULL DEFAULT '',
  role text NOT NULL DEFAULT '',
  department text NOT NULL DEFAULT '',
  event_type text NOT NULL CHECK (event_type IN ('CHECK_IN', 'CHECK_OUT')),
  status text NOT NULL DEFAULT 'ON_TIME' CHECK (status IN ('ON_TIME', 'LATE', 'EARLY', 'LEFT_EARLY', 'OVERTIME')),
  synced_at timestamptz DEFAULT now(),
  created_at timestamptz DEFAULT now()
);

ALTER TABLE attendance_events ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "select_attendance_authenticated" ON attendance_events;
CREATE POLICY "select_attendance_authenticated" ON attendance_events
  FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_attendance_authenticated" ON attendance_events;
CREATE POLICY "insert_attendance_authenticated" ON attendance_events
  FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_attendance_authenticated" ON attendance_events;
CREATE POLICY "update_attendance_authenticated" ON attendance_events
  FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_attendance_authenticated" ON attendance_events;
CREATE POLICY "delete_attendance_authenticated" ON attendance_events
  FOR DELETE TO authenticated USING (true);

CREATE TABLE IF NOT EXISTS device_heartbeats (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id text NOT NULL,
  firmware_version text,
  wifi_connected boolean DEFAULT false,
  ip_address text,
  rssi integer,
  free_heap integer,
  sd_available boolean DEFAULT false,
  rtc_available boolean DEFAULT false,
  rtc_time_valid boolean DEFAULT false,
  templates integer DEFAULT 0,
  capacity integer DEFAULT 200,
  entry_count integer DEFAULT 0,
  exit_count integer DEFAULT 0,
  pending_count integer DEFAULT 0,
  checkin_time text,
  checkout_time text,
  device_name text,
  is_enrolling boolean DEFAULT false,
  fingerprint_ok boolean DEFAULT false,
  reported_at timestamptz DEFAULT now(),
  received_at timestamptz DEFAULT now()
);

ALTER TABLE device_heartbeats ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "select_heartbeats_authenticated" ON device_heartbeats;
CREATE POLICY "select_heartbeats_authenticated" ON device_heartbeats
  FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_heartbeats_authenticated" ON device_heartbeats;
CREATE POLICY "insert_heartbeats_authenticated" ON device_heartbeats
  FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_heartbeats_authenticated" ON device_heartbeats;
CREATE POLICY "update_heartbeats_authenticated" ON device_heartbeats
  FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_heartbeats_authenticated" ON device_heartbeats;
CREATE POLICY "delete_heartbeats_authenticated" ON device_heartbeats
  FOR DELETE TO authenticated USING (true);

CREATE TABLE IF NOT EXISTS device_commands (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id text NOT NULL,
  command text NOT NULL CHECK (command IN ('restart', 'sync_time')),
  payload jsonb DEFAULT '{}'::jsonb,
  status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'acknowledged', 'failed')),
  created_by uuid,
  created_at timestamptz DEFAULT now(),
  acknowledged_at timestamptz
);

ALTER TABLE device_commands ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "select_commands_authenticated" ON device_commands;
CREATE POLICY "select_commands_authenticated" ON device_commands
  FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_commands_authenticated" ON device_commands;
CREATE POLICY "insert_commands_authenticated" ON device_commands
  FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_commands_authenticated" ON device_commands;
CREATE POLICY "update_commands_authenticated" ON device_commands
  FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_commands_authenticated" ON device_commands;
CREATE POLICY "delete_commands_authenticated" ON device_commands
  FOR DELETE TO authenticated USING (true);

CREATE TABLE IF NOT EXISTS audit_log (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id uuid,
  action text NOT NULL,
  target text,
  details jsonb DEFAULT '{}'::jsonb,
  created_at timestamptz DEFAULT now()
);

ALTER TABLE audit_log ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "select_audit_authenticated" ON audit_log;
CREATE POLICY "select_audit_authenticated" ON audit_log
  FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_audit_authenticated" ON audit_log;
CREATE POLICY "insert_audit_authenticated" ON audit_log
  FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_audit_authenticated" ON audit_log;
CREATE POLICY "update_audit_authenticated" ON audit_log
  FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_audit_authenticated" ON audit_log;
CREATE POLICY "delete_audit_authenticated" ON audit_log
  FOR DELETE TO authenticated USING (true);

CREATE INDEX IF NOT EXISTS idx_attendance_device_date ON attendance_events(device_id, event_date DESC);
CREATE INDEX IF NOT EXISTS idx_attendance_date ON attendance_events(event_date DESC);
CREATE INDEX IF NOT EXISTS idx_attendance_fingerprint ON attendance_events(fingerprint_id);
CREATE INDEX IF NOT EXISTS idx_attendance_event_type ON attendance_events(event_type);
CREATE INDEX IF NOT EXISTS idx_attendance_status ON attendance_events(status);
CREATE INDEX IF NOT EXISTS idx_heartbeats_device ON device_heartbeats(device_id, reported_at DESC);
CREATE INDEX IF NOT EXISTS idx_heartbeats_recent ON device_heartbeats(received_at DESC);
CREATE INDEX IF NOT EXISTS idx_commands_device_status ON device_commands(device_id, status);
CREATE INDEX IF NOT EXISTS idx_audit_created ON audit_log(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen_at DESC);
