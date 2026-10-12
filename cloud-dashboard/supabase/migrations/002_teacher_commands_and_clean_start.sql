-- Commands and teacher registry required by the firmware's verified hardware workflow.
ALTER TABLE device_commands DROP CONSTRAINT IF EXISTS device_commands_command_check;
ALTER TABLE device_commands ADD CONSTRAINT device_commands_command_check
  CHECK (command IN ('restart', 'sync_time', 'enroll_fingerprint', 'delete_fingerprint', 'factory_reset'));

CREATE TABLE IF NOT EXISTS teachers (
  fingerprint_id integer PRIMARY KEY CHECK (fingerprint_id BETWEEN 1 AND 200),
  name text NOT NULL DEFAULT '',
  college_id text NOT NULL DEFAULT '',
  role text NOT NULL DEFAULT 'Teacher',
  department text NOT NULL DEFAULT '',
  note text NOT NULL DEFAULT '',
  enrollment_status text NOT NULL DEFAULT 'enrolled' CHECK (enrollment_status IN ('pending', 'enrolled', 'failed')),
  enrolled_at timestamptz,
  created_at timestamptz NOT NULL DEFAULT now(),
  updated_at timestamptz NOT NULL DEFAULT now()
);

ALTER TABLE teachers ENABLE ROW LEVEL SECURITY;
DROP POLICY IF EXISTS "select_teachers_authenticated" ON teachers;
CREATE POLICY "select_teachers_authenticated" ON teachers FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "insert_teachers_authenticated" ON teachers;
CREATE POLICY "insert_teachers_authenticated" ON teachers FOR INSERT TO authenticated WITH CHECK (true);
DROP POLICY IF EXISTS "update_teachers_authenticated" ON teachers;
CREATE POLICY "update_teachers_authenticated" ON teachers FOR UPDATE TO authenticated USING (true) WITH CHECK (true);
DROP POLICY IF EXISTS "delete_teachers_authenticated" ON teachers;
CREATE POLICY "delete_teachers_authenticated" ON teachers FOR DELETE TO authenticated USING (true);
