MABOSYTECH - send-failure fix + diagnostics (2026-10-09)
Based ONLY on MVP_MABOSYTECH_FINAL_NATIVE_SD_FIX_2026-10-05.zip (source/ folder).

Changed files: source/native_http_compat.h, source/native_http_compat.cpp, source/dashboard.cpp
(also supplied as dashboard_send_fix.patch).

NOT INCLUDED: firmware binaries. I cannot build ESP32 firmware here, and the old .bin/.elf
would NOT contain these changes. Rebuild in Arduino IDE (esp32 core 3.3.12, board
"ESP32 Dev Module", 4MB flash, Partition Scheme min_spiffs, flash qio 80MHz, 240MHz CPU).

NOT TESTED on hardware. Not compiled with the ESP32 toolchain (syntax-checked
native_http_compat.cpp against host stubs only; dashboard.cpp not compiled).

Serial after login should show:
  [HTTP] send dashboard-gzip ok ... bytes=46377 ...
If it shows FAILED, send me that full line (errno, so_error, ms, largest).
Wi-Fi, auth, APIs, SD, fingerprint, OTA code are unchanged.