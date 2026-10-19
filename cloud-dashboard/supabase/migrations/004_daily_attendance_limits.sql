/*
 * Daily attendance invariant:
 * - One CHECK_IN and one CHECK_OUT maximum per fingerprint/teacher per local
 *   attendance date.
 * - Existing historical rows are preserved; this migration does not delete
 *   or rewrite legacy duplicates.
 * - Concurrent inserts are serialized by a transaction-scoped advisory lock.
 * - A repeated event with the same event_uid remains idempotent and is handled
 *   by the existing ON CONFLICT(event_uid) DO NOTHING device-ingest upsert.
 *
 * If another event of the same type already exists for this teacher/date, skip
 * the new row by returning NULL from this BEFORE INSERT trigger.
 */

CREATE OR REPLACE FUNCTION public.enforce_daily_attendance_pair()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
BEGIN
  -- Ignore invalid/incomplete data here; the device-ingest endpoint validates
  -- the input shape before insertion. The existing table constraints remain.
  IF NEW.fingerprint_id IS NULL OR NEW.event_date IS NULL OR
     NEW.event_type NOT IN ('CHECK_IN', 'CHECK_OUT') THEN
    RETURN NEW;
  END IF;

  -- A single device/system may retry the exact same event_uid. Keep that retry
  -- idempotent; the table's unique event_uid constraint handles it.
  PERFORM pg_advisory_xact_lock(
    hashtextextended(
      NEW.fingerprint_id::text || ':' || NEW.event_date::text || ':' || NEW.event_type,
      0
    )
  );

  IF EXISTS (
    SELECT 1
    FROM public.attendance_events AS existing
    WHERE existing.fingerprint_id = NEW.fingerprint_id
      AND existing.event_date = NEW.event_date
      AND existing.event_type = NEW.event_type
      AND existing.event_uid <> NEW.event_uid
  ) THEN
    RAISE LOG 'Ignoring duplicate daily attendance: fingerprint_id=%, date=%, event_type=%',
      NEW.fingerprint_id, NEW.event_date, NEW.event_type;
    RETURN NULL;
  END IF;

  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS attendance_events_daily_limit ON public.attendance_events;
CREATE TRIGGER attendance_events_daily_limit
  BEFORE INSERT ON public.attendance_events
  FOR EACH ROW
  EXECUTE FUNCTION public.enforce_daily_attendance_pair();

REVOKE ALL ON FUNCTION public.enforce_daily_attendance_pair() FROM PUBLIC, anon, authenticated;

-- Supporting index for the daily trigger. It does not remove or mutate history.
CREATE INDEX IF NOT EXISTS idx_attendance_daily_pair
  ON public.attendance_events(fingerprint_id, event_date, event_type);
