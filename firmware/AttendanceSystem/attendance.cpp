#include "app.h"

bool splitDateTimeText(const String &timeText, String &outDate, String &outTime) {
  if (timeText.length() != 19) return false;
  if (timeText[4] != '-' || timeText[7] != '-' || timeText[10] != ' ') return false;
  String d = timeText.substring(0, 10);
  String t = timeText.substring(11);
  if (!isValidDateStr(d)) return false;
  outDate = d; outTime = t;
  return true;
}

// event is one of the free-form strings addLog() has always used, e.g.
// "CHECK IN - LATE", "CHECK OUT - OVERTIME". Returns "CHECK_IN",
// "CHECK_OUT", or "" if this is not an attendance event at all.
String attendanceEventTypeOf(const String &event) {
  if (event.startsWith("CHECK IN"))  return "CHECK_IN";
  if (event.startsWith("CHECK OUT")) return "CHECK_OUT";
  return "";
}

// Maps the free-form attendance suffix onto the persisted status enum.
// EARLY is intentionally kept as a distinct value so a check-in before
// the on-time minute remains distinguishable in historical reports.
String attendanceStatusEnumOf(const String &event) {
  String e = event; e.toUpperCase();
  if (e.indexOf("LEFT EARLY") >= 0) return "LEFT_EARLY";
  if (e.indexOf("OVERTIME")   >= 0) return "OVERTIME";
  if (e.indexOf("LATE")       >= 0) return "LATE";
  if (e.indexOf("EARLY")      >= 0) return "EARLY";
  if (e.indexOf("ON TIME")    >= 0) return "ON_TIME";
  return "ON_TIME";
}

// Inverse of attendanceStatusEnumOf(), used when reconstructing
// historical CHECK IN / CHECK OUT events from attendance.csv.
String attendanceStatusDisplayOf(const String &statusEnum) {
  if (statusEnum == "EARLY")       return "EARLY";
  if (statusEnum == "LEFT_EARLY") return "LEFT EARLY";
  if (statusEnum == "OVERTIME")   return "OVERTIME";
  if (statusEnum == "LATE")       return "LATE";
  return "ON TIME";
}

String buildAttendanceCsvRow(const String &dateStr, const String &timeStr, int id,
                              const String &name, const String &collegeId, const String &role,
                              const String &department, const String &eventType, const String &status) {
  String row; row.reserve(220);
  row += csvEscape(dateStr);      row += ",";
  row += csvEscape(timeStr);      row += ",";
  row += String(id);              row += ",";
  row += csvEscape(name);         row += ",";
  row += csvEscape(collegeId);    row += ",";
  row += csvEscape(role);         row += ",";
  row += csvEscape(department);   row += ",";
  row += csvEscape(eventType);    row += ",";
  row += csvEscape(status);       row += "\n";
  return row;
}

String buildSystemLogRow(const String &dateStr, const String &timeStr, const String &level,
                          const String &event, const String &message) {
  String row; row.reserve(220);
  row += csvEscape(dateStr);  row += ",";
  row += csvEscape(timeStr);  row += ",";
  row += csvEscape(level);    row += ",";
  row += csvEscape(event);    row += ",";
  row += csvEscape(message);  row += "\n";
  return row;
}

// =====================================================
// TRANSACTIONAL ATTENDANCE STORAGE
// =====================================================
// Critical attendance events are fail-closed. A CHECK IN/OUT is acknowledged
// only after: valid time -> NVS journal -> SD write -> flush/close -> exact
// read-back -> journal removal. This also gives a replay-safe recovery point
// for a power loss between the SD write and the NVS cleanup.

static int parseAttendanceCsvLine(const String &line, String fields[], int maxFields) {
  for (int i=0;i<maxFields;i++) fields[i]="";
  int out=0;
  String cur; cur.reserve(64);
  bool inQuotes=false;
  for (size_t i=0;i<line.length();i++) {
    char c=line[i];
    if (inQuotes) {
      if (c=='"') {
        if (i+1<line.length() && line[i+1]=='"') { cur+='"'; i++; }
        else inQuotes=false;
      } else cur+=c;
    } else {
      if (c=='"') inQuotes=true;
      else if (c==',') {
        if (out<maxFields) fields[out]=cur;
        out++; cur="";
      } else if (c!='\r' && c!='\n') cur+=c;
    }
  }
  if (out<maxFields) fields[out]=cur;
  return out+1;
}

static bool validStoredAttendanceTime(const String &t) {
  if(t.length()!=8 || t[2]!=':' || t[5]!=':') return false;
  for(int i=0;i<8;i++) if(i!=2 && i!=5 && !isDigit((unsigned char)t[i])) return false;
  int hh=t.substring(0,2).toInt();
  int mm=t.substring(3,5).toInt();
  int ss=t.substring(6,8).toInt();
  return hh>=0 && hh<=23 && mm>=0 && mm<=59 && ss>=0 && ss<=59;
}

static bool validStoredAttendanceRow(const String &row) {
  String f[9];
  if (parseAttendanceCsvLine(row,f,9)!=9) return false;
  if (!isValidDateStr(f[0]) || !validStoredAttendanceTime(f[1])) return false;
  int id=f[2].toInt();
  if (id<1 || id>MAX_FINGERPRINTS) return false;
  if (f[7]!="CHECK_IN" && f[7]!="CHECK_OUT") return false;
  if (f[8]!="EARLY" && f[8]!="ON_TIME" && f[8]!="LATE" && f[8]!="LEFT_EARLY" && f[8]!="OVERTIME") return false;
  return true;
}

bool storageCanAcceptAttendance() {
  if (!sdAvailable) return false;
  if (attendanceJournalActive) return false;
  if (!attendanceQueue) return false;
  if (uxQueueSpacesAvailable(attendanceQueue)==0) return false;
  int pending = 0;
  if (!lockState(20)) return false;
  pending = pendingUsed;
  unlockState();
  return pending < MAX_PENDING;
}

bool persistAttendanceJournal(uint32_t seq,const String& row) {
  if(seq==0 || !validStoredAttendanceRow(row)) return false;
  String payload; payload.reserve(380);
  payload += String(seq); payload += '\n'; payload += row;
  if(payload.length()>SD_JOURNAL_MAX_PAYLOAD) return false;

  // Critical attendance uses a write-then-read verification. The generic
  // Preferences helper only reports that it obtained the mutex, not that the
  // underlying NVS write produced the expected value.
  if(!lockPrefs(1000)) return false;
  size_t written = prefs.putString(SD_ATTENDANCE_JOURNAL_KEY,payload);
  String verify = prefs.getString(SD_ATTENDANCE_JOURNAL_KEY,"");
  unlockPrefs();
  if(written==0 || verify!=payload) {
    Serial.println("[SD] ERROR: attendance journal NVS write verification failed.");
    return false;
  }

  // Keep the sequence monotonic across a reboot/recovery cycle.
  if(lockState(100)) {
    if(seq>logSeqCounter) logSeqCounter=seq;
    unlockState();
  }
  attendanceJournalSeq=seq;
  attendanceJournalActive=true;
  attendanceCommitAck=false;
  attendanceJournalRecovery=false;
  attendanceStateAppliedSeq=0;
  return true;
}

void loadAttendanceJournal() {
  String payload=prefsGetString(SD_ATTENDANCE_JOURNAL_KEY,String());
  if(!payload.length()) return;
  int nl=payload.indexOf('\n');
  if(nl<=0 || nl>=(int)payload.length()-1) {
    Serial.println("[SD] ERROR: attendance journal malformed; retaining it for safety.");
    attendanceJournalActive=true;
    attendanceJournalSeq=0;
    attendanceCommitAck=false;
    attendanceJournalRecovery=true;
    return;
  }
  String seqText=payload.substring(0,nl); seqText.trim();
  String row=payload.substring(nl+1);
  char* endp=nullptr;
  unsigned long seq=strtoul(seqText.c_str(),&endp,10);
  if(endp==seqText.c_str() || *endp!='\0' || seq==0 || !validStoredAttendanceRow(row)) {
    Serial.println("[SD] ERROR: attendance journal failed validation; retaining it for safety.");
    attendanceJournalActive=true;
    attendanceJournalSeq=(uint32_t)seq;
    attendanceCommitAck=false;
    attendanceJournalRecovery=true;
    return;
  }
  attendanceJournalSeq=(uint32_t)seq;
  attendanceJournalActive=true;
  attendanceJournalRecovery=true;
  attendanceCommitAck=false;
  attendanceStateAppliedSeq=0;
  if(lockState(100)) {
    if(attendanceJournalSeq>logSeqCounter) logSeqCounter=attendanceJournalSeq;
    unlockState();
  }
  Serial.print("[SD] Recovered attendance journal seq="); Serial.println(attendanceJournalSeq);
}

bool readAttendanceJournalRow(String& row) {
  if(!attendanceJournalActive || attendanceJournalSeq==0) return false;
  String payload=prefsGetString(SD_ATTENDANCE_JOURNAL_KEY,String());
  int nl=payload.indexOf('\n');
  if(nl<=0) return false;
  String seqText=payload.substring(0,nl); seqText.trim();
  char* endp=nullptr;
  unsigned long seq=strtoul(seqText.c_str(),&endp,10);
  row=payload.substring(nl+1);
  return endp!=seqText.c_str() && *endp=='\0' && seq==attendanceJournalSeq && validStoredAttendanceRow(row);
}

bool clearAttendanceJournal() {
  if(!lockPrefs(1000)) return false;
  bool removed = prefs.remove(SD_ATTENDANCE_JOURNAL_KEY);
  bool gone = !prefs.isKey(SD_ATTENDANCE_JOURNAL_KEY);
  unlockPrefs();
  if(!removed && !gone) return false;
  attendanceJournalActive=false;
  attendanceJournalSeq=0;
  attendanceJournalRecovery=false;
  attendanceStateAppliedSeq=0;
  return gone;
}

bool waitForAttendanceCommit(uint32_t seq,uint32_t timeoutMs) {
  unsigned long start=millis();
  while(attendanceJournalActive && attendanceJournalSeq==seq) {
    serviceLedBuzzerTimers();
    if((uint32_t)(millis()-start)>=timeoutMs) return false;
    delay(5);
  }
  return attendanceCommitAck;
}

static bool ramHasAttendanceRow(const String& row) {
  String f[9];
  if(parseAttendanceCsvLine(row,f,9)!=9) return false;
  if(!lockState(150)) return false;
  bool found=false;
  for(int i=0;i<logCount;i++) {
    int idx=(logWriteIndex-logCount+i+MAX_LOGS)%MAX_LOGS;
    const AttendanceLog& l=logs[idx];
    if(l.id==f[2].toInt() && l.timeText==(f[0]+" "+f[1]) &&
       ((f[7]=="CHECK_IN" && l.event.startsWith("CHECK IN")) ||
        (f[7]=="CHECK_OUT" && l.event.startsWith("CHECK OUT")))) {
      found=true; break;
    }
  }
  unlockState();
  return found;
}

static void addRecoveredAttendanceToRam(const String& row,uint32_t seq) {
  if(ramHasAttendanceRow(row)) return;
  String f[9];
  if(parseAttendanceCsvLine(row,f,9)!=9) return;
  int id=f[2].toInt();
  if(id<1 || id>MAX_FINGERPRINTS) return;
  String statusDisplay=attendanceStatusDisplayOf(f[8]);
  bool checkOut=(f[7]=="CHECK_OUT");
  String eventText=(checkOut?"CHECK OUT - " : "CHECK IN - " )+statusDisplay;
  String timeText=f[0]+" "+f[1];
  String name=f[3];
  String message=(name.length()?name:("ID "+String(id)))+
                 (checkOut?" checked out at " : " checked in at " )+
                 timeText+" - "+eventText;
  if(lockState(150)) {
    AttendanceLog &l=logs[logWriteIndex];
    l.timeText=timeText; l.event=eventText; l.id=id;
    l.name=name; l.collegeId=f[4]; l.role=f[5]; l.department=f[6];
    l.message=message; l.seq=seq; l.savedToSd=true;
    logWriteIndex=(logWriteIndex+1)%MAX_LOGS;
    if(logCount<MAX_LOGS) logCount++; else ramDropped++;
    cachedLastEntryJsonForId=id;
    cachedLastEntryJson="{\"id\":"+String(id)+",\"name\":\""+jsonEscape(name)+"\",\"collegeId\":\""+jsonEscape(f[4])+"\",\"department\":\""+jsonEscape(f[6])+"\",\"message\":\"Last check in\"}";
    unlockState();
    bootSnapshotDirty=true;
  }
}

static bool applyAttendanceCommitState(const String& row,bool updateCounts) {
  String f[9];
  if(parseAttendanceCsvLine(row,f,9)!=9) return false;
  int id=f[2].toInt();
  if(id<1 || id>MAX_FINGERPRINTS) return false;
  bool checkIn=(f[7]=="CHECK_IN");
  bool checkOut=(f[7]=="CHECK_OUT");
  if(!checkIn && !checkOut) return false;

  // Persist the desired presence first. The old implementation changed the
  // in-memory state and incremented the counters before this NVS write. If
  // NVS verification failed, a later retry could increment entryCount or
  // exitCount again even though the SD row was already the same transaction.
  // Writing the desired value first makes a failed persistence a clean retry
  // with no counter mutation.
  if (!savePresenceStateValue((uint16_t)id, checkIn)) {
    Serial.printf("[ATTEND] ERROR: failed to persist presence state for ID %d; transaction retained.\n", id);
    return false;
  }

  const unsigned long nowMs=millis();
  if(!lockState(250)) return false;
  insideState[id]=checkIn;
  lastAttendanceActionMs[id]=nowMs;
  if(checkIn) { lastEntryId=id; if(updateCounts) entryCount++; }
  else if(checkOut && updateCounts) exitCount++;
  unlockState();

  updateRegisteredInsideCache((uint16_t)id, checkIn);
  return true;
}

bool finalizeAttendanceCommit(uint32_t seq,const String& row,bool recovery) {
  if(seq==0 || !validStoredAttendanceRow(row)) return false;

  // Apply runtime state exactly once. Recovery rows are already included in
  // the SD history that will be scanned at boot, so they update only the
  // current inside/outside state and never increment totals here.
  if(!recovery) {
    if(attendanceStateAppliedSeq!=seq) {
      if(!applyAttendanceCommitState(row,true)) return false;
      attendanceStateAppliedSeq=seq;
    }
  } else {
    if(!applyAttendanceCommitState(row,false)) return false;
    // loadTodayFromSdToRam() runs before journal recovery at boot. If this
    // journal row was not already present in SD, recovery adds it now. The
    // authoritative boot scan skips the same recovered row when it later
    // encounters it in attendance.csv.
    addRecoveredAttendanceToRam(row,seq);
  }

  markLogSaved(seq);
  if(clearAttendanceJournal()) {
    attendanceCommitAck=true;
    Serial.print("[SD] Attendance commit VERIFIED seq="); Serial.println(seq);
    return true;
  }

  // Keep the journal active until the NVS removal itself is verified. If a
  // later loop finds the exact row at EOF, finalizeAttendanceCommit() will
  // return here without double-counting because attendanceStateAppliedSeq
  // already contains the runtime sequence.
  Serial.println("[SD] ERROR: attendance journal cleanup failed; transaction retained.");
  return false;
}

static void removeRamLogSeq(uint32_t seq) {
  if(seq==0) return;
  if(!lockState(150)) return;
  if(logCount<=0) { unlockState(); return; }

  int target=-1;
  for(int i=0;i<logCount;i++) {
    int idx=(logWriteIndex-logCount+i+MAX_LOGS)%MAX_LOGS;
    if(logs[idx].seq==seq) { target=idx; break; }
  }
  if(target<0) { unlockState(); return; }

  const int newest=(logWriteIndex-1+MAX_LOGS)%MAX_LOGS;
  int cur=target;
  while(cur!=newest) {
    const int nxt=(cur+1)%MAX_LOGS;
    logs[cur]=logs[nxt];
    cur=nxt;
  }
  logs[newest].timeText=logs[newest].event=logs[newest].name="";
  logs[newest].collegeId=logs[newest].role=logs[newest].department=logs[newest].message="";
  logs[newest].id=-1; logs[newest].seq=0; logs[newest].savedToSd=false;
  logWriteIndex=newest;
  logCount--;

  cachedLastEntryJsonForId=-1;
  cachedLastEntryJson="null";
  if(logCount>0) {
    int idx=(logWriteIndex-1+MAX_LOGS)%MAX_LOGS;
    const AttendanceLog &l=logs[idx];
    if(l.id>=1) {
      cachedLastEntryJsonForId=l.id;
      cachedLastEntryJson="{\"id\":"+String(l.id)+",\"name\":\""+jsonEscape(l.name)+"\",\"collegeId\":\""+jsonEscape(l.collegeId)+"\",\"department\":\""+jsonEscape(l.department)+"\",\"message\":\"Last check in\"}";
    }
  }
  unlockState();
}


// =====================================================
// LOG CREATION
// =====================================================
bool addLog(String event, int id, String message) {
  AttendanceLog entry;
  entry.timeText = uptimeText();
  entry.event = event;
  entry.id = id;
  entry.message = message;
  entry.savedToSd = false;

  if (id >= 1 && id <= MAX_FINGERPRINTS) {
    PersonProfile p = loadProfile(id);
    entry.name = p.name; entry.collegeId = p.collegeId;
    entry.role = p.role; entry.department = p.department;
  }

  uint32_t seq = 0;
  bool stateAdded = false;
  if (lockState(200)) {
    seq = ++logSeqCounter;
    entry.seq = seq;
    logs[logWriteIndex] = entry;
    logWriteIndex = (logWriteIndex + 1) % MAX_LOGS;
    if (logCount < MAX_LOGS) logCount++; else ramDropped++;

    // Keep the last-entry cache under the same mutex as the source log.
    // Async HTTP handlers only ever copy it while holding this mutex.
    if (id >= 1 && id <= MAX_FINGERPRINTS) {
      cachedLastEntryJsonForId=id;
      cachedLastEntryJson="{\"id\":"+String(id)+",\"name\":\""+jsonEscape(entry.name)+"\",\"collegeId\":\""+jsonEscape(entry.collegeId)+"\",\"department\":\""+jsonEscape(entry.department)+"\",\"message\":\"Last check in\"}";
    } else {
      cachedLastEntryJsonForId=-1;
      cachedLastEntryJson="null";
    }
    stateAdded = true;
    unlockState();
  }
  if(!stateAdded || seq==0) return false;
  bootSnapshotDirty = true;

  // ---- STORAGE ROUTING (attendance.csv vs system.log) --------------
  // event_type is only ever non-empty for the two real attendance
  // actions (see attendanceEventTypeOf()). Everything else -- INFO,
  // ERROR, WARNING, ENROLL, ENROLL REQUEST, ENROLLED, CANCEL, DELETE,
  // DUPLICATE, MATCH, NOT FOUND, SYSTEM_READY, Wi-Fi/SD diagnostics,
  // etc. -- goes to /system.log. A valid RTC/NTP date is required for
  // an attendance row; if it's missing (entry.timeText is the
  // "UPTIME HH:MM:SS" fallback) the event is demoted to system.log
  // instead of ever being written with an invalid date, matching the
  // time-validation rule in section 4. This is only a safety net --
  // scanFingerprint() already refuses to create an attendance record
  // at all when time is invalid, before addLog() is ever called.
  String rowDate, rowTime;
  bool haveValidTime = splitDateTimeText(entry.timeText, rowDate, rowTime);
  String eventType = haveValidTime ? attendanceEventTypeOf(event) : String("");

  if (eventType.length() > 0) {
    if (!sdAvailable || attendanceJournalActive || !attendanceQueue) {
      Serial.println("[SD] Attendance refused: durable SD storage is not ready.");
      removeRamLogSeq(seq);
      return false;
    }

    String status = attendanceStatusEnumOf(event);
    String row = buildAttendanceCsvRow(rowDate, rowTime, id, entry.name, entry.collegeId,
                                        entry.role, entry.department, eventType, status);
    if(!persistAttendanceJournal(seq,row)) {
      Serial.println("[SD] ERROR: could not journal attendance; record NOT acknowledged.");
      removeRamLogSeq(seq);
      return false;
    }

    AttendanceEvent qEvt; qEvt.seq=seq;
    strncpy(qEvt.row,row.c_str(),sizeof(qEvt.row)-1);
    qEvt.row[sizeof(qEvt.row)-1]='\0';
    // The journal is authoritative. Queue is a fast signal; if it cannot be
    // queued, sdTask will discover the journal and recover it directly.
    if(xQueueSend(attendanceQueue,&qEvt,0)!=pdTRUE) {
      Serial.println("[SD] attendance queue full; journal retained for sdTask recovery.");
    }

    // Do not report success or alter presence state until the verified SD
    // commit has completed.
    return waitForAttendanceCommit(seq,SD_COMMIT_WAIT_MS);
  } else {
    String level="INFO";
    String eUp=event; eUp.toUpperCase();
    if(eUp=="ERROR") level="ERROR"; else if(eUp=="WARNING") level="WARNING";
    String logDate=haveValidTime?rowDate:String("");
    String logTime=haveValidTime?rowTime:entry.timeText;
    String row=buildSystemLogRow(logDate,logTime,level,event,message);
    SystemLogEvent qEvt; qEvt.seq=seq;
    strncpy(qEvt.row,row.c_str(),sizeof(qEvt.row)-1); qEvt.row[sizeof(qEvt.row)-1]='\0';
    if(xQueueSend(systemLogQueue,&qEvt,pdMS_TO_TICKS(5))!=pdTRUE) {
      // Keep a small protected overflow buffer so a burst of diagnostics
      // cannot silently disappear just because the SD writer queue is full.
      bool retained = false;
      if(lockState(50)) {
        if(systemLogFallbackCount < 4) {
          systemLogFallback[systemLogFallbackCount++] = qEvt;
          retained = true;
        } else {
          pendingQueueDropped++;
        }
        unlockState();
      }
      if(!retained) return false;
    }
  }

  Serial.print("[LOG] "); Serial.print(event); Serial.print(" ID="); Serial.println(id);
  return true;
}

// =====================================================
// STATUS JSON (with cached lastEntry)
// =====================================================