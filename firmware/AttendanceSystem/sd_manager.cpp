#include "app.h"


void setSdAvailableState(bool available, const char* reason) {
  // Centralize SD state transitions so the main/system buzzer gives a short,
  // periodic warning whenever SD is unavailable, including a cold boot with
  // no card. A successful mount silences the warning immediately.
  const bool wasAvailable = sdAvailable;

  sdAvailable = available;

  if (available) {
    sdHealthConsecutiveFailures = 0;
    return;
  }

  sdStatValid = false;
  sdTotalBytesCache = 0;
  sdUsedBytesCache = 0;
  sdCsvBytesCache = 0;
  sdLessonsBytesCache = 0;

  mainSdBuzzerWarningPending = true;
  if (wasAvailable) {
    Serial.print("[SD] WARNING: mounted SD card is now unavailable");
    if (reason && reason[0]) {
      Serial.print(" - ");
      Serial.print(reason);
    }
    Serial.println(". Main warning buzzer armed until SD mounts again.");
  }
}

// Buffered CSV reader used only by sdTask-owned file operations.
struct SdReader {
  File* f = nullptr;
  uint8_t buf[512];
  int len = 0;
  int pos = 0;

  bool fill() {
    if (!f) return false;
    len = (int)f->readBytes((char*)buf, sizeof(buf));
    pos = 0;
    return len > 0;
  }

  int read() {
    if (pos >= len && !fill()) return -1;
    return buf[pos++];
  }

  int peek() {
    if (pos >= len && !fill()) return -1;
    return buf[pos];
  }
};

static bool sdValidAttendanceTime(const String& t) {
  if(t.length()!=8 || t[2]!=':' || t[5]!=':') return false;
  for(int i=0;i<8;i++) if(i!=2 && i!=5 && !isDigit((unsigned char)t[i])) return false;
  int hh=t.substring(0,2).toInt();
  int mm=t.substring(3,5).toInt();
  int ss=t.substring(6,8).toInt();
  return hh>=0 && hh<=23 && mm>=0 && mm<=59 && ss>=0 && ss<=59;
}


static bool sdFileTailEquals(const char* path, const String& row) {
  if (!sdAvailable || !path || row.length()==0) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  if (sz < row.length()) { f.close(); return false; }
  if (!f.seek(sz - row.length())) { f.close(); return false; }
  String tail; tail.reserve(row.length());
  while (f.available()) tail += (char)f.read();
  f.close();
  return tail == row;
}


static bool sdFileContainsExactLine(const char* path, const String& row) {
  if (!sdAvailable || !path || row.length()==0) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() && line[line.length()-1]=='\r') line.remove(line.length()-1);
    if (line + "\n" == row) { f.close(); return true; }
  }
  f.close();
  return false;
}

static bool appendSdRowVerified(const char* path, const String& row, const char* label) {
  if (!sdAvailable || !path || row.length()==0) return false;

  // Idempotent retry: if the exact line is already the final complete line,
  // the data commit succeeded even if the previous verification path failed.
  if (sdFileTailEquals(path, row)) {
    Serial.printf("[%s] row already present at EOF; treating retry as committed.\n", label ? label : "SD");
    return true;
  }

  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;

  size_t sz = f.size();

  // If a file was recreated after a remount, create its correct header before
  // the first data row. Existing non-empty files are never rewritten.
  if (sz == 0) {
    const char* header = nullptr;
    if (strcmp(path, SD_LESSONS_FILE) == 0) header = LESSONS_CSV_HEADER;
    else if (strcmp(path, SD_SYSLOG_FILE) == 0) header = SYSTEM_LOG_HEADER;
    if (header) {
      const size_t hlen = strlen(header);
      if (f.write((const uint8_t*)header, hlen) != hlen ||
          f.write((const uint8_t*)"\n", 1) != 1) {
        f.close();
        return false;
      }
      sz = hlen + 1;
    }
  }

  // Preserve any existing partial final fragment, but separate it from the
  // new row so two records can never become one concatenated CSV record.
  if (sz > 0) {
    if (!f.seek(sz - 1)) { f.close(); return false; }
    int last = f.read();
    if (last != '\n') {
      if (!f.seek(sz)) { f.close(); return false; }
      if (f.write((const uint8_t*)"\n", 1) != 1) { f.close(); return false; }
      sz++;
    }
  }

  if (!f.seek(sz)) { f.close(); return false; }
  const size_t written = f.write((const uint8_t*)row.c_str(), row.length());
  f.flush();
  f.close();
  if (written != row.length()) return false;

  // Read-back verification makes the caller trust only data that is actually
  // present at the end of the file. A retry remains idempotent.
  if (sdFileTailEquals(path, row)) return true;
  vTaskDelay(1);
  return sdFileTailEquals(path, row);
}

static SdReader g_sdReader;

// Single-item retry slots are owned exclusively by sdTask. They prevent a
// failed non-attendance write from being lost because xQueueSendToFront()
// races with another producer filling the queue.
static LessonAppend g_lessonRetry{};
static bool g_lessonRetryActive = false;
static volatile uint8_t g_lessonJournalQueuedMask = 0;
static uint8_t g_lessonJournalClearRetryMask = 0;
static portMUX_TYPE g_lessonJournalMux = portMUX_INITIALIZER_UNLOCKED;
static SystemLogEvent g_systemLogRetry{};
static bool g_systemLogRetryActive = false;

void sdReaderAttach(File &f) {
  g_sdReader.f = &f;
  g_sdReader.len = 0;
  g_sdReader.pos = 0;
}

bool readCsvRecord(File &f, String outFields[], int maxFields) {
  if (g_sdReader.f != &f) sdReaderAttach(f);

  for (int i = 0; i < maxFields; i++) outFields[i] = "";
  int fieldIndex = 0;
  String current;
  current.reserve(64);
  bool inQuotes = false;
  bool haveData = false;

  for (;;) {
    int ci = g_sdReader.read();
    if (ci < 0) break;
    char c = (char)ci;
    haveData = true;

    if (inQuotes) {
      if (c == '"') {
        if (g_sdReader.peek() == '"') {
          current += '"';
          g_sdReader.read();
        } else {
          inQuotes = false;
        }
      } else {
        current += c;
      }
    } else {
      if (c == '"') {
        inQuotes = true;
      } else if (c == ',') {
        if (fieldIndex < maxFields) outFields[fieldIndex] = current;
        fieldIndex++;
        current = "";
        current.reserve(64);
      } else if (c == '\n') {
        if (fieldIndex < maxFields) outFields[fieldIndex] = current;
        return true;
      } else if (c == '\r') {
        // Ignore CR; LF terminates the record.
      } else {
        current += c;
      }
    }
  }

  if (haveData) {
    if (fieldIndex < maxFields) outFields[fieldIndex] = current;
    return true;
  }
  return false;
}

static bool sdAttendanceRowAtEnd(const String& row) {
  if(!sdAvailable || row.length()==0) return false;
  File f=SD.open(SD_LOG_FILE,FILE_READ);
  if(!f) return false;
  size_t sz=f.size();
  if(sz<row.length()) { f.close(); return false; }
  if(!f.seek(sz-row.length())) { f.close(); return false; }
  String tail; tail.reserve(row.length());
  while(f.available()) tail+=(char)f.read();
  f.close();
  return tail==row;
}

static bool pendingContainsSeq(uint32_t seq) {
  if(seq==0) return false;
  for(int i=0;i<pendingUsed;i++) {
    int idx=(pendingHead+i)%MAX_PENDING;
    if(pendingRows[idx].seq==seq) return true;
  }
  return false;
}

static bool appendAttendanceRowVerified(const String& row,bool recovery,uint32_t seq) {
  if(!sdAvailable || row.length()==0) return false;
  if(sdAttendanceRowAtEnd(row)) return true;
  digitalWrite(SD_CS,HIGH); delayMicroseconds(5);
  File f=SD.open(SD_LOG_FILE,"r+");
  if(!f) { Serial.println("[SD] attendance.csv r+ open failed"); return false; }

  uint32_t fileSize=(uint32_t)f.size();
  uint32_t base=fileSize;
  if(fileSize==0) {
    if(!f.seek(0)) { f.close(); return false; }
    if(f.print(ATTENDANCE_CSV_HEADER)==0 || f.print("\n")==0) { f.close(); return false; }
    f.flush();
    base=(uint32_t)f.position();
  } else {
    if(!f.seek(fileSize-1)) { f.close(); return false; }
    int last=f.read();
    if(last=='\n') {
      base=fileSize;
    } else {
      // Never overwrite an existing final fragment during recovery. Whether it
      // is a valid row without LF or a torn/corrupt fragment, preserving all
      // existing bytes is safer than trying to repair in place. The validator
      // will ignore a malformed fragment, and the journal supplies the complete
      // authoritative row immediately after it.
      if(!f.seek(fileSize)) { f.close(); return false; }
      if(f.write((const uint8_t*)"\n",1)!=1) { f.close(); return false; }
      base=fileSize+1;
      if(recovery) Serial.println("[SD] Recovery: preserved existing final fragment; adding separator before verified journal row.");
    }
  }

  if(!f.seek(base)) { f.close(); return false; }
  size_t n=f.write((const uint8_t*)row.c_str(),row.length());
  if(n!=row.length()) {
    if(!f.seek(base)) { f.close(); return false; }
    n=f.write((const uint8_t*)row.c_str(),row.length());
  }
  f.flush();
  f.close();
  if(n!=row.length()) return false;
  bool ok=sdAttendanceRowAtEnd(row);
  Serial.printf("[SD] attendance verify seq=%lu result=%s\n",(unsigned long)seq,ok?"OK":"FAIL");
  return ok;
}

void serviceRecoveredAttendanceJournal() {
  if(!sdAvailable || !attendanceJournalActive) return;
  String row;
  if(!readAttendanceJournalRow(row)) {
    Serial.println("[SD] ERROR: attendance journal unreadable; blocking new attendance.");
    return;
  }
  if(sdAttendanceRowAtEnd(row)) {
    finalizeAttendanceCommit(attendanceJournalSeq,row,true);
    return;
  }
  if(pendingContainsSeq(attendanceJournalSeq)) return;
  if(pendingUsed>=MAX_PENDING) return;
  pendingRows[pendingTail].csv=row;
  pendingRows[pendingTail].seq=attendanceJournalSeq;
  pendingRows[pendingTail].recovery=attendanceJournalRecovery;
  pendingTail=(pendingTail+1)%MAX_PENDING;
  pendingUsed++;
  Serial.print("[SD] Journal row staged for verified recovery seq="); Serial.println(attendanceJournalSeq);
}

static bool finishBootPreparation() {
  if (otaInProgress() || !sdAvailable) return false;

  migrateStorageIfNeeded();

  if (!SD.exists(SD_LOG_FILE)) {
    if (SD.exists(SD_LEGACY_LOG_FILE)) {
      // Never create an empty live attendance file while the immutable legacy
      // source still exists. A later retry will rebuild it without hiding data.
      Serial.println("[SD] Boot preparation deferred: legacy attendance source still needs migration.");
      return false;
    }

    Serial.println("[SD] Live attendance.csv missing on a fresh card; creating verified empty file.");
    File w = SD.open(SD_LOG_FILE, FILE_APPEND);
    bool created = false;
    if (w) {
      const size_t hlen = strlen(ATTENDANCE_CSV_HEADER);
      created = (w.write((const uint8_t*)ATTENDANCE_CSV_HEADER, hlen) == hlen) &&
                (w.write((const uint8_t*)"\n", 1) == 1);
      w.flush();
      w.close();
    }
    if (!created) {
      setSdAvailableState(false, "attendance.csv creation failed during boot");
      return false;
    }
  }

  serviceRecoveredAttendanceJournal();
  while (sdAvailable && (attendanceJournalActive || pendingUsed > 0 || uxQueueMessagesWaiting(attendanceQueue) > 0)) {
    if (otaInProgress()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    serviceRecoveredAttendanceJournal();
    serviceSdQueueTask();
    vTaskDelay(5);
  }

  scanSdHistoryOnBootTask();
  dashboardBootReady = true;
  attendanceBootReady = true;
  bootSnapshotDirty = true;
  refreshBootSnapshot();
  Serial.println("[SD] Boot initialization complete; dashboard bootstrap is ready.");
  return true;
}

void sdTaskFunc(void *pv) {
  (void)pv;
  lastSdRemountAttempt = millis();
  static unsigned long lastStackReport = 0;
  static unsigned long nextBootPreparationRetryMs = 0;
  static unsigned long lastBootPreparationLogMs = 0;
  static unsigned long lastLessonJournalRecoveryMs = 0;

  while (otaInProgress()) vTaskDelay(pdMS_TO_TICKS(20));

  // Initial mount is completed in setup() before AsyncWebServer allocation.
  // From this point forward sdTask is the sole filesystem owner.
  Serial.println("[SD] Background boot initialization starting...");
  dashboardBootReady = false;
  attendanceBootReady = false;
  if (!finishBootPreparation()) {
    // The dashboard itself does not require an SD card. Publish the normal
    // bootstrap snapshot immediately when the card is absent, while keeping
    // attendance/fingerprint processing gated until authoritative SD history
    // has eventually been loaded.
    if (!sdAvailable) {
      dashboardBootReady = true;
      bootSnapshotDirty = true;
      refreshBootSnapshot();
      Serial.println("[SD] No SD at boot: dashboard bootstrap released in offline mode; attendance gate remains closed.");
    }
    lastBootPreparationLogMs = millis();
    Serial.println("[SD] Boot initialization is waiting for a healthy, migrated attendance store.");
  }

  for (;;) {
    if (otaInProgress()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Shared ownership gate: cloud outbox access must not overlap any
    // filesystem work performed by this single-owner SD task.
    if (!lockCloudSd(1000)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (!attendanceBootReady && sdAvailable) {
      const unsigned long nowBoot = millis();
      if ((long)(nowBoot - nextBootPreparationRetryMs) >= 0) {
        nextBootPreparationRetryMs = nowBoot + 2000UL;
        if (!finishBootPreparation()) {
          if (nowBoot - lastBootPreparationLogMs >= 10000UL) {
            lastBootPreparationLogMs = nowBoot;
            Serial.println("[SD] Boot preparation still pending.");
          }
        }
      }
    }

    // Fast path: move any queued attendance row into the SD-owned staging ring.
    // Nothing is discarded when the ring is full.
    AttendanceEvent evt;
    while(pendingUsed<MAX_PENDING && xQueueReceive(attendanceQueue,&evt,0)==pdTRUE){
      if(evt.seq!=0 && !pendingContainsSeq(evt.seq)){
        pendingRows[pendingTail].csv=String(evt.row);
        pendingRows[pendingTail].seq=evt.seq;
        pendingRows[pendingTail].recovery=false;
        pendingTail=(pendingTail+1)%MAX_PENDING;
        pendingUsed++;
      }
    }

    // The journal is the fallback signal if the queue handoff failed, and also
    // retries journal cleanup after a successful SD write.
    serviceRecoveredAttendanceJournal();

    // Durable attendance gets first access to the SD before any HTTP read.
    serviceSdQueueTask();
    const unsigned long nowLessonRecovery = millis();
    if (nowLessonRecovery - lastLessonJournalRecoveryMs >= 1000UL) {
      lastLessonJournalRecoveryMs = nowLessonRecovery;
      serviceLessonJournalRecoveryTask();
    }
    serviceLessonQueueTask();
    serviceSystemLogQueueTask();

    // HTTP never touches SD directly; every SD request is serialized here.
    SdBulkRequest req;
    while(xQueueReceive(sdRequestQueue,&req,0)==pdTRUE){
      serviceSdBulkRequest(req);
      if(pendingUsed>0 || uxQueueMessagesWaiting(attendanceQueue)>0) break;
    }

    serviceBackgroundStatsTask();

    // Boot history is scanned exactly once before dashboardBootReady becomes
    // true. There is no second background scan here, so the browser cannot see
    // a moving target while the boot snapshot is being assembled.

    unlockCloudSd();
    vTaskDelay(pdMS_TO_TICKS(5));
    unsigned long now=millis();
    if(now-lastStackReport>=10000){lastStackReport=now;Serial.printf("[STACK] sdTask HWM: %u bytes free\n",(unsigned)uxTaskGetStackHighWaterMark(NULL));}
  }
}

void markLogSaved(uint32_t seq) {
  if (seq == 0) return;
  if (!lockState(100)) return;
  for (int i = 0; i < logCount; i++) {
    int idx = (logWriteIndex - logCount + i + MAX_LOGS) % MAX_LOGS;
    if (logs[idx].seq == seq) { logs[idx].savedToSd = true; break; }
  }
  unlockState();
}

// =====================================================
// LESSON QUEUE
// =====================================================
static void setLessonJournalQueued(uint8_t slot, bool queued) {
  if (slot >= SD_LESSON_JOURNAL_SLOTS) return;
  portENTER_CRITICAL(&g_lessonJournalMux);
  if (queued) g_lessonJournalQueuedMask |= (uint8_t)(1U << slot);
  else       g_lessonJournalQueuedMask &= (uint8_t)~(1U << slot);
  portEXIT_CRITICAL(&g_lessonJournalMux);
}

static bool isLessonJournalQueued(uint8_t slot) {
  if (slot >= SD_LESSON_JOURNAL_SLOTS) return false;
  bool queued=false;
  portENTER_CRITICAL(&g_lessonJournalMux);
  queued = (g_lessonJournalQueuedMask & (uint8_t)(1U << slot)) != 0;
  portEXIT_CRITICAL(&g_lessonJournalMux);
  return queued;
}

void serviceLessonJournalRecoveryTask() {
  if (!sdAvailable || !lessonAppendQueue) return;

  // Retry journal cleanup first. No SD write is performed here: the row was
  // already confirmed on the card; we only need to remove the NVS safety copy.
  for (uint8_t slot = 0; slot < SD_LESSON_JOURNAL_SLOTS; ++slot) {
    if ((g_lessonJournalClearRetryMask & (uint8_t)(1U << slot)) == 0) continue;
    if (clearLessonJournal(slot)) {
      g_lessonJournalClearRetryMask &= (uint8_t)~(1U << slot);
      setLessonJournalQueued(slot, false);
    }
  }

  // Recover durable journal records that were persisted before the browser
  // queue accepted them, or that survived a reboot while SD was offline.
  for (uint8_t slot = 0; slot < SD_LESSON_JOURNAL_SLOTS; ++slot) {
    if (isLessonJournalQueued(slot)) continue;
    if (g_lessonRetryActive && g_lessonRetry.journalSlot == slot) continue;

    LessonAppend recovered{};
    if (!loadLessonJournal(slot, recovered)) continue;
    if (xQueueSend(lessonAppendQueue, &recovered, 0) == pdTRUE) {
      setLessonJournalQueued(slot, true);
    } else {
      break;
    }
  }
}

void serviceLessonQueueTask() {
  if (!sdAvailable) return;

  LessonAppend la{};
  const bool fromRetry = g_lessonRetryActive;
  if (fromRetry) la = g_lessonRetry;
  else if (xQueueReceive(lessonAppendQueue, &la, 0) != pdTRUE) return;

  String row; row.reserve(500);
  row += csvEscape(String(la.date));          row += ",";
  row += csvEscape(String(la.time));          row += ",";
  row += String(la.fingerprintId);            row += ",";
  row += csvEscape(String(la.name));          row += ",";
  row += csvEscape(String(la.collegeId));     row += ",";
  row += csvEscape(String(la.role));          row += ",";
  row += csvEscape(String(la.department));    row += ",";
  row += csvEscape(String(la.type));          row += ",";
  row += csvEscape(String(la.lesson));        row += "\n";

  digitalWrite(SD_CS, HIGH); delayMicroseconds(5);
  // A power loss can leave the journal behind even after the SD append really
  // succeeded. Search the lesson file before retrying so an already-committed
  // row can never be duplicated merely because NVS cleanup was delayed.
  const bool alreadyPresent = sdFileContainsExactLine(SD_LESSONS_FILE, row);
  const bool ok = alreadyPresent || appendSdRowVerified(SD_LESSONS_FILE, row, "LESSON");
  if (ok) {
    g_lessonRetryActive = false;
    if (la.journalSlot < SD_LESSON_JOURNAL_SLOTS) {
      if (!clearLessonJournal(la.journalSlot)) {
        g_lessonJournalClearRetryMask |= (uint8_t)(1U << la.journalSlot);
      } else {
        g_lessonJournalClearRetryMask &= (uint8_t)~(1U << la.journalSlot);
        setLessonJournalQueued(la.journalSlot, false);
      }
    }
    if (!alreadyPresent) sdLessonCount++;
    sdLessonsBytesCache = 0;
    sdStatValid = false;
    Serial.print("[LESSON] durable save id="); Serial.print(la.fingerprintId);
    Serial.print(" type="); Serial.println(la.type);
  } else {
    if (!fromRetry) {
      g_lessonRetry = la;
      g_lessonRetryActive = true;
    }
    Serial.println("[LESSON] durable append/verify failed - retained in sdTask retry slot.");
    setSdAvailableState(false, "SD lesson write failed");
    sdStatValid = false;
  }
}

void serviceSystemLogQueueTask() {
  if (!sdAvailable) return;

  SystemLogEvent evt{};
  const bool fromRetry = g_systemLogRetryActive;
  if (fromRetry) {
    evt = g_systemLogRetry;
  } else {
    bool fromFallback = false;
    if (lockState(20)) {
      if (systemLogFallbackCount > 0) {
        evt = systemLogFallback[0];
        for (uint8_t i = 1; i < systemLogFallbackCount; ++i) systemLogFallback[i-1] = systemLogFallback[i];
        --systemLogFallbackCount;
        fromFallback = true;
      }
      unlockState();
    }
    if (!fromFallback && xQueueReceive(systemLogQueue, &evt, 0) != pdTRUE) return;
  }

  digitalWrite(SD_CS, HIGH); delayMicroseconds(5);
  String row(evt.row);
  const bool ok = appendSdRowVerified(SD_SYSLOG_FILE, row, "SYSLOG");
  if (ok) {
    g_systemLogRetryActive = false;
    markLogSaved(evt.seq);
  } else {
    if (!fromRetry) {
      g_systemLogRetry = evt;
      g_systemLogRetryActive = true;
    }
    Serial.println("[SYSLOG] durable append/verify failed - retained in sdTask retry slot.");
    setSdAvailableState(false, "SD system log write failed");
    sdStatValid = false;
  }
}

// =====================================================
// SD WRITER
// =====================================================
void serviceSdQueueTask() {
  unsigned long now=millis();
  if(sdAvailable && pendingUsed>0) {
    if(sdLastOpenFailMs!=0 && (now-sdLastOpenFailMs)<SD_OPEN_RETRY_DELAY_MS) return;
    lastSdWriteAttempt=now;

    String row=pendingRows[pendingHead].csv;
    uint32_t seq=pendingRows[pendingHead].seq;
    bool recovery=pendingRows[pendingHead].recovery;

    bool alreadyCommitted = sdAttendanceRowAtEnd(row);
    bool verified = appendAttendanceRowVerified(row,recovery,seq);

    if(verified) {
      sdLastOpenFailMs=0;
      pendingRows[pendingHead].csv="";
      pendingRows[pendingHead].seq=0;
      pendingRows[pendingHead].recovery=false;
      pendingHead=(pendingHead+1)%MAX_PENDING;
      pendingUsed--;
      if(!alreadyCommitted) sdRecordCount++;
      sdWriteConsecutiveFailures=0;
      sdConsecutiveFailures=0;
      sdStatValid=false;
      finalizeAttendanceCommit(seq,row,recovery);
      return;
    }

    sdLastOpenFailMs=now;
    sdWriteConsecutiveFailures++;
    Serial.printf("[SD] attendance commit pending seq=%lu write-failure=%u\n",(unsigned long)seq,(unsigned)sdWriteConsecutiveFailures);
    if(sdWriteConsecutiveFailures>=SD_FAILURE_THRESHOLD) setSdAvailableState(false, "attendance write failure threshold reached");
    return;
  }

  if(!sdAvailable) {
    if(now-lastSdRemountAttempt<sdBackoffMs) return;
    lastSdRemountAttempt=now;
    bool ok=tryQuickSdMount();
    if(ok) sdBackoffMs=5000;
    else { sdBackoffMs*=2; if(sdBackoffMs>SD_BACKOFF_MAX_MS) sdBackoffMs=SD_BACKOFF_MAX_MS; }
  }
}

void serviceBackgroundStatsTask() {
  static unsigned long lastStat = 0;
  static unsigned long lastHealth = 0;
  unsigned long now = millis();

  if (!sdAvailable) { sdStatValid = false; return; }

  // ---- Runtime card health check ---------------------------------
  if (now - lastHealth < SD_HEALTHCHECK_INTERVAL_MS) return;
  lastHealth = now;
  // IMPORTANT: do NOT send raw CMD0 (GO_IDLE_STATE) to a card that is
  // already mounted. CMD0 resets the card back to idle state and can
  // make a perfectly good mounted filesystem appear to have vanished.
  //
  // Instead, probe through the mounted filesystem itself. This is a
  // non-destructive check: open a file, read its size, and close it.
  // Five consecutive failures are treated as a real offline event;
  // the normal remount back-off then reacquires the card.
  bool healthy = false;
  File probe = SD.open(SD_LOG_FILE, FILE_READ);
  if (probe) {
    (void)probe.size();
    probe.close();
    healthy = true;
  }

  if (!healthy) {
    sdHealthConsecutiveFailures++;
    Serial.printf("[SD] health probe failed %u/%u\n", (unsigned)sdHealthConsecutiveFailures, (unsigned)SD_HEALTH_FAILURE_THRESHOLD);
    if (sdHealthConsecutiveFailures >= SD_HEALTH_FAILURE_THRESHOLD) {
      setSdAvailableState(false, "runtime health check failed");
      sdStatValid = false;
      sdGeneration++;
      refreshBootSnapshot();
      Serial.println("[SD] Card health check failed - going offline (will retry).");
    }
    return;
  }
  sdHealthConsecutiveFailures = 0;
  // ---- end card health check -------------------------------------

  if (now - lastStat < SD_STAT_TTL_MS) return;
  lastStat = now;

  digitalWrite(SD_CS, HIGH); delayMicroseconds(5);
  sdTotalBytesCache = SD.cardSize();
  File sf = SD.open(SD_LOG_FILE, FILE_READ); if (sf) { sdCsvBytesCache = sf.size(); sf.close(); }
  File lf = SD.open(SD_LESSONS_FILE, FILE_READ); if (lf) { sdLessonsBytesCache = lf.size(); lf.close(); }
  if (now - sdUsedBytesAtMs >= SD_USEDBYTES_TTL_MS) { sdUsedBytesAtMs = now; sdUsedBytesCache = SD.usedBytes(); }
  sdStatAtMs = now; sdStatValid = true;
}

// =====================================================
// SD MOUNT
// ---------------------------------------------------------------------
// Root causes fixed in this revision:
//
//  1. The previous version called SD.end() before the very first
//     SD.begin(), even though SD was never successfully mounted.
//     On the ESP32 SD library that leaves the driver in an undefined
//     state and every subsequent SD.begin() fails regardless of clock.
//     We now track whether SD was ever mounted, and only call SD.end()
//     when it actually was.
//
//  2. The file header promised "full SPI bus reset between mount
//     attempts" but the code never did it - it kept the same SPI
//     object alive. We now do SPI.end() + pinMode + SPI.begin() with
//     our pin mapping between every attempt, plus 74 clocks with CS
//     high to put the card back into SPI mode.
//
//  3. Only 1 attempt per clock, only 3 clocks. Now 2 attempts per
//     clock, 5 clocks (400k..20M). A marginal card gets a second try
//     after a genuinely clean bus reset.
//
//  4. Longer settle delays (cold power-up on some modules needs it).
// ---------------------------------------------------------------------
bool tryQuickSdMount() {
  static bool sdEverMounted = false;

  const uint32_t sdClocks[] = { 400000UL, 1000000UL, 4000000UL, 10000000UL, 20000000UL };
  const uint8_t  numClocks  = sizeof(sdClocks) / sizeof(sdClocks[0]);
  const uint8_t  attemptsPerClock = 2;

  for (uint8_t speedIndex = 0; speedIndex < numClocks; speedIndex++) {
    const uint32_t hz = sdClocks[speedIndex];

    for (uint8_t attempt = 1; attempt <= attemptsPerClock; attempt++) {

      // ---- Only tear down SD if it was actually mounted before ----
      if (sdEverMounted) {
        SD.end();
        sdEverMounted = false;
        delay(50);
      }

      // ---- Full SPI bus reset: the piece the old comment promised ----
      SPI.end();
      delay(30);

      pinMode(SD_CS, OUTPUT);
      digitalWrite(SD_CS, HIGH);
      delay(80);

      SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
      delay(30);

      // Send >= 74 clocks with CS high so the card re-enters SPI mode
      SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
      digitalWrite(SD_CS, HIGH);
      for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
      SPI.endTransaction();
      digitalWrite(SD_CS, HIGH);
      delay(20);

      Serial.printf("[SD] Mount try: %lu Hz, attempt %u/%u (CS=%d SCK=%d MISO=%d MOSI=%d)\n",
                    (unsigned long)hz, (unsigned)attempt, (unsigned)attemptsPerClock,
                    SD_CS, SD_SCLK, SD_MISO, SD_MOSI);

      if (!SD.begin(SD_CS, SPI, hz)) {
        Serial.println("[SD] SD.begin FAILED");
        delay(300);
        continue;
      }

      sdEverMounted = true;

      uint8_t ctype = SD.cardType();
      Serial.printf("[SD] SD.begin OK, cardType=%u\n", (unsigned)ctype);
      if (ctype == CARD_NONE) {
        Serial.println("[SD] Card type is NONE after begin");
        delay(300);
        continue;
      }

      // Verify the mounted FAT filesystem using the app's real file
      File f = SD.open(SD_LOG_FILE, FILE_READ);
      if (!f) {
        Serial.println("[SD] attendance.csv not openable; trying to create it...");
        File w = SD.open(SD_LOG_FILE, FILE_APPEND);
        if (w) {
          if (w.size() == 0) {
            w.println(ATTENDANCE_CSV_HEADER);
            w.flush();
          }
          w.close();
          f = SD.open(SD_LOG_FILE, FILE_READ);
        }
      }
      if (!f) {
        Serial.println("[SD] FAT filesystem mounted badly: attendance.csv unavailable");
        delay(300);
        continue;
      }

      uint32_t fileSize = (uint32_t)f.size();
      f.close();

      File lf = SD.open(SD_LESSONS_FILE, FILE_READ);
      if (!lf) {
        File w = SD.open(SD_LESSONS_FILE, FILE_APPEND);
        if (w) {
          if (w.size() == 0) {
            w.println(LESSONS_CSV_HEADER);
            w.flush();
          }
          w.close();
        }
      } else {
        lf.close();
      }

      File slf = SD.open(SD_SYSLOG_FILE, FILE_READ);
      if (!slf) {
        File w = SD.open(SD_SYSLOG_FILE, FILE_APPEND);
        if (w) {
          if (w.size() == 0) {
            w.println(SYSTEM_LOG_HEADER);
            w.flush();
          }
          w.close();
        }
      } else {
        slf.close();
      }

      setSdAvailableState(true);
      sdConsecutiveFailures = 0;
      sdWriteConsecutiveFailures = 0;
      sdHealthConsecutiveFailures = 0;
      sdBackoffMs = 5000;
      sdGeneration++;
      refreshBootSnapshot();
      sdMountedClockHz = hz;
      Serial.printf("[SD] Mounted successfully at %lu Hz. attendance.csv=%lu bytes.\n",
                    (unsigned long)hz, (unsigned long)fileSize);
      return true;
    }
  }

  setSdAvailableState(false, "all mount attempts failed");
  sdConsecutiveFailures++;
  Serial.println("[SD] All mount attempts failed.");
  Serial.println("[SD] Hardware checklist:");
  Serial.println("[SD]   1. microSD formatted FAT32 (not exFAT), <=32 GB");
  Serial.println("[SD]   2. 3V3 + GND firmly connected to the SD module");
  Serial.println("[SD]   3. CS=GPIO4  SCK=GPIO23  MISO=GPIO19  MOSI=GPIO18");
  Serial.println("[SD]   4. Try a different microSD card (counterfeits are common)");
  return false;
}

void initializeSD() {
  Serial.println("[SD] Initializing SD...");
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  setSdAvailableState(false, "initializing SD");
  sdConsecutiveFailures = 0;
  sdWriteConsecutiveFailures = 0;
  sdHealthConsecutiveFailures = 0;
  sdBackoffMs = 5000;

  // Give the module and card time to settle after power-up.
  // Was 300 ms; some SD breakout modules need longer on cold boot.
  delay(600);

  // Force a genuinely clean SPI bus before the first mount attempt.
  // The old version inherited whatever state setup() left behind.
  SPI.end();
  delay(30);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  delay(30);
  digitalWrite(SD_CS, HIGH);

  // 74+ clocks with CS high so the card is in SPI mode before
  // SD.begin() runs its own init sequence.
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  digitalWrite(SD_CS, HIGH);
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
  SPI.endTransaction();
  digitalWrite(SD_CS, HIGH);
  delay(30);

  if (!tryQuickSdMount()) {
    Serial.println("[SD] SD card could NOT be mounted. RAM-only.");
    return;
  }

  sdTotalBytesCache = SD.cardSize();
  sdUsedBytesCache  = SD.usedBytes();
  sdUsedBytesAtMs = millis();
  File sf = SD.open(SD_LOG_FILE, FILE_READ);
  if (sf) { sdCsvBytesCache = sf.size(); sf.close(); }
  File lf = SD.open(SD_LESSONS_FILE, FILE_READ);
  if (lf) { sdLessonsBytesCache = lf.size(); lf.close(); }
  sdStatAtMs = millis();
  sdStatValid = true;
}

bool ensureLessonsCsv() {
  if (!sdAvailable) return false;
  File f = SD.open(SD_LESSONS_FILE, FILE_READ);
  if (f) {
    bool empty = (f.size() == 0);
    f.close();
    if (!empty) return true;
  }
  File w = SD.open(SD_LESSONS_FILE, FILE_APPEND);
  if (!w) return false;
  const size_t hlen = strlen(LESSONS_CSV_HEADER);
  bool ok = (w.write((const uint8_t*)LESSONS_CSV_HEADER, hlen) == hlen) &&
            (w.write((const uint8_t*)"\n", 1) == 1);
  w.flush(); w.close();
  return ok;
}

bool ensureSystemLog() {
  if (!sdAvailable) return false;
  File f = SD.open(SD_SYSLOG_FILE, FILE_READ);
  if (f) {
    bool empty = (f.size() == 0);
    f.close();
    if (!empty) return true;
  }
  File w = SD.open(SD_SYSLOG_FILE, FILE_APPEND);
  if (!w) return false;
  const size_t hlen = strlen(SYSTEM_LOG_HEADER);
  bool ok = (w.write((const uint8_t*)SYSTEM_LOG_HEADER, hlen) == hlen) &&
            (w.write((const uint8_t*)"\n", 1) == 1);
  w.flush(); w.close();
  return ok;
}

// =====================================================
// LESSONS.CSV HEADER MIGRATION (section 13)
// -----------------------------------------------------------------
// Existing lesson rows are data that must not be rewritten just to change
// labels. The current reader is positional, so an old header is accepted
// without any destructive migration.
// =====================================================
void migrateLessonsHeaderIfNeeded() {
  // Data columns are positional and the current reader accepts both the old
  // and new header names. Do not rewrite an existing lessons.csv header in
  // place: header migration is not worth any risk to a user's reason data.
  if (!sdAvailable) return;
  File f = SD.open(SD_LESSONS_FILE, FILE_READ);
  if (!f) return;
  String header = f.readStringUntil('\n');
  header.trim();
  f.close();
  if (header.length() && header != LESSONS_CSV_HEADER)
    Serial.println("[MIGRATE] Keeping existing lessons.csv header; data remains untouched.");
}

// =====================================================
// ATTENDANCE.CSV STORAGE MIGRATION (sections 11 & 12)
// -----------------------------------------------------------------
// Runs once, on sdTask, before the first boot read. Gated by a
// Preferences schema-version flag (never prefs.clear()) so it never
// repeats once it has succeeded. Safe to re-enter after a mid-
// migration crash/power-loss: /attendance_legacy.csv is the source of
// truth once created and is never modified again, so a retry just
// re-derives attendance.csv/system.log from it.
// =====================================================
void migrateStorageIfNeeded() {
  if (!sdAvailable) return;

  const int ver = prefsGetInt(STORAGE_SCHEMA_KEY, 1);
  if (ver >= STORAGE_SCHEMA_CURRENT) return;

  // Find the immutable legacy source. If the current attendance.csv is already
  // in the new schema and no legacy backup exists, there is nothing left to
  // migrate. Never treat a partially rebuilt file as authoritative when the
  // legacy source is still present.
  bool legacyExists = SD.exists(SD_LEGACY_LOG_FILE);
  if (!legacyExists) {
    File current = SD.open(SD_LOG_FILE, FILE_READ);
    if (current) {
      String header = current.readStringUntil('\n');
      header.trim();
      current.close();
      if (header == ATTENDANCE_CSV_HEADER) {
        migrateLessonsHeaderIfNeeded();
        if (prefsPutInt(STORAGE_SCHEMA_KEY, STORAGE_SCHEMA_CURRENT))
          Serial.println("[MIGRATE] Attendance schema already current; marked complete.");
        return;
      }
    }

    if (!SD.exists(SD_LOG_FILE)) {
      if (prefsPutInt(STORAGE_SCHEMA_KEY, STORAGE_SCHEMA_CURRENT))
        Serial.println("[MIGRATE] Fresh card with no attendance data; schema marked complete.");
      return;
    }

    digitalWrite(SD_CS, HIGH); delayMicroseconds(5);
    if (!SD.rename(SD_LOG_FILE, SD_LEGACY_LOG_FILE)) {
      Serial.println("[MIGRATE] Could not create immutable attendance backup; migration deferred.");
      return;
    }
    legacyExists = true;
    Serial.println("[MIGRATE] Backed up legacy attendance source to /attendance_legacy.csv");
  }

  Serial.println("[MIGRATE] Rebuilding attendance.csv from immutable legacy source...");

  // Only attendance.csv is rebuilt atomically. system.log is preserved in
  // place and receives missing legacy diagnostic rows idempotently below, so
  // migration never deletes diagnostics that already existed on the card.
  SD.remove(SD_ATTENDANCE_TMP_FILE);

  File legacy = SD.open(SD_LEGACY_LOG_FILE, FILE_READ);
  if (!legacy) {
    Serial.println("[MIGRATE] Legacy source cannot be opened; migration deferred.");
    return;
  }

  File af = SD.open(SD_ATTENDANCE_TMP_FILE, FILE_APPEND);
  if (!af) {
    legacy.close();
    Serial.println("[MIGRATE] Temporary attendance file open failed; migration deferred.");
    return;
  }

  const size_t headerBytes = strlen(ATTENDANCE_CSV_HEADER) + 1;
  bool allWritesOk = true;
  if (af.print(ATTENDANCE_CSV_HEADER) != strlen(ATTENDANCE_CSV_HEADER) || af.print("\n") != 1)
    allWritesOk = false;

  String fields[8];
  if (allWritesOk) {
    sdReaderAttach(legacy);
    if (!readCsvRecord(legacy, fields, 8)) allWritesOk = false;
  }

  uint32_t migratedAttendance = 0, migratedSystem = 0, sinceYield = 0;
  size_t expectedAttendanceBytes = headerBytes;

  while (allWritesOk && readCsvRecord(legacy, fields, 8)) {
    bool allEmpty = true;
    for (int i = 0; i < 8; i++) if (fields[i].length() > 0) { allEmpty = false; break; }
    if (allEmpty) continue;

    String timeText = fields[0], event = fields[1];
    int id = fields[2].toInt();
    String name = fields[3], collegeId = fields[4], role = fields[5], department = fields[6], message = fields[7];

    String rowDate, rowTime;
    bool validTime = splitDateTimeText(timeText, rowDate, rowTime);
    String eventType = validTime ? attendanceEventTypeOf(event) : String("");

    // Only well-formed legacy CHECK IN/OUT records become attendance rows.
    // Everything else is retained as a system diagnostic entry.
    if (eventType.length() > 0 && id >= 1 && id <= MAX_FINGERPRINTS) {
      String status = attendanceStatusEnumOf(event);
      String row = buildAttendanceCsvRow(rowDate, rowTime, id, name, collegeId, role, department, eventType, status);
      size_t n = af.write((const uint8_t*)row.c_str(), row.length());
      if (n != row.length()) { allWritesOk = false; break; }
      expectedAttendanceBytes += row.length();
      migratedAttendance++;
    } else {
      String level = "INFO";
      String eUp = event; eUp.toUpperCase();
      if (eUp == "ERROR") level = "ERROR";
      else if (eUp == "WARNING") level = "WARNING";
      String logDate = validTime ? rowDate : String("");
      String logTime = validTime ? rowTime : timeText;
      String row = buildSystemLogRow(logDate, logTime, level, event, message);
      migratedSystem++;
      // system.log is handled after the attendance source has been fully
      // converted, so it can be retried independently without ever deleting
      // existing diagnostics.
      (void)row;
    }

    if (++sinceYield >= 50) { sinceYield = 0; vTaskDelay(1); }
  }

  legacy.close();
  af.flush();
  const size_t actualAttendanceBytes = af.size();
  af.close();

  if (!allWritesOk || actualAttendanceBytes != expectedAttendanceBytes) {
    SD.remove(SD_ATTENDANCE_TMP_FILE);
    Serial.printf("[MIGRATE] ABORTED: attendance temp size mismatch (%u/%u). Legacy source preserved.\n",
                  (unsigned)actualAttendanceBytes, (unsigned)expectedAttendanceBytes);
    return;
  }

  // Validate that the complete temporary attendance file can be reopened.
  File verifyAttendance = SD.open(SD_ATTENDANCE_TMP_FILE, FILE_READ);
  bool tempReadable = verifyAttendance && verifyAttendance.size() == expectedAttendanceBytes;
  if (verifyAttendance) verifyAttendance.close();
  if (!tempReadable) {
    SD.remove(SD_ATTENDANCE_TMP_FILE);
    Serial.println("[MIGRATE] ABORTED: attendance temp read-back failed. Legacy source preserved.");
    return;
  }

  // Swap only the fully verified attendance file. If power fails here, the
  // immutable legacy source remains, and the next boot can repeat the swap.
  SD.remove(SD_LOG_FILE);
  if (!SD.rename(SD_ATTENDANCE_TMP_FILE, SD_LOG_FILE)) {
    Serial.println("[MIGRATE] Attendance final rename failed; schema remains old for retry.");
    return;
  }

  // Idempotently migrate the non-attendance legacy diagnostics into the
  // existing system.log. Exact-line detection prevents duplicate rows if a
  // previous boot was interrupted after some diagnostics were appended.
  const bool legacy2Exists = SD.exists(SD_LEGACY_LOG_FILE);
  File legacy2 = SD.open(SD_LEGACY_LOG_FILE, FILE_READ);
  // The legacy diagnostics file is optional. Its absence is a valid state
  // (for example, a clean card or a card that contained only attendance).
  // Only treat it as a migration failure when it exists but cannot be opened.
  bool systemMigrationOk = !legacy2Exists || (bool)legacy2;
  if (legacy2) {
    sdReaderAttach(legacy2);
    String f2[8];
    if (!readCsvRecord(legacy2, f2, 8)) systemMigrationOk = false;
    while (systemMigrationOk && readCsvRecord(legacy2, f2, 8)) {
      bool allEmpty = true;
      for (int i=0;i<8;i++) if (f2[i].length()>0) { allEmpty=false; break; }
      if (allEmpty) continue;

      String timeText=f2[0], event=f2[1];
      int id=f2[2].toInt();
      String name=f2[3], collegeId=f2[4], role=f2[5], department=f2[6], message=f2[7];
      String rowDate,rowTime;
      bool validTime=splitDateTimeText(timeText,rowDate,rowTime);
      String eventType=validTime?attendanceEventTypeOf(event):String("");
      if(eventType.length()>0 && id>=1 && id<=MAX_FINGERPRINTS) continue;

      String level="INFO"; String eUp=event; eUp.toUpperCase();
      if(eUp=="ERROR") level="ERROR"; else if(eUp=="WARNING") level="WARNING";
      String logDate=validTime?rowDate:String("");
      String logTime=validTime?rowTime:timeText;
      String row=buildSystemLogRow(logDate,logTime,level,event,message);

      if(!sdFileContainsExactLine(SD_SYSLOG_FILE,row)) {
        if(!appendSdRowVerified(SD_SYSLOG_FILE,row,"MIGRATE")) {
          systemMigrationOk=false;
          break;
        }
      }
    }
    legacy2.close();
  }

  if(!systemMigrationOk) {
    Serial.println("[MIGRATE] Attendance conversion is complete, but system.log migration is incomplete; schema stays old and will retry idempotently.");
    return;
  }

  migrateLessonsHeaderIfNeeded();
  const bool versionSaved = prefsPutInt(STORAGE_SCHEMA_KEY, STORAGE_SCHEMA_CURRENT);
  if (!versionSaved) {
    Serial.println("[MIGRATE] Files migrated successfully, but schema marker save failed; next boot will re-verify idempotently.");
  }
  sdCsvBytesCache = expectedAttendanceBytes;
  sdStatValid = false;
  Serial.printf("[MIGRATE] Done. attendance=%u system=%u; attendance bytes=%u verified.\n",
                (unsigned)migratedAttendance, (unsigned)migratedSystem, (unsigned)expectedAttendanceBytes);
}

// =====================================================
// LOAD TODAY FROM SD -> RAM
// =====================================================
void scanSdHistoryOnBootTask() {
  if (!sdAvailable) { sdRecordCount = 0; return; }
  uint32_t records = 0; int entries = 0, exits = 0, lastEntry = -1;
  uint32_t todayLoaded = 0;
  const String todayDate = currentDateOnlyText();
  if (todayDate.length() == 10) refreshDailyAttendanceStateForDate(todayDate.c_str());
  File f = SD.open(SD_LOG_FILE, FILE_READ);
  if (f) {
    sdReaderAttach(f);
    String fields[9];
    if (readCsvRecord(f, fields, 9)) {
      uint32_t sinceYield = 0;
      while (readCsvRecord(f, fields, 9)) {
        bool allEmpty = true;
        for (int i=0;i<9;i++) if (fields[i].length()>0) { allEmpty = false; break; }
        if (allEmpty) continue;
        if(!isValidDateStr(fields[0]) || !sdValidAttendanceTime(fields[1]) || fields[2].toInt()<1 || fields[2].toInt()>MAX_FINGERPRINTS ||
           (fields[7]!="CHECK_IN" && fields[7]!="CHECK_OUT") ||
           (fields[8]!="EARLY" && fields[8]!="ON_TIME" && fields[8]!="LATE" && fields[8]!="LEFT_EARLY" && fields[8]!="OVERTIME")) continue;
        records++;
        const int id = fields[2].toInt();
        const bool checkIn = (fields[7] == "CHECK_IN");
        if (checkIn) { entries++; lastEntry=id; }
        else { exits++; }

        // Rebuild only the current date's attendance flags from the durable
        // CSV. Older days never force a new day to begin with CHECK_OUT.
        if (todayDate.length() == 10 && fields[0] == todayDate && lockState(50)) {
          if (checkIn) todayCheckInDone[id] = true;
          else todayCheckOutDone[id] = true;
          unlockState();
        }

        // Cloud-only firmware keeps durable history on SD/cloud; rebuilding per-row String objects here fragments internal RAM needed by TLS. Counters above are still computed from every valid SD row.
        if (!CLOUD_ONLY_MODE && todayDate.length()==10 && fields[0] == todayDate) {
          const String statusDisplay = attendanceStatusDisplayOf(fields[8]);
          const String eventText = (checkIn ? "CHECK IN - " : "CHECK OUT - ") + statusDisplay;
          const String combinedTime = fields[0] + " " + fields[1];
          const String verb = checkIn ? "checked in at " : "checked out at ";
          const String message = (fields[3].length() ? fields[3] : ("ID " + String(id))) + " " + verb + combinedTime + " - " + eventText;
          bool recoveredAlreadyPresent = false;
          if (lockState(100)) {
            for (int li = 0; li < logCount; li++) {
              int lidx = (logWriteIndex - logCount + li + MAX_LOGS) % MAX_LOGS;
              const AttendanceLog &existing = logs[lidx];
              if (existing.seq != 0 &&
                  existing.id == id &&
                  existing.timeText == combinedTime &&
                  existing.event == eventText) {
                recoveredAlreadyPresent = true;
                break;
              }
            }
            if (!recoveredAlreadyPresent) {
              AttendanceLog &l = logs[logWriteIndex];
              l.timeText=combinedTime; l.event=eventText; l.id=id;
              l.name=fields[3]; l.collegeId=fields[4]; l.role=fields[5]; l.department=fields[6];
              l.message=message; l.seq=0; l.savedToSd=true;
              logWriteIndex=(logWriteIndex+1)%MAX_LOGS;
              if(logCount<MAX_LOGS) logCount++; else ramDropped++;
              todayLoaded++;
            }
            unlockState();
          }
        }
        if (++sinceYield >= 100) { sinceYield = 0; vTaskDelay(1); }
      }
    }
    f.close();
  }
  sdRecordCount = records;

  uint32_t lc = 0;
  File lf = SD.open(SD_LESSONS_FILE, FILE_READ);
  if (lf) {
    sdReaderAttach(lf);
    String lf9[9];
    if (readCsvRecord(lf, lf9, 9)) {
      while (readCsvRecord(lf, lf9, 9)) {
        bool allEmpty = true;
        for (int i=0;i<9;i++) if (lf9[i].length()>0) { allEmpty=false; break; }
        if (allEmpty) continue;
        lc++;
      }
    }
    lf.close();
  }
  sdLessonCount = lc;

  if (lockState(500)) { entryCount=entries; exitCount=exits; if (lastEntry>=1) lastEntryId=lastEntry; unlockState(); }
  Serial.print("[SD] Today loaded: "); Serial.println(todayLoaded);
  Serial.print("[SD] Records: "); Serial.println(records);
  Serial.print("[SD] Lessons: "); Serial.println(lc);
}

// =====================================================
// BULK SD REQUEST HANDLER
// =====================================================
static bool sdEmitBuf(QueueHandle_t queue,uint32_t id,String &data){
  if(!queue || data.length()==0) return true;

  // Copy from a moving offset instead of repeatedly String::remove(0,n).
  // The old approach shifted the whole String for every 511-byte chunk,
  // which made large historical reads increasingly expensive (O(n^2)).
  size_t offset=0;
  const size_t total=data.length();
  while(offset<total){
    if(!sdHttpStreamMatches(id,queue)) return false;

    SdBulkChunk chunk{};
    chunk.id=id;
    chunk.len=total-offset>SD_CHUNK_CAP?SD_CHUNK_CAP:total-offset;
    memcpy(chunk.data,data.c_str()+offset,chunk.len);
    chunk.done=false;
    chunk.ok=true;

    const unsigned long waitStart=millis();
    bool queued=false;
    while(sdHttpStreamMatches(id,queue)){
      // Reserve one slot for the final completion marker.
      if(uxQueueSpacesAvailable(queue)>1){
        if(xQueueSend(queue,&chunk,0)==pdTRUE){ queued=true; break; }
      }
      if(millis()-waitStart>=SD_HTTP_STREAM_BACKPRESSURE_MS) break;
      vTaskDelay(1);
    }

    if(!queued) return false;
    offset+=chunk.len;
  }
  // All bytes in this String have been copied into the response queue.
  // Clear the whole buffer; removing only one byte would retain almost the
  // entire payload and cause subsequent chunks/responses to duplicate data.
  data.clear();
  return true;
}

static bool sdEmitDone(QueueHandle_t queue,uint32_t id,bool ok){
  if(!queue || !sdHttpStreamMatches(id,queue)) return false;

  SdBulkChunk chunk{};
  chunk.id=id;
  chunk.len=0;
  chunk.done=true;
  chunk.ok=ok;

  const unsigned long waitStart=millis();
  while(sdHttpStreamMatches(id,queue)){
    if(xQueueSend(queue,&chunk,0)==pdTRUE) return true;
    if(millis()-waitStart>=SD_HTTP_STREAM_BACKPRESSURE_MS) break;
    vTaskDelay(1);
  }

  Serial.println("[SD] WARN: stream completion could not be delivered");
  return false;
}

static void applyAttendanceDeleteToRam(){
  if(lockState(500)){
    logCount=0; logWriteIndex=0; logSeqCounter=0; ramDropped=0;
    for(int i=0;i<MAX_LOGS;i++){
      logs[i].timeText=logs[i].event=logs[i].name="";
      logs[i].collegeId=logs[i].role=logs[i].department=logs[i].message="";
      logs[i].id=-1; logs[i].seq=0; logs[i].savedToSd=false;
    }
    for(int i=0;i<=MAX_FINGERPRINTS;i++){
      insideState[i]=false;
      lastAttendanceActionMs[i]=0;
    }
    const String insideTrue = "\"inside\":true";
    const String insideFalse = "\"inside\":false";
    int pos=0;
    while((pos=cachedRegisteredJson.indexOf(insideTrue,pos))>=0){
      cachedRegisteredJson = cachedRegisteredJson.substring(0,pos) +
                            insideFalse + cachedRegisteredJson.substring(pos + insideTrue.length());
      pos += insideFalse.length();
    }
    entryCount=0; exitCount=0; lastEntryId=-1;
    cachedLastEntryJsonForId=-1; cachedLastEntryJson="null";
    tickLastRamDroppedSeen=0;
    bootSnapshotDirty=true;
    unlockState();
  }

  bool presenceCleared=true;
  if(lockPrefs(1500)){
    char key[16];
    for(uint16_t id=1; id<=MAX_FINGERPRINTS; id++){
      snprintf(key,sizeof(key),"p_%u",(unsigned)id);
      if(!prefs.remove(key)) presenceCleared=false;
    }
    unlockPrefs();
  } else {
    presenceCleared=false;
  }
  if(!presenceCleared) Serial.println("[SD] WARN: persisted presence reset incomplete.");
  addLog("INFO",-1,"Attendance data deleted from SD card");
}

void serviceSdBulkRequest(SdBulkRequest &req) {
  SdBulkChunk chunk;
  const bool hasDateFilter   = (req.date[0] != 0);
  const bool hasDateToFilter = (req.dateTo[0] != 0);

  // Type 3: DELETE attendance.csv
  if (req.type == 3) {
    bool ok = false;
    if (attendanceJournalActive || pendingUsed>0 || uxQueueMessagesWaiting(attendanceQueue)>0) {
      Serial.println("[SD] Delete attendance refused: pending attendance transaction exists.");
      sdEmitDone(req.replyQueue, req.id,false);
      return;
    }
    if (sdAvailable) {
      digitalWrite(SD_CS, HIGH); delayMicroseconds(5);
      const bool existedBefore = SD.exists(SD_LOG_FILE);
      const bool removed = !existedBefore || SD.remove(SD_LOG_FILE);
      bool recreated = false;
      if (removed) {
        File w = SD.open(SD_LOG_FILE, FILE_APPEND);
        if (w) {
          const size_t hlen = strlen(ATTENDANCE_CSV_HEADER);
          const bool wroteHeader =
            (w.write((const uint8_t*)ATTENDANCE_CSV_HEADER, hlen) == hlen) &&
            (w.write((const uint8_t*)"\n", 1) == 1);
          w.flush();
          w.close();
          recreated = wroteHeader;
        }
      }
      File verify = recreated ? SD.open(SD_LOG_FILE, FILE_READ) : File();
      const bool verified = verify && verify.size() == strlen(ATTENDANCE_CSV_HEADER) + 1;
      sdCsvBytesCache = verified ? verify.size() : 0;
      if (verify) verify.close();
      ok = removed && recreated && verified;
      if (ok) {
        sdRecordCount = 0; sdStatValid = false;
        sdTotalBytesCache = SD.cardSize(); sdUsedBytesCache = SD.usedBytes();
        sdUsedBytesAtMs = millis(); sdStatAtMs = millis(); sdStatValid = true;
      }
    }
    Serial.print("[SD] Delete attendance: "); Serial.println(ok ? "ok" : "failed");
    if(ok) applyAttendanceDeleteToRam();
    sdEmitDone(req.replyQueue, req.id, ok);
    return;
  }

  // Type 5: lessons by date
  if (req.type == 5) {
    if (!sdAvailable) { sdEmitDone(req.replyQueue, req.id,false); return; }
    File f = SD.open(SD_LESSONS_FILE, FILE_READ);
    if (!f) { sdEmitDone(req.replyQueue, req.id,false); return; }
    sdReaderAttach(f);
    String fields[9]; readCsvRecord(f, fields, 9);
    String buf; buf.reserve(1100); bool first=true; uint32_t sinceYield=0;
    while (readCsvRecord(f, fields, 9)) {
      bool allEmpty=true; for (int i=0;i<9;i++) if (fields[i].length()>0) { allEmpty=false; break; }
      if (allEmpty) continue;
      if (!rowMatchesDate(fields[0], req.date)) continue;
      String obj = "{";
      obj += "\"date\":\""        + jsonEscape(fields[0]) + "\",";
      obj += "\"time\":\""        + jsonEscape(fields[1]) + "\",";
      obj += "\"fingerprintId\":" + String(fields[2].toInt()) + ",";
      obj += "\"name\":\""        + jsonEscape(fields[3]) + "\",";
      obj += "\"collegeId\":\""   + jsonEscape(fields[4]) + "\",";
      obj += "\"role\":\""        + jsonEscape(fields[5]) + "\",";
      obj += "\"department\":\""  + jsonEscape(fields[6]) + "\",";
      obj += "\"type\":\""        + jsonEscape(fields[7]) + "\",";
      obj += "\"lesson\":\""      + jsonEscape(fields[8]) + "\"}";
      if (!first) buf += ",";
      buf += obj; first = false;
      if (buf.length() >= 900) {
        if (!sdEmitBuf(req.replyQueue, req.id, buf)) { f.close(); sdEmitDone(req.replyQueue, req.id, false); return; }
      }
      if (++sinceYield >= 100) { sinceYield = 0; vTaskDelay(1); }
    }
    f.close();
    if (!sdEmitBuf(req.replyQueue, req.id, buf)) { sdEmitDone(req.replyQueue, req.id, false); return; }
    sdEmitDone(req.replyQueue, req.id, true);
    return;
  }

  // Type 6: lessons by range
  if (req.type == 6) {
    if (!sdAvailable) { sdEmitDone(req.replyQueue, req.id,false); return; }
    File f = SD.open(SD_LESSONS_FILE, FILE_READ);
    if (!f) { sdEmitDone(req.replyQueue, req.id,false); return; }
    sdReaderAttach(f);
    String fields[9]; readCsvRecord(f, fields, 9);
    String buf; buf.reserve(1100); bool first=true; uint32_t sinceYield=0;
    while (readCsvRecord(f, fields, 9)) {
      bool allEmpty=true; for (int i=0;i<9;i++) if (fields[i].length()>0) { allEmpty=false; break; }
      if (allEmpty) continue;
      String d = fields[0];
      if (hasDateFilter   && d < String(req.date))   continue;
      if (hasDateToFilter && d > String(req.dateTo)) continue;
      String obj = "{";
      obj += "\"date\":\""        + jsonEscape(fields[0]) + "\",";
      obj += "\"time\":\""        + jsonEscape(fields[1]) + "\",";
      obj += "\"fingerprintId\":" + String(fields[2].toInt()) + ",";
      obj += "\"name\":\""        + jsonEscape(fields[3]) + "\",";
      obj += "\"collegeId\":\""   + jsonEscape(fields[4]) + "\",";
      obj += "\"role\":\""        + jsonEscape(fields[5]) + "\",";
      obj += "\"department\":\""  + jsonEscape(fields[6]) + "\",";
      obj += "\"type\":\""        + jsonEscape(fields[7]) + "\",";
      obj += "\"lesson\":\""      + jsonEscape(fields[8]) + "\"}";
      if (!first) buf += ",";
      buf += obj; first = false;
      if (buf.length() >= 900) {
        if (!sdEmitBuf(req.replyQueue, req.id, buf)) { f.close(); sdEmitDone(req.replyQueue, req.id, false); return; }
      }
      if (++sinceYield >= 100) { sinceYield = 0; vTaskDelay(1); }
    }
    f.close();
    if (!sdEmitBuf(req.replyQueue, req.id, buf)) { sdEmitDone(req.replyQueue, req.id, false); return; }
    sdEmitDone(req.replyQueue, req.id, true);
    return;
  }

  // Type 1: CSV export
  if (req.type == 1) {
    if (!sdAvailable) { sdEmitDone(req.replyQueue, req.id,false); return; }
    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (!f) { sdEmitDone(req.replyQueue, req.id,false); return; }

    if (!hasDateFilter) {
      bool streamOk = true;
      while (f.available()) {
        if (!sdHttpStreamMatches(req.id, req.replyQueue)) {
          streamOk = false;
          break;
        }
        chunk.id=req.id;
        chunk.len = f.readBytes(chunk.data, sizeof(chunk.data));
        if (chunk.len == 0) { streamOk = false; break; }
        String rawChunk; rawChunk.reserve(chunk.len);
        rawChunk.concat(chunk.data, chunk.len);
        if (!sdEmitBuf(req.replyQueue, req.id, rawChunk)) {
          streamOk = false;
          break;
        }
        vTaskDelay(1);
      }
      f.close();
      // Never report a successful export after a chunk was lost.
      if (sdHttpStreamMatches(req.id, req.replyQueue)) {
        sdEmitDone(req.replyQueue, req.id, streamOk);
      }
      return;
    }

    String fields[9]; String buf; buf.reserve(560);
    bool headerEmitted = false; uint32_t sinceYield = 0;
    while (readCsvRecord(f, fields, 9)) {
      bool allEmpty = true; for (int i=0;i<9;i++) if (fields[i].length()>0) { allEmpty=false; break; }
      if (allEmpty) continue;
      if (!headerEmitted) {
        headerEmitted = true;
        for (int i=0;i<9;i++) { if (i>0) buf += ","; buf += csvEscape(fields[i]); }
        buf += "\n"; continue;
      }
      if (!rowMatchesDate(fields[0], req.date)) continue;
      for (int i=0;i<9;i++) { if (i>0) buf += ","; buf += csvEscape(fields[i]); }
      buf += "\n";
      if (buf.length() >= 900) {
        if (!sdEmitBuf(req.replyQueue, req.id, buf)) { f.close(); sdEmitDone(req.replyQueue, req.id, false); return; }
      }
      if (++sinceYield >= 100) { sinceYield = 0; vTaskDelay(1); }
    }
    f.close();
    if (!sdEmitBuf(req.replyQueue, req.id, buf)) { sdEmitDone(req.replyQueue, req.id, false); return; }
    sdEmitDone(req.replyQueue, req.id, true);
    return;
  }

  // Type 7: system.log JSON by exact date
  if (req.type == 7) {
    if (!sdAvailable) { sdEmitDone(req.replyQueue, req.id,false); return; }
    File f = SD.open(SD_SYSLOG_FILE, FILE_READ);
    if (!f) { sdEmitDone(req.replyQueue, req.id,false); return; }
    sdReaderAttach(f);
    String fields[5]; readCsvRecord(f, fields, 5);
    String buf; buf.reserve(1100); bool first=true; uint32_t sinceYield=0;
    while (readCsvRecord(f, fields, 5)) {
      bool allEmpty=true; for(int i=0;i<5;i++) if(fields[i].length()>0){ allEmpty=false; break; }
      if(allEmpty) continue;
      if(!rowMatchesDate(fields[0], req.date)) continue;

      String obj="{";
      obj += "\"date\":\""+jsonEscape(fields[0])+"\",";
      obj += "\"time\":\""+jsonEscape(fields[1])+"\",";
      obj += "\"level\":\""+jsonEscape(fields[2])+"\",";
      obj += "\"event\":\""+jsonEscape(fields[3])+"\",";
      obj += "\"message\":\""+jsonEscape(fields[4])+"\"}";
      if(!first) buf+=",";
      buf+=obj; first=false;
      if(buf.length()>=900){
        if(!sdEmitBuf(req.replyQueue,req.id,buf)){ f.close(); sdEmitDone(req.replyQueue,req.id,false); return; }
      }
      if(++sinceYield>=100){sinceYield=0;vTaskDelay(1);}
    }
    f.close();
    if(!sdEmitBuf(req.replyQueue,req.id,buf)){ sdEmitDone(req.replyQueue,req.id,false); return; }
    sdEmitDone(req.replyQueue,req.id,true);
    return;
  }

  // Type 2: JSON logs (historical)
  if (req.type == 2) {
    if (!sdAvailable) { sdEmitDone(req.replyQueue, req.id,false); return; }
    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (!f) { sdEmitDone(req.replyQueue, req.id,false); return; }

    // New attendance.csv schema (9 columns): date,time,fingerprint_id,
    // name,college_id,role,department,event_type,status. Reconstruct
    // the old combined time/event/message JSON shape so historical
    // rows render identically to live ones in the dashboard.
    sdReaderAttach(f);
    String fields[9]; readCsvRecord(f, fields, 9);
    String buf; buf.reserve(1100); bool first=true; uint32_t sinceYield=0;
    while (readCsvRecord(f, fields, 9)) {
      bool allEmpty = true; for (int i=0;i<9;i++) if (fields[i].length()>0) { allEmpty=false; break; }
      if (allEmpty) continue;
      if(!isValidDateStr(fields[0]) || !sdValidAttendanceTime(fields[1]) || fields[2].toInt()<1 || fields[2].toInt()>MAX_FINGERPRINTS ||
         (fields[7]!="CHECK_IN" && fields[7]!="CHECK_OUT") ||
         (fields[8]!="EARLY" && fields[8]!="ON_TIME" && fields[8]!="LATE" && fields[8]!="LEFT_EARLY" && fields[8]!="OVERTIME")) continue;
      if (hasDateFilter && !rowMatchesDate(fields[0], req.date)) continue;

      String rowDate = fields[0], rowTime = fields[1];
      int id = fields[2].toInt();
      String name = fields[3], collegeId = fields[4], role = fields[5], department = fields[6];
      String eventType = fields[7], statusEnum = fields[8];
      String statusDisplay = attendanceStatusDisplayOf(statusEnum);
      bool isCheckOut = (eventType == "CHECK_OUT");
      String eventText = (isCheckOut ? "CHECK OUT - " : "CHECK IN - ") + statusDisplay;
      String combinedTime = rowDate + " " + rowTime;
      String message = (name.length() ? name : ("ID " + String(id))) +
                        (isCheckOut ? " checked out at " : " checked in at ") +
                        combinedTime + " - " + eventText;

      String obj = "{";
      obj += "\"time\":\""        + jsonEscape(combinedTime) + "\",";
      obj += "\"event\":\""       + jsonEscape(eventText) + "\",";
      obj += "\"id\":"            + String(id) + ",";
      obj += "\"name\":\""        + jsonEscape(name) + "\",";
      obj += "\"collegeId\":\""   + jsonEscape(collegeId) + "\",";
      obj += "\"role\":\""        + jsonEscape(role) + "\",";
      obj += "\"department\":\""  + jsonEscape(department) + "\",";
      obj += "\"message\":\""     + jsonEscape(message) + "\",";
      obj += "\"pending\":false}";
      if (!first) buf += ",";
      buf += obj; first=false;
      if (buf.length() >= 900) {
        if (!sdEmitBuf(req.replyQueue, req.id, buf)) { f.close(); sdEmitDone(req.replyQueue, req.id, false); return; }
      }
      if (++sinceYield >= 100) { sinceYield = 0; vTaskDelay(1); }
    }
    f.close();
    if (!sdEmitBuf(req.replyQueue, req.id, buf)) { sdEmitDone(req.replyQueue, req.id, false); return; }
    sdEmitDone(req.replyQueue, req.id, true);
    return;
  }
}

// =====================================================
// LED/BUZZER + OLED KEEPALIVE
// =====================================================