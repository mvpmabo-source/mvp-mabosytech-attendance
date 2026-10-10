#include "app.h"
#include <mbedtls/sha256.h>

bool hasParam(AsyncWebServerRequest* request,const char* name,bool post){
  return request && request->hasParam(name,post,false);
}

String getParamValue(AsyncWebServerRequest* request,const char* name,bool post){
  if(!request) return String();
  const AsyncWebParameter* p=request->getParam(name,post,false);
  if(p) return p->value();
  return String();
}

static void sendJson(AsyncWebServerRequest* request,int code,const String& body,bool noStore=true){
  if(!request) return;
  AsyncWebServerResponse* response=request->beginResponse(code,"application/json",body);
  if(!response){request->send(500,"text/plain","Response allocation failed");return;}
  // Keep the response compatible with the installed ESPAsyncWebServer API.
  // Do not manipulate the Connection header here; the library owns it.
  if(noStore) response->addHeader("Cache-Control","no-store");
  response->addHeader("X-Content-Type-Options","nosniff");
  response->addHeader("X-Frame-Options","DENY");
  response->addHeader("Referrer-Policy","no-referrer");
  request->send(response);
}

// ---------------- SD -> async HTTP bridge ----------------
struct SdHttpStream {
  bool active=false;
  uint32_t generation=0;
  uint32_t reqId=0;
  uint8_t requestType=0;
  QueueHandle_t queue=nullptr;
  String prefix;
  String suffix;
  size_t prefixOffset=0;
  size_t suffixOffset=0;
  SdBulkChunk current{};
  size_t currentOffset=0;
  bool haveCurrent=false;
  bool sawDone=false;
  bool ok=false;
  AsyncWebServerResponse* response=nullptr;
  unsigned long lastActivityMs=0;
};

static SdHttpStream g_sdStreams[SD_HTTP_STREAM_SLOTS];
static portMUX_TYPE g_streamAllocMux=portMUX_INITIALIZER_UNLOCKED;

bool sdHttpStreamMatches(uint32_t id, QueueHandle_t queue){
  bool found=false;
  portENTER_CRITICAL(&g_streamAllocMux);
  for(int i=0;i<SD_HTTP_STREAM_SLOTS;i++){
    const SdHttpStream &st=g_sdStreams[i];
    if(st.active && st.reqId==id && st.queue==queue){ found=true; break; }
  }
  portEXIT_CRITICAL(&g_streamAllocMux);
  return found;
}

bool initSdHttpBridge(){
  for(int i=0;i<SD_HTTP_STREAM_SLOTS;i++){
    if(!g_sdStreams[i].queue){
      g_sdStreams[i].queue=xQueueCreate(SD_HTTP_STREAM_QUEUE_DEPTH,sizeof(SdBulkChunk));
      if(!g_sdStreams[i].queue) return false;
    }
  }
  return true;
}

static void resetStreamState(SdHttpStream &s){
  s.reqId=0; s.requestType=0; s.prefix=""; s.suffix="";
  s.prefixOffset=0; s.suffixOffset=0; s.currentOffset=0;
  s.haveCurrent=false; s.sawDone=false; s.ok=false;
  s.response=nullptr; s.lastActivityMs=millis();
}

static bool claimStream(int &index,uint32_t &generation){
  index=-1; generation=0;
  portENTER_CRITICAL(&g_streamAllocMux);
  for(int i=0;i<SD_HTTP_STREAM_SLOTS;i++){
    if(!g_sdStreams[i].active){
      g_sdStreams[i].generation++;
      g_sdStreams[i].active=true;
      index=i; generation=g_sdStreams[i].generation;
      break;
    }
  }
  portEXIT_CRITICAL(&g_streamAllocMux);
  if(index<0) return false;

  SdBulkChunk stale;
  while(xQueueReceive(g_sdStreams[index].queue,&stale,0)==pdTRUE){}
  resetStreamState(g_sdStreams[index]);
  return true;
}

static void releaseStream(SdHttpStream &s,uint32_t generation){
  portENTER_CRITICAL(&g_streamAllocMux);
  if(s.generation==generation){
    s.reqId=0;
    s.requestType=0;
    s.active=false;
  }
  portEXIT_CRITICAL(&g_streamAllocMux);
}

static size_t sdHttpFill(SdHttpStream* s,uint8_t* buffer,size_t maxLen,uint32_t generation){
  if(!s || maxLen==0) return 0;
  bool active=false;
  portENTER_CRITICAL(&g_streamAllocMux);
  active=(s->active && s->generation==generation);
  portEXIT_CRITICAL(&g_streamAllocMux);
  if(!active) return 0;

  if(s->prefixOffset<s->prefix.length()){
    size_t n=s->prefix.length()-s->prefixOffset; if(n>maxLen) n=maxLen;
    memcpy(buffer,s->prefix.c_str()+s->prefixOffset,n);
    s->prefixOffset+=n;
    s->lastActivityMs=millis();
    return n;
  }

  while(maxLen>0){
    if(s->haveCurrent){
      size_t avail=s->current.len-s->currentOffset;
      size_t n=avail; if(n>maxLen) n=maxLen;
      memcpy(buffer,s->current.data+s->currentOffset,n);
      s->currentOffset+=n; buffer+=n; maxLen-=n;
      if(n>0) s->lastActivityMs=millis();
      if(s->currentOffset>=s->current.len){
        if(s->current.done){
          s->sawDone=true; s->ok=s->current.ok;
          if(s->requestType==3 && s->suffix.length()==0){
            s->suffix=s->ok ? "{\"success\":true,\"message\":\"Attendance data deleted\"}"
                            : "{\"success\":false,\"message\":\"Delete failed\"}";
          }
        }
        s->haveCurrent=false; s->currentOffset=0;
      }
      if(maxLen==0) break;
      continue;
    }

    if(s->sawDone){
      if(s->suffixOffset<s->suffix.length()){
        size_t n=s->suffix.length()-s->suffixOffset; if(n>maxLen) n=maxLen;
        memcpy(buffer,s->suffix.c_str()+s->suffixOffset,n);
        s->suffixOffset+=n; s->lastActivityMs=millis(); buffer+=n; maxLen-=n;
        if(maxLen==0) break;
        continue;
      }
      releaseStream(*s,generation);
      return 0;
    }

    if(xQueueReceive(s->queue,&s->current,0)==pdTRUE){
      s->haveCurrent=true; s->currentOffset=0;
      if(s->current.len==0 && s->current.done){
        s->sawDone=true; s->ok=s->current.ok;
        if(s->requestType==3 && s->suffix.length()==0){
          s->suffix=s->ok ? "{\"success\":true,\"message\":\"Attendance data deleted\"}"
                          : "{\"success\":false,\"message\":\"Delete failed\"}";
        }
        s->haveCurrent=false;
      } else if(s->current.len==0){
        s->haveCurrent=false;
      }
      continue;
    }
    if(millis()-s->lastActivityMs>SD_HTTP_STREAM_IDLE_MS){
      releaseStream(*s,generation);
      return 0;
    }
    return (size_t)RESPONSE_TRY_AGAIN;
  }
  return (size_t)RESPONSE_TRY_AGAIN;
}

enum SdStreamStartResult { SD_STREAM_STARTED, SD_STREAM_QUEUE_FULL, SD_STREAM_NO_SLOT };

static SdStreamStartResult beginSdStreamWithReq(AsyncWebServerRequest* request,SdBulkRequest req,const String& prefix,const String& suffix,const String& contentType,const char* disposition=nullptr){
  if(!request) return SD_STREAM_NO_SLOT;
  if(!sdAvailable && req.type!=3) return SD_STREAM_NO_SLOT;
  if(!initSdHttpBridge()) return SD_STREAM_NO_SLOT;

  int freeIndex=-1; uint32_t generation=0;
  if(!claimStream(freeIndex,generation)) return SD_STREAM_NO_SLOT;

  SdHttpStream &s=g_sdStreams[freeIndex];
  s.reqId=req.id; s.requestType=req.type; s.prefix=prefix; s.suffix=suffix;
  req.replyQueue=s.queue;

  SdHttpStream* stream=&s;
  AsyncWebServerResponse* response=request->beginChunkedResponse(contentType.c_str(),[stream,generation](uint8_t* buffer,size_t maxLen,size_t)->size_t{
    return sdHttpFill(stream,buffer,maxLen,generation);
  });
  if(!response){releaseStream(s,generation);return SD_STREAM_NO_SLOT;}
  if(disposition) response->addHeader("Content-Disposition",disposition);
  response->addHeader("Cache-Control","no-store");
  s.response=response;
  request->onDisconnect([stream,generation](){releaseStream(*stream,generation);});

  if(xQueueSend(sdRequestQueue,&req,0)!=pdTRUE){
    s.response=nullptr;
    delete response;
    releaseStream(s,generation);
    return SD_STREAM_QUEUE_FULL;
  }
  request->send(response);
  return SD_STREAM_STARTED;
}

static SdStreamStartResult enqueueSdStream(AsyncWebServerRequest* request,uint8_t type,const String& prefix,const String& suffix,const String& contentType="application/json",const char* disposition=nullptr){
  SdBulkRequest req=makeBulkReq(nextBulkId(),type);
  return beginSdStreamWithReq(request,req,prefix,suffix,contentType,disposition);
}

// ---------------- API routes ----------------
void registerApiRoutes(){
  server.on("/api/setup",HTTP_POST,[](AsyncWebServerRequest* request){
    if(accountExists()){sendJson(request,409,"{\"ok\":false,\"message\":\"An account already exists\"}");return;}
    String username=getParamValue(request,"username",true), password=getParamValue(request,"password",true); username.trim();
    if(username.length()<3||username.length()>32){sendJson(request,400,"{\"ok\":false,\"message\":\"Username must be 3-32 characters\"}");return;}
    if(password.length()<6||password.length()>64){sendJson(request,400,"{\"ok\":false,\"message\":\"Password must be 6-64 characters\"}");return;}
    String record=createPasswordRecord(password);
    if(record.length()<1){sendJson(request,500,"{\"ok\":false,\"message\":\"Could not secure password\"}");return;}
    if(!prefsPutString(AUTH_USERNAME_KEY,username) || !prefsPutString(AUTH_PASSWORD_KEY,record)){
      sendJson(request,500,"{\"ok\":false,\"message\":\"Could not save account securely\"}");return;
    }
    if(lockState()){authUsername=username;authPasswordHash=record;unlockState();}
    clearAllAuthSessions();
    sendJson(request,200,"{\"ok\":true,\"message\":\"Account created\"}");
  });

  server.on("/api/login",HTTP_POST,[](AsyncWebServerRequest* request){
    Serial.printf("[HTTP] POST /api/login heap=%u\n",(unsigned)ESP.getFreeHeap());
    if(!accountExists()){sendJson(request,400,"{\"ok\":false,\"message\":\"Create an account first\"}");return;}
    const unsigned long nowMs = millis();
    bool loginBlocked = false;
    unsigned long blockedUntil = 0;
    if (lockState(20)) {
      blockedUntil = loginBlockedUntilMs;
      loginBlocked = blockedUntil && (long)(nowMs - blockedUntil) < 0;
      unlockState();
    }
    if (loginBlocked) {
      const unsigned long remainMs = blockedUntil - nowMs;
      sendJson(request,429,String("{\"ok\":false,\"message\":\"Too many failed attempts. Try again in ")+String((remainMs+999UL)/1000UL)+" seconds.\"}");
      return;
    }

    String username=getParamValue(request,"username",true), password=getParamValue(request,"password",true); username.trim();
    String expectedUser,expectedRecord;
    if(lockState()){expectedUser=authUsername;expectedRecord=authPasswordHash;unlockState();}
    bool legacyRecord=false;
    const bool usernameOk = (username==expectedUser);
    const bool passwordVerified = verifyPasswordRecord(password,expectedRecord,&legacyRecord);
    const bool passwordOk = usernameOk && passwordVerified;
    if(!passwordOk){
      Serial.printf("[AUTH] Login rejected: userMatch=%s recordLen=%u recordFormat=%s\n",
                    usernameOk ? "YES" : "NO",
                    (unsigned)expectedRecord.length(),
                    expectedRecord.startsWith("v1$") ? "v1-salted" : "legacy/invalid");
      bool blockNow = false;
      if (lockState(20)) {
        loginFailedAttempts = loginFailedAttempts < 255 ? (uint8_t)(loginFailedAttempts + 1) : 255;
        if (loginFailedAttempts >= 5) {
          loginBlockedUntilMs = millis() + 30000UL;
          loginFailedAttempts = 0;
          blockNow = true;
        }
        unlockState();
      }
      if (blockNow) {
        sendJson(request,429,"{\"ok\":false,\"message\":\"Too many failed attempts. Try again in 30 seconds.\"}");
      } else {
        sendJson(request,401,"{\"ok\":false,\"message\":\"Invalid username or password\"}");
      }
      return;
    }
    if (lockState(20)) {
      loginFailedAttempts=0;
      loginBlockedUntilMs=0;
      unlockState();
    }
    if(legacyRecord && !migrateLegacyPasswordRecordIfValid(password,expectedRecord)){
      sendJson(request,500,"{\"ok\":false,\"message\":\"Password migration failed; login aborted\"}");
      return;
    }
    String token=generateSessionToken();
    if(allocateAuthSession(token)<0){
      sendJson(request,503,"{\"ok\":false,\"message\":\"Authentication service busy; try again\"}");
      return;
    }
    AsyncWebServerResponse* response=request->beginResponse(200,"application/json","{\"ok\":true,\"message\":\"Login successful\"}");
    if(!response){
      removeAuthSessionForToken(token);
      sendJson(request,500,"{\"ok\":false,\"message\":\"Response allocation failed\"}");
      return;
    }
    setLoginCookie(response, token); request->send(response);
  });

  server.on("/api/logout",HTTP_POST,[](AsyncWebServerRequest* request){
    AsyncWebServerResponse* response=request->beginResponse(200,"application/json","{\"ok\":true,\"message\":\"Logged out\"}");
    if(!response){request->send(500,"text/plain","Logout response allocation failed");return;}
    clearLoginCookieForRequest(request,response);
    request->send(response);
  });

  server.on("/api/auth/status",HTTP_GET,[](AsyncWebServerRequest* request){
    bool acct=accountExists();
    bool authed=isAuthenticated(request);
    String user;
    if(authed && lockState(100)){ user=authUsername; unlockState(); }
    String json="{\"accountExists\":"+String(acct?"true":"false")+",\"authenticated\":"+String(authed?"true":"false");
    if(authed) json+=",\"username\":\""+jsonEscape(user)+"\"";
    json+="}";
    sendJson(request,200,json);
  });

  // The dashboard supplies a current UTC Unix timestamp after authentication.
  // Keeping this write route authenticated prevents unauthenticated clients
  // from repeatedly changing the device clock.
  server.on("/api/time/sync",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    Serial.printf("[HTTP] POST /api/time/sync heap=%u\n",(unsigned)ESP.getFreeHeap());
    if(!hasParam(request,"epoch",true)){
      sendJson(request,400,"{\"ok\":false,\"message\":\"Missing epoch\"}");
      return;
    }
    String raw=getParamValue(request,"epoch",true); raw.trim();
    if(raw.length()<10 || raw.length()>11){
      sendJson(request,400,"{\"ok\":false,\"message\":\"Invalid epoch\"}");
      return;
    }
    char* endp=nullptr;
    unsigned long epoch=strtoul(raw.c_str(),&endp,10);
    if(endp==raw.c_str() || *endp!='\0' || !syncRTCFromBrowserEpoch((uint32_t)epoch)){
      sendJson(request,400,"{\"ok\":false,\"message\":\"RTC time synchronization failed\"}");
      return;
    }
    // Important: this callback never touches Wire/DS3231, settimeofday(), or
    // Preferences. It only places the validated epoch into a length-1 queue.
    // loop() consumes that request and performs the real clock transaction.
    sendJson(request,202,"{\"ok\":true,\"queued\":true,\"timezone\":\"Rwanda CAT UTC+2\"}");
  });

  server.on("/api/time/status",HTTP_GET,[](AsyncWebServerRequest* request){
    String now=currentDateTimeText();
    String json=String("{\"ok\":true,\"rtcAvailable\":")+(rtcAvailable?"true":"false")+
                ",\"rtcTimeValid\":"+(rtcTimeValid?"true":"false")+
                ",\"timezone\":\"Rwanda CAT UTC+2\",\"time\":\""+jsonEscape(now)+"\"}";
    sendJson(request,200,json);
  });

  server.on("/api/bootstrap",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!accountExists()){ sendJson(request,503,"{\"ok\":false}"); return; }
    if(!isAuthenticated(request)){ sendJson(request,401,"{\"ok\":false}"); return; }
    if(!dashboardBootReady){ sendJson(request,503,bootSnapshotCopy()); return; }
    sendJson(request,200,bootSnapshotCopy());
  });

  server.on("/api/status",HTTP_GET,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}sendJson(request,200,fastStatusJsonCopy());});
  server.on("/api/system",HTTP_GET,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}sendJson(request,200,buildSystemJson());});
  server.on("/api/device-setup",HTTP_GET,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}sendJson(request,200,buildSetupJson());});

  server.on("/api/logs",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    int limit=80; if(hasParam(request,"limit")) limit=getParamValue(request,"limit").toInt();
    if(limit>0){sendJson(request,200,"{\"logs\":"+buildRamLogsJson(limit,nullptr)+"}");return;}
    if(enqueueSdStream(request,2,"{\"logs\":[","]}","application/json")!=SD_STREAM_STARTED) sendJson(request,503,"{\"ok\":false,\"message\":\"SD task busy, try again\"}");
  });

  auto historyHandler=[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String dateParam=hasParam(request,"date")?getParamValue(request,"date"):String(""); dateParam.trim();
    if(!isValidDateStr(dateParam)) {
      String today=currentDateOnlyText();
      if(!isValidDateStr(today)) { sendJson(request,503,"{\"ok\":false,\"message\":\"Device date is not ready\"}"); return; }
      dateParam=today;
    }
    if(!sdAvailable) { sendJson(request,503,"{\"ok\":false,\"message\":\"SD card is not available\"}"); return; }
    SdBulkRequest req=makeBulkReq(nextBulkId(),2,dateParam.c_str());
    if(beginSdStreamWithReq(request,req,"{\"date\":\""+jsonEscape(dateParam)+"\",\"logs\":[","]}","application/json")!=SD_STREAM_STARTED)
      sendJson(request,503,"{\"ok\":false,\"message\":\"SD task busy, try again\"}");
  };
  server.on("/api/history",HTTP_GET,historyHandler);
  server.on("/api/report",HTTP_GET,historyHandler);

  server.on("/api/teachers",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String reg="[]";if(lockState()){reg=cachedRegisteredJson;unlockState();}sendJson(request,200,reg);
  });

  server.on("/api/lessons",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String date=hasParam(request,"date")?getParamValue(request,"date"):String("");
    date.trim();
    if(!isValidDateStr(date)) date=currentDateOnlyText();
    if(!isValidDateStr(date)){
      sendJson(request,503,"{\"ok\":false,\"message\":\"Device date is not ready\"}");
      return;
    }
    SdBulkRequest req=makeBulkReq(nextBulkId(),5,date.c_str());
    String prefix="{\"date\":\""+jsonEscape(date)+"\",\"lessons\":[";
    if(beginSdStreamWithReq(request,req,prefix,"]}","application/json")!=SD_STREAM_STARTED) sendJson(request,503,"{\"ok\":false,\"message\":\"SD task busy, try again\"}");
  });

  server.on("/api/lesson-report",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String from=hasParam(request,"from")?getParamValue(request,"from"):String(""); String to=hasParam(request,"to")?getParamValue(request,"to"):String(""); from.trim();to.trim();
    if(!isValidDateStr(from))from="";if(!isValidDateStr(to))to="";
    SdBulkRequest req=makeBulkReq(nextBulkId(),6,from.c_str(),to.c_str());
    String prefix="{\"from\":\""+jsonEscape(from)+"\",\"to\":\""+jsonEscape(to)+"\",\"lessons\":[";
    if(beginSdStreamWithReq(request,req,prefix,"]}","application/json")!=SD_STREAM_STARTED) sendJson(request,503,"{\"ok\":false,\"message\":\"SD task busy, try again\"}");
  });

  server.on("/api/lessons",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String date=getParamValue(request,"date",true), time=getParamValue(request,"time",true), idStr=getParamValue(request,"fingerprintId",true), type=getParamValue(request,"type",true), lesson=getParamValue(request,"lesson",true);
    date.trim();time.trim();idStr.trim();type.trim();
    if(!isValidDateStr(date)){sendJson(request,400,"{\"ok\":false,\"error\":\"Valid date is required\"}");return;}
    if(time.length()<5){sendJson(request,400,"{\"ok\":false,\"error\":\"Valid time is required\"}");return;}
    int id=idStr.toInt();if(id<1||id>MAX_FINGERPRINTS){sendJson(request,400,"{\"ok\":false,\"error\":\"Invalid teacher ID\"}");return;}
    if(type.length()<2||type.length()>22){sendJson(request,400,"{\"ok\":false,\"error\":\"Invalid type\"}");return;}
    if(lesson.length()<1||lesson.length()>250){sendJson(request,400,"{\"ok\":false,\"error\":\"Reason is required (max 250 chars)\"}");return;}
    if(!lessonAppendQueue){sendJson(request,503,"{\"ok\":false,\"error\":\"Reason storage queue is unavailable\"}");return;}
    PersonProfile person=loadProfile((uint16_t)id);LessonAppend la{};
    strncpy(la.date,date.c_str(),sizeof(la.date)-1);strncpy(la.time,time.c_str(),sizeof(la.time)-1);la.fingerprintId=id;
    strncpy(la.name,person.name.c_str(),sizeof(la.name)-1);strncpy(la.collegeId,person.collegeId.c_str(),sizeof(la.collegeId)-1);strncpy(la.role,person.role.c_str(),sizeof(la.role)-1);strncpy(la.department,person.department.c_str(),sizeof(la.department)-1);strncpy(la.type,type.c_str(),sizeof(la.type)-1);strncpy(la.lesson,lesson.c_str(),sizeof(la.lesson)-1);

    // Persist the complete reason record in NVS before acknowledging the
    // browser. SD is the authoritative long-term file, but this journal is
    // the power-loss/reboot safety net while the SD write is pending.
    const int journalSlot = reserveLessonJournal(la);
    if(journalSlot < 0){
      sendJson(request,503,"{\"ok\":false,\"error\":\"Reason queue is full; retry shortly\"}");
      return;
    }

    // Prefer immediate queueing. If the runtime queue is temporarily full,
    // keep the NVS journal and let sdTask recover it later; the record is
    // already durable at this point, so the browser does not need to retry.
    if(xQueueSend(lessonAppendQueue,&la,0)==pdTRUE){
      sendJson(request,202,sdAvailable
        ? "{\"ok\":true,\"queued\":true,\"durable\":true,\"message\":\"Reason accepted and queued for SD save.\"}"
        : "{\"ok\":true,\"queued\":true,\"durable\":true,\"message\":\"Reason stored safely; SD is offline and the record will be saved when the card returns.\"}");
    } else {
      sendJson(request,202,"{\"ok\":true,\"queued\":false,\"durable\":true,\"message\":\"Reason stored safely and will be written to SD automatically.\"}");
    }
  });

  server.on("/api/tick",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    long since=-1;if(hasParam(request,"since"))since=getParamValue(request,"since").toInt();int limit=80;if(hasParam(request,"limit"))limit=getParamValue(request,"limit").toInt();if(limit<=0)limit=80;
    uint32_t currentSeq=0;int locRamDropped=0;bool needResync=false;
    if(lockState()){currentSeq=logSeqCounter;locRamDropped=ramDropped;needResync=(tickLastRamDroppedSeen>=0&&locRamDropped!=tickLastRamDroppedSeen);tickLastRamDroppedSeen=locRamDropped;unlockState();}
    bool logsChanged=(since<0)||(uint32_t)since!=currentSeq;
    String json="{\"status\":"+fastStatusJsonCopy();
    if(logsChanged)json+=",\"logsIncluded\":true,\"logs\":"+buildRamLogsJson(limit,nullptr);else json+=",\"logsIncluded\":false";
    json+=",\"sdReadOk\":"+String(needResync?"false":"true")+"}";sendJson(request,200,json);
  });

  server.on("/api/system-logs",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String date=hasParam(request,"date")?getParamValue(request,"date"):String("");
    date.trim();
    if(!isValidDateStr(date)) date=currentDateOnlyText();
    if(!isValidDateStr(date)){sendJson(request,503,"{\"ok\":false,\"message\":\"Device date is not ready\"}");return;}
    SdBulkRequest req=makeBulkReq(nextBulkId(),7,date.c_str());
    String prefix="{\"date\":\""+jsonEscape(date)+"\",\"logs\":[";
    if(beginSdStreamWithReq(request,req,prefix,"]}","application/json")!=SD_STREAM_STARTED)
      sendJson(request,503,"{\"ok\":false,\"message\":\"SD task busy, try again\"}");
  });

  server.on("/api/registered",HTTP_GET,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}String reg="[]";if(lockState()){reg=cachedRegisteredJson;unlockState();}sendJson(request,200,"{\"fingerprints\":"+reg+"}");});

  server.on("/api/enroll",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    bool busy=false,full=false;uint16_t nid=1;if(lockState()){busy=isEnrolling||dashboardEnrollRequest;nid=nextEnrollID;full=(nid==0);unlockState();}
    if(busy){sendJson(request,409,"{\"ok\":false,\"message\":\"Enrollment already running\"}");return;}
    if(full){sendJson(request,409,"{\"ok\":false,\"message\":\"Fingerprint database is full (200/200). Delete a record first.\"}");return;}
    String name=getParamValue(request,"name",true),college=getParamValue(request,"collegeId",true),role=getParamValue(request,"role",true),dept=getParamValue(request,"department",true),note=getParamValue(request,"note",true);
    name.trim();college.trim();role.trim();dept.trim();note.trim();
    // Match the fixed-width SD queue fields exactly so enrollment data can
    // never be silently truncated when a later reason record is queued.
    if(name.length()>63){sendJson(request,400,"{\"ok\":false,\"message\":\"Name is too long (max 63 characters)\"}");return;}
    if(college.length()>23){sendJson(request,400,"{\"ok\":false,\"message\":\"College ID is too long (max 23 characters)\"}");return;}
    if(dept.length()>39){sendJson(request,400,"{\"ok\":false,\"message\":\"Department is too long (max 39 characters)\"}");return;}
    if(note.length()>250){sendJson(request,400,"{\"ok\":false,\"message\":\"Note is too long (max 250 characters)\"}");return;}
    if(role!="Administrator"&&role!="Staff"){sendJson(request,400,"{\"ok\":false,\"message\":\"Role must be Administrator or Staff\"}");return;}
    if(lockState()){pendingName=name;pendingCollegeId=college;pendingRole=role;pendingDepartment=dept;pendingNote=note;dashboardEnrollRequest=true;unlockState();}
    String msg="Enroll requested from dashboard";if(name.length()>0)msg+=" for "+name;addLog("ENROLL REQUEST",-1,msg);
    sendJson(request,200,"{\"ok\":true,\"message\":\"Enrollment requested\"}");
  });

  server.on("/api/cancel",HTTP_POST,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}if(lockState()){cancelEnroll=true;dashboardEnrollRequest=false;unlockState();}addLog("CANCEL",-1,"Cancel requested from dashboard");sendJson(request,200,"{\"ok\":true,\"message\":\"Cancel requested\"}");});

  server.on("/api/fingerprint",HTTP_DELETE,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    if(!hasParam(request,"id")){sendJson(request,400,"{\"ok\":false,\"message\":\"Missing ID\"}");return;}
    int id=getParamValue(request,"id").toInt();if(id<1||id>MAX_FINGERPRINTS){sendJson(request,400,"{\"ok\":false,\"message\":\"Invalid ID\"}");return;}
    bool enrolling=false;if(lockState()){enrolling=isEnrolling;unlockState();}if(enrolling){sendJson(request,409,"{\"ok\":false,\"message\":\"Enrollment running. Try again later.\"}");return;}
    if(!lockFinger(50)){sendJson(request,503,"{\"ok\":false,\"message\":\"Sensor busy, try again.\"}");return;}
    uint8_t p=finger.deleteModel(id);uint8_t verify=(p==FINGERPRINT_OK)?finger.loadModel(id):0xFF;unlockFinger();
    if(p==FINGERPRINT_OK&&verify!=FINGERPRINT_OK){deleteProfile(id);if(lockState()){insideState[id]=false;unlockState();}clearPresenceState(id);addLog("DELETE",id,"Fingerprint deleted from sensor memory");refreshTemplateCountCache();removeRegisteredEntry(id);if(lockState()){if(scanFirstFreeId==0||id<scanFirstFreeId)scanFirstFreeId=id;nextEnrollID=scanFirstFreeId;unlockState();}sendJson(request,200,"{\"ok\":true,\"message\":\"Fingerprint deleted from sensor memory\"}");return;}
    if(p==FINGERPRINT_OK&&verify==FINGERPRINT_OK){addLog("ERROR",id,"Delete command sent but fingerprint still exists");sendJson(request,500,"{\"ok\":false,\"message\":\"Delete command sent but fingerprint still exists in sensor\"}");return;}
    addLog("ERROR",id,"Delete failed");sendJson(request,500,"{\"ok\":false,\"message\":\"Delete failed. Fingerprint may not exist or sensor rejected command\"}");
  });

  server.on("/api/export",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String date=hasParam(request,"date")?getParamValue(request,"date"):String("");date.trim();if(!isValidDateStr(date))date="";
    SdBulkRequest req=makeBulkReq(nextBulkId(),1,date.c_str());
    SdStreamStartResult start=beginSdStreamWithReq(request,req,"","","text/csv","attachment; filename=attendance_logs.csv");
    if(start==SD_STREAM_STARTED) return;
    if(start==SD_STREAM_NO_SLOT){ sendJson(request,503,"{\"ok\":false,\"message\":\"SD stream busy, try again\"}"); return; }
    // Preserve the original RAM fallback when the SD request queue is saturated.
    String csv=String(ATTENDANCE_CSV_HEADER)+"\n";
    if(lockState()){
      for(int i=0;i<logCount;i++){
        int idx=(logWriteIndex-logCount+i+MAX_LOGS)%MAX_LOGS;AttendanceLog &l=logs[idx];
        if(date.length()==10&&!rowMatchesDate(l.timeText,date.c_str()))continue;
        String rowDate,rowTime;if(!splitDateTimeText(l.timeText,rowDate,rowTime))continue;String eventType=attendanceEventTypeOf(l.event);if(eventType.length()==0)continue;String status=attendanceStatusEnumOf(l.event);
        csv+=buildAttendanceCsvRow(rowDate,rowTime,l.id,l.name,l.collegeId,l.role,l.department,eventType,status);
      }
      unlockState();
    }
    AsyncWebServerResponse* response=request->beginResponse(200,"text/csv",csv);
    if(!response){request->send(500,"text/plain","Export response allocation failed");return;}
    response->addHeader("Content-Disposition","attachment; filename=attendance_logs.csv");
    request->send(response);
  });

  server.on("/api/device-setup",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String checkin=getParamValue(request,"checkinTime",true),checkout=getParamValue(request,"checkoutTime",true),device=getParamValue(request,"deviceName",true),tz=getParamValue(request,"timezone",true),secStr=getParamValue(request,"securityLevel",true);checkin.trim();checkout.trim();device.trim();tz.trim();secStr.trim();
    int checkinMin=hhmmToMinutes(checkin),checkoutMin=hhmmToMinutes(checkout);if(checkinMin<0||checkoutMin<0){sendJson(request,400,"{\"ok\":false,\"message\":\"Check-in/out time must be in HH:MM (24h) format\"}");return;}
    if(device.length()<1||device.length()>40){sendJson(request,400,"{\"ok\":false,\"message\":\"Device name must be 1-40 characters\"}");return;}if(tz.length()<1||tz.length()>40){sendJson(request,400,"{\"ok\":false,\"message\":\"Timezone string must be 1-40 characters\"}");return;}
    int sec=secStr.toInt();if(sec<1||sec>5){sendJson(request,400,"{\"ok\":false,\"message\":\"Fingerprint security level must be 1-5\"}");return;}
    if(lockState()){
      cfgCheckinMinutes=checkinMin;
      cfgCheckoutMinutes=checkoutMin;
      cfgDeviceName=device;
      cfgTimezone=tz;
      cfgSecurityLevel=sec;
      deviceSetupPersistPending=true;
      fingerprintSecurityDirty = true;
      unlockState();
    } else {
      sendJson(request,503,"{\"ok\":false,\"message\":\"Device state is busy; try again\"}");
      return;
    }
    sendJson(request,202,"{\"ok\":true,\"queued\":true,\"message\":\"Setup update queued\"}");
  });

  server.on("/api/storage",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}if(!sdStatValid){sendJson(request,200,"{\"ok\":false,\"mounted\":false}");return;}
    uint64_t total=sdTotalBytesCache,used=sdUsedBytesCache,freeB=(total>used)?total-used:0,csvSize=sdCsvBytesCache,lessonSize=sdLessonsBytesCache;int percent=total?(int)((used*100ULL)/total):0;String health=percent>=90?"Critical":percent>=80?"Warning":"Healthy";int pending=0;if(lockState()){pending=pendingUsed;unlockState();}
    String json="{\"ok\":true,\"mounted\":true,\"total\":\""+formatBytes(total)+"\",\"used\":\""+formatBytes(used)+"\",\"free\":\""+formatBytes(freeB)+"\",\"csvSize\":\""+formatBytes(csvSize)+"\",\"lessonsSize\":\""+formatBytes(lessonSize)+"\",\"percent\":"+String(percent)+",\"logs\":"+String(sdRecordCount)+",\"lessons\":"+String(sdLessonCount)+",\"pending\":"+String(pending)+",\"health\":\""+health+"\"}";sendJson(request,200,json);
  });

  server.on("/api/sd/status",HTTP_GET,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    bool mounted=sdAvailable;uint64_t total=sdTotalBytesCache,used=sdUsedBytesCache,csv=sdCsvBytesCache,lesson=sdLessonsBytesCache,freeB=total>used?total-used:0;String json="{\"ok\":true,\"mounted\":"+String(mounted?"true":"false")+",\"totalBytes\":"+String((unsigned long long)total)+",\"usedBytes\":"+String((unsigned long long)used)+",\"freeBytes\":"+String((unsigned long long)freeB)+",\"csvBytes\":"+String((unsigned long long)csv)+",\"lessonsBytes\":"+String((unsigned long long)lesson)+",\"recordCount\":"+String(sdRecordCount)+",\"lessonCount\":"+String(sdLessonCount)+"}";sendJson(request,200,json);
  });

  server.on("/api/sd/delete-attendance",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    SdBulkRequest req=makeBulkReq(nextBulkId(),3,"");
    if(beginSdStreamWithReq(request,req,"","","application/json")!=SD_STREAM_STARTED) sendJson(request,503,"{\"success\":false,\"message\":\"SD task busy, try again\"}");
  });

  server.on("/api/system/restart",HTTP_POST,[](AsyncWebServerRequest* request){if(!isAuthenticated(request)){requireAuthentication(request);return;}addLog("WARNING",-1,"Device restart requested from dashboard");requestRestart(false);sendJson(request,200,"{\"ok\":true,\"message\":\"Restarting...\"}");});

  server.on("/api/system/factory-reset",HTTP_POST,[](AsyncWebServerRequest* request){
    if(!isAuthenticated(request)){requireAuthentication(request);return;}
    String confirm=getParamValue(request,"confirm",true),password=getParamValue(request,"password",true);
    confirm.trim();
    if(confirm!="FACTORY RESET"){sendJson(request,400,"{\"ok\":false,\"message\":\"Type FACTORY RESET exactly to confirm\"}");return;}
    String expectedRecord;if(lockState()){expectedRecord=authPasswordHash;unlockState();}
    if(!verifyPasswordRecord(password,expectedRecord,nullptr)){sendJson(request,401,"{\"ok\":false,\"message\":\"Current password is incorrect\"}");return;}

    bool enrolling=false,journal=false;
    if(lockState(100)){enrolling=isEnrolling;journal=attendanceJournalActive;unlockState();}
    bool queuedAttendance=(attendanceQueue && uxQueueMessagesWaiting(attendanceQueue)>0);
    if(enrolling || journal || queuedAttendance){
      sendJson(request,409,"{\"ok\":false,\"message\":\"Factory reset is temporarily blocked while an attendance transaction is active.\"}");
      return;
    }

    if(!fingerprintSensorOk){
      sendJson(request,503,"{\"ok\":false,\"message\":\"Fingerprint sensor unavailable; reset aborted.\"}");
      return;
    }
    if(!lockFinger(100)){
      sendJson(request,503,"{\"ok\":false,\"message\":\"Fingerprint sensor is busy; try again.\"}");
      return;
    }
    uint8_t eraseResult=finger.emptyDatabase();
    unlockFinger();
    if(eraseResult!=FINGERPRINT_OK){
      sendJson(request,500,"{\"ok\":false,\"message\":\"Fingerprint database erase failed; reset aborted.\"}");
      return;
    }

    if(!prefsClearAll()){
      sendJson(request,500,"{\"ok\":false,\"message\":\"Settings reset failed; reset aborted.\"}");
      return;
    }
    addLog("WARNING",-1,"Factory reset requested from dashboard.");
    requestRestart(true);
    sendJson(request,200,"{\"ok\":true,\"message\":\"Factory reset complete. Restarting...\"}");
  });
}