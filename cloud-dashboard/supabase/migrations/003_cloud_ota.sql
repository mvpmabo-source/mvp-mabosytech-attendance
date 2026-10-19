/*
 * Cloud OTA foundation for MABOSYTECH ESP32 attendance devices.
 * The binaries stay in a private storage bucket. The authenticated device-ingest
 * edge function issues short-lived signed URLs after validating release metadata.
 */

ALTER TABLE public.device_commands
  DROP CONSTRAINT IF EXISTS device_commands_command_check;

ALTER TABLE public.device_commands
  ADD CONSTRAINT device_commands_command_check
  CHECK (command IN ('restart', 'sync_time', 'cloud_ota'));

CREATE TABLE IF NOT EXISTS public.firmware_releases (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  version text NOT NULL UNIQUE
    CHECK (version ~ '^V?[0-9]+\.[0-9]+\.[0-9]+([+-][A-Za-z0-9.-]+)?$'),
  storage_path text NOT NULL UNIQUE
    CHECK (storage_path ~ '^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}/[A-Za-z0-9._-]+\.bin$'),
  sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
  size_bytes bigint NOT NULL CHECK (size_bytes > 0 AND size_bytes <= 2097152),
  release_notes text,
  uploaded_by uuid REFERENCES auth.users(id) ON DELETE SET NULL,
  created_at timestamptz NOT NULL DEFAULT now()
);

ALTER TABLE public.firmware_releases ENABLE ROW LEVEL SECURITY;
DROP POLICY IF EXISTS "firmware_releases_select_authenticated" ON public.firmware_releases;
CREATE POLICY "firmware_releases_select_authenticated"
  ON public.firmware_releases FOR SELECT TO authenticated USING (true);
DROP POLICY IF EXISTS "firmware_releases_insert_authenticated" ON public.firmware_releases;
CREATE POLICY "firmware_releases_insert_authenticated"
  ON public.firmware_releases FOR INSERT TO authenticated
  WITH CHECK (uploaded_by IS NULL OR uploaded_by = auth.uid());

REVOKE ALL ON public.firmware_releases FROM anon;
GRANT SELECT, INSERT ON public.firmware_releases TO authenticated;
CREATE INDEX IF NOT EXISTS idx_firmware_releases_created_at
  ON public.firmware_releases(created_at DESC);

-- Private bucket; no anonymous/public download or upload access.
INSERT INTO storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
VALUES (
  'firmware-releases',
  'firmware-releases',
  false,
  2097152,
  ARRAY['application/octet-stream', 'application/x-binary']
)
ON CONFLICT (id) DO UPDATE
SET public = false,
    file_size_limit = 2097152,
    allowed_mime_types = ARRAY['application/octet-stream', 'application/x-binary'];

DROP POLICY IF EXISTS "firmware_bucket_upload_authenticated" ON storage.objects;
CREATE POLICY "firmware_bucket_upload_authenticated"
  ON storage.objects FOR INSERT TO authenticated
  WITH CHECK (
    bucket_id = 'firmware-releases'
    AND name ~ '^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}/[A-Za-z0-9._-]+\.bin$'
  );

-- The browser needs only SELECT/INSERT on release metadata and INSERT on the
-- storage object. It never receives the service-role key or direct download access.
