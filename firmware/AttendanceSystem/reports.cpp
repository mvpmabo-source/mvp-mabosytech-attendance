#include "app.h"

namespace {
String g_fastStatusJson;
SemaphoreHandle_t g_fastStatusMutex=nullptr;
unsigned long g_fastStatusAtMs=0;
}

static bool ensureFastStatusMutex(){
  if (g_fastStatusMutex) return true;
  g_fastStatusMutex = xSemaphoreCreateMutex();
  return g_fastStatusMutex != nullptr;
}

void initFastStatusCache(){
  if(!ensureFastStatusMutex()) {
    Serial.println("[STATUS] Cache mutex init failed.");
  }
}

void serviceFastStatusCache(){
  if(!ensureFastStatusMutex()) return;
  const unsigned long now=millis();
  bool fresh=false;
  if(xSemaphoreTake(g_fastStatusMutex,0)==pdTRUE){
    fresh=(g_fastStatusJson.length()>0 && now-g_fastStatusAtMs<750UL);
    xSemaphoreGive(g_fastStatusMutex);
  }
  if(fresh) return;

  String next=buildStatusJson();
  if(xSemaphoreTake(g_fastStatusMutex,pdMS_TO_TICKS(20))==pdTRUE){
    g_fastStatusJson=next;
    g_fastStatusAtMs=now;
    xSemaphoreGive(g_fastStatusMutex);
  }
}

String fastStatusJsonCopy(){
  if(!ensureFastStatusMutex()) return buildStatusJson();
  if(xSemaphoreTake(g_fastStatusMutex,pdMS_TO_TICKS(20))==pdTRUE){
    String out=g_fastStatusJson;
    xSemaphoreGive(g_fastStatusMutex);
    if(out.length()) return out;
  }
  return buildStatusJson();
}

String buildStatusJson() {
  String lastJson = "null";
  int locEntryCount, locExitCount, locLastEntryId, locPending, locPendingDropped, locRamDropped;
  uint16_t locTemplates, locNextId; bool locEnrolling, locSdAvailable; uint32_t locSdGen, locSeq;
  String locDeviceName;
  int locCachedLastId=-1;
  String locCachedLastJson="null";
  bool locFpAsleep=false, locFpOk=false;

  if (lockState()) {
    if (logCount > 0) {
      int idx = (logWriteIndex - 1 + MAX_LOGS) % MAX_LOGS;
      AttendanceLog &l = logs[idx];
      lastJson = "{";
      lastJson += "\"time\":\"" + jsonEscape(l.timeText) + "\",";
      lastJson += "\"event\":\"" + jsonEscape(l.event) + "\",";
      lastJson += "\"id\":" + String(l.id) + ",";
      lastJson += "\"name\":\"" + jsonEscape(l.name) + "\",";
      lastJson += "\"collegeId\":\"" + jsonEscape(l.collegeId) + "\",";
      lastJson += "\"role\":\"" + jsonEscape(l.role) + "\",";
      lastJson += "\"department\":\"" + jsonEscape(l.department) + "\",";
      lastJson += "\"message\":\"" + jsonEscape(l.message) + "\"";
      lastJson += "}";
    }
    locEntryCount = entryCount; locExitCount = exitCount; locLastEntryId = lastEntryId;
    locTemplates = cachedTemplateCount; locNextId = nextEnrollID;
    locCachedLastId = cachedLastEntryJsonForId; locCachedLastJson = cachedLastEntryJson;
    locEnrolling = isEnrolling; locSdAvailable = sdAvailable; locSdGen = sdGeneration;
    locPending = pendingUsed; locPendingDropped = pendingDropped + pendingQueueDropped;
    locRamDropped = ramDropped; locSeq = logSeqCounter; locDeviceName = cfgDeviceName;
    locFpAsleep = fpAsleep;
    locFpOk = fingerprintSensorOk;
    unlockState();
  } else {
    locEntryCount = locExitCount = locLastEntryId = 0;
    locTemplates = locNextId = 0; locEnrolling = locSdAvailable = false;
    locSdGen = locPending = locPendingDropped = locRamDropped = locSeq = 0;
    locDeviceName = "MBS BA MVP";
  }

  // PERF: copy the producer-maintained last-entry cache while under the
  // state mutex, so async handlers never observe a partially updated String.
  String lastEntryJson = "null";
  if (locLastEntryId >= 1 && locLastEntryId == locCachedLastId && locCachedLastJson.length() > 4) {
    lastEntryJson = locCachedLastJson;
  }

  bool wifiOk = wifiDashboardReady();

  String json; json.reserve(1500);
  json += "{\n";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"wifi\":" + String(wifiOk ? "true" : "false") + ",";
  json += "\"ip\":\"" + String(wifiOk ? wifiDashboardIP().toString() : "") + "\",";
  json += "\"rssi\":" + String(wifiOk ? (int)WiFi.RSSI() : 0) + ",";
  json += "\"templates\":" + String(locTemplates) + ",";
  json += "\"capacity\":" + String(MAX_FINGERPRINTS) + ",";
  json += "\"nextId\":" + String(locNextId) + ",";
  json += "\"databaseFull\":" + String(locNextId == 0 ? "true" : "false") + ",";
  json += "\"logCount\":" + String(locSeq) + ",";
  json += "\"sdRecords\":" + String(sdRecordCount) + ",";
  json += "\"sdLessons\":" + String(sdLessonCount) + ",";
  json += "\"pendingCount\":" + String(locPending) + ",";
  json += "\"pendingDropped\":" + String(locPendingDropped) + ",";
  json += "\"ramDropped\":" + String(locRamDropped) + ",";
  json += "\"entryCount\":" + String(locEntryCount) + ",";
  json += "\"exitCount\":" + String(locExitCount) + ",";
  json += "\"isEnrolling\":" + String(locEnrolling ? "true" : "false") + ",";
  json += "\"sensor\":\"" + String(locFpOk ? "Ready" : "Not Found") + "\",";
  json += "\"fingerprintOk\":" + String(locFpOk ? "true" : "false") + ",";
  json += "\"fingerprintAsleep\":" + String(locFpAsleep ? "true" : "false") + ",";
  json += "\"sdAvailable\":" + String(locSdAvailable ? "true" : "false") + ",";
  json += "\"sdGen\":" + String(locSdGen) + ",";
  json += "\"rtcAvailable\":" + String(rtcAvailable ? "true" : "false") + ",";
  json += "\"rtcTimeValid\":" + String(rtcTimeValid ? "true" : "false") + ",";
  json += "\"currentTime\":\"" + jsonEscape(currentDateTimeText()) + "\",";
  json += "\"deviceName\":\"" + jsonEscape(locDeviceName) + "\",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"last\":" + lastJson + ",";
  json += "\"lastEntry\":" + lastEntryJson;
  json += "}";
  return json;
}

String buildSystemJson() {
  String nowText = currentDateTimeText();
  int locEntryCount, locExitCount, locPending, locPendingDropped, locRamDropped;
  uint16_t locTemplates, locNextId; int locSecLevel; String locTz, locDeviceName;
  int locCheckin, locCheckout; bool locSdAvailable; uint32_t locSdGen;
  bool locFpAsleep=false, locFpOk=false;

  if (lockState()) {
    locEntryCount = entryCount; locExitCount = exitCount;
    locTemplates = cachedTemplateCount; locNextId = nextEnrollID;
    locSecLevel = cfgSecurityLevel; locTz = cfgTimezone; locDeviceName = cfgDeviceName;
    locCheckin = cfgCheckinMinutes; locCheckout = cfgCheckoutMinutes;
    locSdAvailable = sdAvailable; locSdGen = sdGeneration;
    locPending = pendingUsed; locPendingDropped = pendingDropped + pendingQueueDropped;
    locRamDropped = ramDropped;
    locFpAsleep = fpAsleep;
    locFpOk = fingerprintSensorOk;
    unlockState();
  } else {
    locEntryCount = locExitCount = 0; locTemplates = locNextId = 0;
    locSecLevel = MATCH_SECURITY_LEVEL; locTz = TIME_ZONE; locDeviceName = "MBS BA MVP";
    locCheckin = CHECKIN_ON_TIME_MINUTES; locCheckout = CHECKOUT_ON_TIME_MINUTES;
    locSdAvailable = false; locSdGen = 0; locPending = locPendingDropped = locRamDropped = 0;
  }

  String json; json.reserve(1500);
  json += "{";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"ok\":true,";
  json += "\"deviceName\":\"" + jsonEscape(locDeviceName) + "\",";
  json += "\"wifi\":" + String(wifiDashboardReady() ? "true" : "false") + ",";
  json += "\"ip\":\"" + String(wifiDashboardReady() ? wifiDashboardIP().toString() : "") + "\",";
  json += "\"fingerprintOk\":" + String(locFpOk ? "true" : "false") + ",";
  json += "\"fingerprintAsleep\":" + String(locFpAsleep ? "true" : "false") + ",";
  json += "\"templates\":" + String(locTemplates) + ",";
  json += "\"capacity\":" + String(MAX_FINGERPRINTS) + ",";
  json += "\"sdAvailable\":" + String(locSdAvailable ? "true" : "false") + ",";
  json += "\"sdGen\":" + String(locSdGen) + ",";
  json += "\"rtcAvailable\":" + String(rtcAvailable ? "true" : "false") + ",";
  json += "\"rtcTimeValid\":" + String(rtcTimeValid ? "true" : "false") + ",";
  json += "\"ntpSynced\":" + String(ntpSynced ? "true" : "false") + ",";
  json += "\"currentTime\":\"" + jsonEscape(nowText) + "\",";
  json += "\"nextId\":" + String(locNextId) + ",";
  json += "\"databaseFull\":" + String(locNextId == 0 ? "true" : "false") + ",";
  json += "\"entryCount\":" + String(locEntryCount) + ",";
  json += "\"exitCount\":" + String(locExitCount) + ",";
  json += "\"securityLevel\":" + String(locSecLevel) + ",";
  json += "\"timezone\":\"" + jsonEscape(locTz) + "\",";
  json += "\"checkinTime\":\"" + minutesToHHMM(locCheckin) + "\",";
  json += "\"checkoutTime\":\"" + minutesToHHMM(locCheckout) + "\",";
  json += "\"logRecords\":" + String(sdRecordCount) + ",";
  json += "\"lessonRecords\":" + String(sdLessonCount) + ",";
  json += "\"pendingRecords\":" + String(locPending) + ",";
  json += "\"pendingDropped\":" + String(locPendingDropped) + ",";
  json += "\"ramDropped\":" + String(locRamDropped) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap());

  if (sdStatValid) {
    uint64_t sdFree = (sdTotalBytesCache > sdUsedBytesCache) ? (sdTotalBytesCache - sdUsedBytesCache) : 0;
    json += ",\"sdTotalBytes\":" + String((unsigned long long)sdTotalBytesCache);
    json += ",\"sdUsedBytes\":"  + String((unsigned long long)sdUsedBytesCache);
    json += ",\"sdFreeBytes\":"  + String((unsigned long long)sdFree);
    json += ",\"csvSizeBytes\":" + String((unsigned long long)sdCsvBytesCache);
    json += ",\"lessonsSizeBytes\":" + String((unsigned long long)sdLessonsBytesCache);
  }
  json += "}";
  return json;
}

String buildSetupJson() {
  int locCheckin, locCheckout, locSec; String locName, locTz;
  if (lockState()) {
    locCheckin = cfgCheckinMinutes; locCheckout = cfgCheckoutMinutes;
    locName = cfgDeviceName; locTz = cfgTimezone; locSec = cfgSecurityLevel;
    unlockState();
  } else {
    locCheckin = CHECKIN_ON_TIME_MINUTES; locCheckout = CHECKOUT_ON_TIME_MINUTES;
    locName = "MBS BA MVP"; locTz = TIME_ZONE; locSec = MATCH_SECURITY_LEVEL;
  }
  String json = "{"; json.reserve(350);
  json += "\"ok\":true,";
  json += "\"checkinTime\":\"" + minutesToHHMM(locCheckin) + "\",";
  json += "\"checkoutTime\":\"" + minutesToHHMM(locCheckout) + "\",";
  json += "\"deviceName\":\"" + jsonEscape(locName) + "\",";
  json += "\"timezone\":\"" + jsonEscape(locTz) + "\",";
  json += "\"securityLevel\":" + String(locSec);
  json += "}";
  return json;
}



String buildRamLogsJson(int limit,const char* dateFilter) {
  static AttendanceLog snapshot[MAX_LOGS];
  int n=0;

  // Keep the large snapshot buffer off the HTTP callback stack. Only one
  // builder may use it at a time; the protected copy is never exposed to
  // another task while it is being serialized.
  if(!ramLogSnapshotMutex) return "[]";
  if(xSemaphoreTake(ramLogSnapshotMutex,pdMS_TO_TICKS(500))!=pdTRUE) return "[]";

  if(lockState(300)) {
    int want=(limit>0 && limit<logCount)?limit:logCount;
    int start=logCount-want; if(start<0) start=0;
    for(int i=start;i<logCount;i++){
      int idx=(logWriteIndex-logCount+i+MAX_LOGS)%MAX_LOGS;
      if(dateFilter && dateFilter[0] && !rowMatchesDate(logs[idx].timeText,dateFilter)) continue;
      snapshot[n++]=logs[idx];
    }
    unlockState();
  }

  String out; out.reserve(n*220+4); out="[";
  for(int i=0;i<n;i++){
    AttendanceLog &l=snapshot[i]; if(i) out+=",";
    out+="{\"time\":\""+jsonEscape(l.timeText)+"\",\"event\":\""+jsonEscape(l.event)+"\",\"id\":"+String(l.id)+",\"name\":\""+jsonEscape(l.name)+"\",\"collegeId\":\""+jsonEscape(l.collegeId)+"\",\"role\":\""+jsonEscape(l.role)+"\",\"department\":\""+jsonEscape(l.department)+"\",\"message\":\""+jsonEscape(l.message)+"\",\"pending\":"+String(l.savedToSd?"false":"true")+"}";
  }
  out+="]";
  if(ramLogSnapshotMutex) xSemaphoreGive(ramLogSnapshotMutex);
  return out;
}

namespace {
struct BootSnapshotSlot {
  String status, system, setup, registered, json;
};
BootSnapshotSlot g_bootSnapshot;
SemaphoreHandle_t g_bootSnapshotMutex=nullptr;
}

static bool ensureBootSnapshotMutex(){
  return g_bootSnapshotMutex!=nullptr;
}

void initBootSnapshotCache(){
  if(!g_bootSnapshotMutex) g_bootSnapshotMutex=xSemaphoreCreateMutex();
}

void refreshBootSnapshot() {
  String s=buildStatusJson(), y=buildSystemJson(), u=buildSetupJson(), r="[]";
  if(lockState()) { r=cachedRegisteredJson; unlockState(); }
  String j="{\"ok\":true,\"booting\":"+String(dashboardBootReady?"false":"true")+",\"status\":"+s+",\"system\":"+y+",\"setup\":"+u+",\"registered\":"+r+",\"logs\":[]}";

  if(!ensureBootSnapshotMutex()) return;
  if(xSemaphoreTake(g_bootSnapshotMutex,pdMS_TO_TICKS(500))==pdTRUE){
    g_bootSnapshot.status=s;
    g_bootSnapshot.system=y;
    g_bootSnapshot.setup=u;
    g_bootSnapshot.registered=r;
    g_bootSnapshot.json=j;
    xSemaphoreGive(g_bootSnapshotMutex);
  }
}

String bootSnapshotCopy(){
  const String fallback=String("{\"ok\":true,\"booting\":true,\"status\":{},\"system\":{},\"setup\":{},\"registered\":[],\"logs\":[]}");
  if(!ensureBootSnapshotMutex()) return fallback;
  String out;
  if(xSemaphoreTake(g_bootSnapshotMutex,pdMS_TO_TICKS(500))==pdTRUE){
    out=g_bootSnapshot.json;
    xSemaphoreGive(g_bootSnapshotMutex);
  }
  return out.length()?out:fallback;
}


void serviceBootSnapshotRefresh() {
  static unsigned long lastRefreshMs = 0;
  if (!bootSnapshotDirty) return;
  const unsigned long now = millis();
  if (now - lastRefreshMs < 250UL) return;
  bootSnapshotDirty = false;
  lastRefreshMs = now;
  refreshBootSnapshot();
}