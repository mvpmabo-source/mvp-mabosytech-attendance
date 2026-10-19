#pragma once

#include <Arduino.h>

// Download and stage one firmware release from private Supabase Storage.
// The caller owns command acknowledgement and reboot sequencing.
bool performCloudFirmwareUpdate(
  const char* targetVersion,
  const char* firmwareUrl,
  const char* expectedSha256,
  size_t expectedSize
);
