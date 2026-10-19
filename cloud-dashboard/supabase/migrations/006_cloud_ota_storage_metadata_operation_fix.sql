/*
 * Cloud OTA Storage RLS follow-up.
 *
 * The scoped SELECT rule initially allowed only the legacy authenticated-info
 * operation. During Storage upload the new row may be checked under the
 * current storage.object.upload operation while Storage returns the inserted
 * object's metadata. Allow those two operations only, retaining the exact
 * firmware bucket and path restrictions.
 *
 * This does not authorize object.list or storage.object.get_authenticated,
 * so it does not grant ordinary listing or payload downloads.
 */

DROP POLICY IF EXISTS "firmware_bucket_metadata_select_authenticated"
  ON storage.objects;

CREATE POLICY "firmware_bucket_metadata_select_authenticated"
  ON storage.objects
  FOR SELECT
  TO authenticated
  USING (
    bucket_id = 'firmware-releases'
    AND storage.allow_any_operation(ARRAY[
      'storage.object.upload',
      'object.get_authenticated_info'
    ])
    AND (storage.foldername(name))[1]
      ~ '^V[0-9]+[.][0-9]+[.][0-9]+([+-][A-Za-z0-9.-]+)?$'
    AND array_length(storage.foldername(name), 1) = 1
    AND storage.extension(name) = 'bin'
  );
