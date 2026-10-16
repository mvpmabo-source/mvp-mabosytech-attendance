-- Repair attendance rows whose metadata was uploaded as blanks or "ID <number>".
-- Match ONLY by fingerprint_id; never infer identity from name or event time.
-- Non-empty attendance metadata is preserved. The migration is safe to re-run.
BEGIN;

UPDATE public.attendance_events AS a
SET
  name = CASE
    WHEN (
      NULLIF(BTRIM(a.name), '') IS NULL
      OR BTRIM(a.name) IN ('-', '—')
      OR BTRIM(a.name) ~* ('^ID[[:space:]]*' || a.fingerprint_id::text || '$')
    )
    THEN COALESCE(NULLIF(BTRIM(t.name), ''), a.name)
    ELSE a.name
  END,
  college_id = CASE
    WHEN NULLIF(BTRIM(a.college_id), '') IS NULL OR BTRIM(a.college_id) IN ('-', '—')
    THEN COALESCE(NULLIF(BTRIM(t.college_id), ''), a.college_id)
    ELSE a.college_id
  END,
  role = CASE
    WHEN NULLIF(BTRIM(a.role), '') IS NULL OR BTRIM(a.role) IN ('-', '—')
    THEN COALESCE(NULLIF(BTRIM(t.role), ''), a.role)
    ELSE a.role
  END,
  department = CASE
    WHEN NULLIF(BTRIM(a.department), '') IS NULL OR BTRIM(a.department) IN ('-', '—')
    THEN COALESCE(NULLIF(BTRIM(t.department), ''), a.department)
    ELSE a.department
  END
FROM public.teachers AS t
WHERE t.fingerprint_id = a.fingerprint_id
  AND (
    NULLIF(BTRIM(a.name), '') IS NULL
    OR BTRIM(a.name) IN ('-', '—')
    OR BTRIM(a.name) ~* ('^ID[[:space:]]*' || a.fingerprint_id::text || '$')
    OR NULLIF(BTRIM(a.college_id), '') IS NULL
    OR BTRIM(a.college_id) IN ('-', '—')
    OR NULLIF(BTRIM(a.role), '') IS NULL
    OR BTRIM(a.role) IN ('-', '—')
    OR NULLIF(BTRIM(a.department), '') IS NULL
    OR BTRIM(a.department) IN ('-', '—')
  );

COMMIT;
