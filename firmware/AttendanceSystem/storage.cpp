#include "app.h"

String prefsGetString(const char* key, const String &def) {
  String v = def;
  if (lockPrefs()) { v = prefs.getString(key, def); unlockPrefs(); }
  return v;
}
bool prefsPutString(const char* key, const String &val) {
  bool ok=false;
  if (lockPrefs()) {
    const size_t written = prefs.putString(key,val);
    const String verify = prefs.getString(key, String("__PREF_VERIFY_MISSING__"));
    // Empty strings legitimately report 0 bytes written, so verify the
    // persisted value instead of interpreting the return count as a boolean.
    ok = (verify == val) && (written > 0 || val.length() == 0);
    unlockPrefs();
  }
  return ok;
}
bool prefsGetBool(const char* key, bool def) {
  bool v=def; if (lockPrefs()) { v=prefs.getBool(key,def); unlockPrefs(); } return v;
}
bool prefsPutBool(const char* key, bool val) {
  bool ok=false;
  if (lockPrefs()) {
    const size_t written = prefs.putBool(key,val);
    const bool verify = prefs.getBool(key,!val);
    ok = (written > 0) && (verify == val);
    unlockPrefs();
  }
  return ok;
}
int prefsGetInt(const char* key, int def) {
  int v=def; if (lockPrefs()) { v=prefs.getInt(key,def); unlockPrefs(); } return v;
}
bool prefsPutInt(const char* key, int val) {
  bool ok=false;
  if (lockPrefs()) {
    const size_t written = prefs.putInt(key,val);
    const int verify = prefs.getInt(key, val ^ 0x5A5A5A5A);
    ok = (written > 0) && (verify == val);
    unlockPrefs();
  }
  return ok;
}
bool prefsRemove(const char* key) {
  bool ok=false;
  if (lockPrefs()) {
    const bool removed = prefs.remove(key);
    // Removing a key that is already absent is an idempotent success.
    ok = removed || !prefs.isKey(key);
    unlockPrefs();
  }
  return ok;
}
bool prefsClearAll() {
  bool ok=false; if (lockPrefs(1000)) { ok=prefs.clear(); unlockPrefs(); } return ok;
}

static String lessonJournalKey(uint8_t slot) {
  char key[8];
  snprintf(key, sizeof(key), "lj_%u", (unsigned)slot);
  return String(key);
}

int reserveLessonJournal(LessonAppend &entry) {
  if (!lockPrefs(1000)) return -1;
  int slotFound = -1;
  for (uint8_t slot = 0; slot < SD_LESSON_JOURNAL_SLOTS; ++slot) {
    const String key = lessonJournalKey(slot);
    if (prefs.isKey(key.c_str())) continue;

    LessonAppend persisted = entry;
    persisted.journalSlot = slot;
    const size_t written = prefs.putBytes(key.c_str(), &persisted, sizeof(persisted));
    if (written != sizeof(persisted)) continue;

    const size_t storedLen = prefs.getBytesLength(key.c_str());
    LessonAppend verify{};
    const size_t readBack = storedLen == sizeof(verify)
                              ? prefs.getBytes(key.c_str(), &verify, sizeof(verify))
                              : 0;
    if (readBack == sizeof(verify) && memcmp(&verify, &persisted, sizeof(verify)) == 0) {
      entry = persisted;
      slotFound = slot;
      break;
    }
    prefs.remove(key.c_str());
  }
  unlockPrefs();
  return slotFound;
}

bool loadLessonJournal(uint8_t slot, LessonAppend &entry) {
  if (slot >= SD_LESSON_JOURNAL_SLOTS) return false;
  if (!lockPrefs(500)) return false;
  const String key = lessonJournalKey(slot);
  const size_t len = prefs.getBytesLength(key.c_str());
  LessonAppend tmp{};
  const size_t got = (len == sizeof(tmp)) ? prefs.getBytes(key.c_str(), &tmp, sizeof(tmp)) : 0;
  const bool ok = got == sizeof(tmp) && tmp.journalSlot == slot;
  if (ok) entry = tmp;
  unlockPrefs();
  return ok;
}

bool clearLessonJournal(uint8_t slot) {
  if (slot >= SD_LESSON_JOURNAL_SLOTS) return false;
  if (!lockPrefs(500)) return false;
  const String key = lessonJournalKey(slot);
  const bool removed = prefs.remove(key.c_str());
  const bool ok = removed || !prefs.isKey(key.c_str());
  unlockPrefs();
  return ok;
}

// =====================================================
// FORMAT HELPERS
// =====================================================
String formatBytes(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1024ULL*1024ULL*1024ULL)      snprintf(buf,sizeof(buf),"%.2f GB", bytes/1073741824.0);
  else if (bytes >= 1024ULL*1024ULL)         snprintf(buf,sizeof(buf),"%.2f MB", bytes/1048576.0);
  else if (bytes >= 1024ULL)                 snprintf(buf,sizeof(buf),"%.2f KB", bytes/1024.0);
  else                                       snprintf(buf,sizeof(buf),"%llu B",(unsigned long long)bytes);
  return String(buf);
}

bool isValidDateStr(const String &s) {
  if (s.length()!=10) return false;
  for (int i=0;i<10;i++) {
    if (i==4||i==7) { if (s[i]!='-') return false; }
    else { if (!isDigit((unsigned char)s[i])) return false; }
  }
  return true;
}
String profileKey(uint16_t id, const char* field) { return String(id) + "_" + String(field); }

void saveProfile(uint16_t id, PersonProfile p) {
  prefsPutString(profileKey(id, "name").c_str(), p.name);
  prefsPutString(profileKey(id, "cid").c_str(), p.collegeId);
  prefsPutString(profileKey(id, "role").c_str(), p.role);
  prefsPutString(profileKey(id, "dept").c_str(), p.department);
  prefsPutString(profileKey(id, "note").c_str(), p.note);
}

PersonProfile loadProfile(uint16_t id) {
  PersonProfile p;
  p.name       = prefsGetString(profileKey(id, "name").c_str(), "");
  p.collegeId  = prefsGetString(profileKey(id, "cid").c_str(), "");
  p.role       = prefsGetString(profileKey(id, "role").c_str(), "");
  p.department = prefsGetString(profileKey(id, "dept").c_str(), "");
  p.note       = prefsGetString(profileKey(id, "note").c_str(), "");
  return p;
}

void deleteProfile(uint16_t id) {
  prefsRemove(profileKey(id, "name").c_str());
  prefsRemove(profileKey(id, "cid").c_str());
  prefsRemove(profileKey(id, "role").c_str());
  prefsRemove(profileKey(id, "dept").c_str());
  prefsRemove(profileKey(id, "note").c_str());
}

String presenceKey(uint16_t id) { return "p_" + String(id); }

void loadPresenceState() {
  for (uint16_t id = 1; id <= MAX_FINGERPRINTS; id++)
    insideState[id] = prefsGetBool(presenceKey(id).c_str(), false);
  Serial.println("Check In / Check Out state loaded.");
}

void serviceDeviceSetupPersistence() {
  static unsigned long retryAtMs = 0;
  if (!deviceSetupPersistPending) return;
  if ((long)(millis() - retryAtMs) < 0) return;

  int checkin = 0, checkout = 0, security = MATCH_SECURITY_LEVEL;
  String device, tz;
  if (!lockState(100)) return;
  checkin = cfgCheckinMinutes;
  checkout = cfgCheckoutMinutes;
  security = cfgSecurityLevel;
  device = cfgDeviceName;
  tz = cfgTimezone;
  unlockState();

  const String oldTz = prefsGetString(CFG_TZ_KEY, "");
  bool ok = false;
  if (lockPrefs(1000)) {
    const bool a = prefs.putInt(CFG_CHECKIN_KEY, checkin) > 0;
    const bool b = prefs.putInt(CFG_CHECKOUT_KEY, checkout) > 0;
    const bool c = prefs.putString(CFG_DEVNAME_KEY, device) > 0;
    const bool d = prefs.putString(CFG_TZ_KEY, tz) > 0;
    const bool e = prefs.putInt(CFG_SECLVL_KEY, security) > 0;
    ok = a && b && c && d && e;
    unlockPrefs();
  }

  // Apply timezone/SNTP configuration only from the Arduino loop, never from
  // an AsyncWebServer callback.
  if (tz != oldTz) {
    configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov", "time.google.com");
  }

  if (ok) {
    deviceSetupPersistPending = false;
    retryAtMs = 0;
    bootSnapshotDirty = true;
    addLog("INFO", -1, "Device setup updated from dashboard");
  } else {
    retryAtMs = millis() + 1000UL;
    Serial.println("[SETUP] Preferences write failed; update remains pending.");
  }
}

bool savePresenceStateValue(uint16_t id, bool value) {
  if (id < 1 || id > MAX_FINGERPRINTS) return false;
  const String key = presenceKey(id);
  if (!lockPrefs(500)) return false;
  const size_t written = prefs.putBool(key.c_str(), value);
  const bool verified = prefs.getBool(key.c_str(), !value) == value;
  unlockPrefs();
  return written > 0 && verified;
}

bool savePresenceState(uint16_t id) {
  if (id < 1 || id > MAX_FINGERPRINTS) return false;
  bool v = false;
  if (!lockState(100)) return false;
  v = insideState[id];
  unlockState();
  return savePresenceStateValue(id, v);
}

void clearPresenceState(uint16_t id) {
  if (id < 1 || id > MAX_FINGERPRINTS) return;
  prefsRemove(presenceKey(id).c_str());
}

bool clearAllTeacherProfilesAndPresence() {
  // Deliberately remove only teacher profile/presence and attendance journal
  // keys. Keep Wi-Fi/cloud credentials, device settings, login credentials,
  // and the durable restart-command guard intact.
  bool ok = false;
  if (!lockPrefs(3000)) return false;
  ok = true;
  char key[32];
  auto removeIfPresent = [&](const char* k) {
    if (prefs.isKey(k) && !prefs.remove(k) && prefs.isKey(k)) ok = false;
  };
  for (uint16_t id = 1; id <= MAX_FINGERPRINTS; ++id) {
    snprintf(key, sizeof(key), "%u_name", (unsigned)id); removeIfPresent(key);
    snprintf(key, sizeof(key), "%u_cid", (unsigned)id); removeIfPresent(key);
    snprintf(key, sizeof(key), "%u_role", (unsigned)id); removeIfPresent(key);
    snprintf(key, sizeof(key), "%u_dept", (unsigned)id); removeIfPresent(key);
    snprintf(key, sizeof(key), "%u_note", (unsigned)id); removeIfPresent(key);
    snprintf(key, sizeof(key), "p_%u", (unsigned)id); removeIfPresent(key);
  }
  for (uint8_t slot = 0; slot < SD_LESSON_JOURNAL_SLOTS; ++slot) {
    snprintf(key, sizeof(key), "lj_%u", (unsigned)slot);
    removeIfPresent(key);
  }
  unlockPrefs();

  if (!clearAttendanceJournal()) ok = false;
  return ok;
}

void loadDeviceSetup() {
  int checkin = prefsGetInt(CFG_CHECKIN_KEY, CHECKIN_ON_TIME_MINUTES);
  int checkout = prefsGetInt(CFG_CHECKOUT_KEY, CHECKOUT_ON_TIME_MINUTES);
  String name = prefsGetString(CFG_DEVNAME_KEY, "MBS BA MVP");
  String tz = prefsGetString(CFG_TZ_KEY, TIME_ZONE);
  int sec = prefsGetInt(CFG_SECLVL_KEY, MATCH_SECURITY_LEVEL);
  if (sec < 1 || sec > 5) sec = MATCH_SECURITY_LEVEL;
  if (lockState()) {
    cfgCheckinMinutes = checkin; cfgCheckoutMinutes = checkout;
    cfgDeviceName = name; cfgTimezone = tz; cfgSecurityLevel = sec;
    unlockState();
  }
  Serial.println("[SETUP] Device setup loaded from Preferences.");
}

int hhmmToMinutes(String hhmm) {
  hhmm.trim();
  int colonIdx = hhmm.indexOf(':');
  if (colonIdx < 1) return -1;
  String hh = hhmm.substring(0, colonIdx), mm = hhmm.substring(colonIdx + 1);
  if (hh.length() < 1 || hh.length() > 2) return -1;
  if (mm.length() != 2) return -1;
  for (unsigned int i = 0; i < hh.length(); i++) if (!isDigit(hh[i])) return -1;
  for (unsigned int i = 0; i < mm.length(); i++) if (!isDigit(mm[i])) return -1;
  int h = hh.toInt(), m = mm.toInt();
  if (h < 0 || h > 23) return -1;
  if (m < 0 || m > 59) return -1;
  return h * 60 + m;
}

String minutesToHHMM(int minutes) {
  if (minutes < 0) minutes = 0;
  minutes = minutes % 1440;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
  return String(buf);
}

// =====================================================
// CSV HELPERS
String csvEscape(String field) {
  bool needsQuotes = field.indexOf(',') >= 0 || field.indexOf('"') >= 0 ||
                      field.indexOf('\n') >= 0 || field.indexOf('\r') >= 0;
  if (field.indexOf('"') >= 0) field.replace("\"", "\"\"");
  if (needsQuotes) return "\"" + field + "\"";
  return field;
}

// =====================================================
// OLED
// =====================================================