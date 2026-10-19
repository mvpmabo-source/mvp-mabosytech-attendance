/*
 * Fix Cloud OTA Storage RLS.
 *
 * The original policy in 003_cloud_ota.sql used a doubly escaped "\\.bin"
 * regex which may not match ordinary .bin paths with PostgreSQL's default
 * standard_conforming_strings setting. Also, authenticated uploads need a
 * narrowly scoped SELECT policy so Storage can RETURN the newly inserted
 * object's metadata without granting list or download access.
 *
 * Keep firmware-releases PRIVATE. Device downloads still use short-lived
 * signed URLs created server-side by device-ingest.
 */

-- Replace the insert rule with a path check compatible with PostgreSQL regexes.
DROP POLICY IF EXISTS "firmware_bucket_upload_authenticated" ON storage.objects;
CREATE POLICY "firmware_bucket_upload_authenticated"
  ON storage.objects
  FOR INSERT
  TO authenticated
  WITH CHECK (
    bucket_id = 'firmware-releases'
    AND (storage.foldername(name))[1] ~ '^V[0-9]+[.][0-9]+[.][0-9]+([+-][A-Za-z0-9.-]+)?$'
    AND array_length(storage.foldername(name), 1) = 1
    AND storage.extension(name) = 'bin'
  );

-- Allow only the Storage metadata-return operation for an eligible firmware
-- path. Do not allow listing the bucket or downloading the private object.
DROP POLICY IF EXISTS "firmware_bucket_metadata_select_authenticated" ON storage.objects;
CREATE POLICY "firmware_bucket_metadata_select_authenticated"
  ON storage.objects
  FOR SELECT
  TO authenticated
  USING (
    bucket_id = 'firmware-releases'
    AND storage.allow_only_operation('object.get_authenticated_info')
    AND (storage.foldername(name))[1] ~ '^V[0-9]+[.][0-9]+[.][0-9]+([+-][A-Za-z0-9.-]+)?$'
    AND array_length(storage.foldername(name), 1) = 1
    AND storage.extension(name) = 'bin'
  );
