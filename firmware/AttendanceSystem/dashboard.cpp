#include "app.h"
#include "dashboard.h"
#include "reports.h"
#include "logo_data.h"
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <errno.h>
#include <esp_heap_caps.h>

bool accountExists() {
  String user, record;
  if (!lockState(100)) return false;
  user = authUsername; record = authPasswordHash;
  unlockState();
  return user.length() > 0 && record.length() > 0;
}

void loadAuthAccount() {
  String user = prefsGetString(AUTH_USERNAME_KEY, "");
  String record = prefsGetString(AUTH_PASSWORD_KEY, "");
  if (lockState()) {
    authUsername = user;
    authPasswordHash = record;
    unlockState();
  }
  clearAllAuthSessions();
  Serial.print("Dashboard account: ");
  if (accountExists()) { Serial.print("configured as "); Serial.println(user); }
  else Serial.println("not configured");
}

String sha256Hex(const String &input) {
  unsigned char hash[32];
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return ""; }
  if (mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length()) != 0) { mbedtls_sha256_free(&ctx); return ""; }
  if (mbedtls_sha256_finish(&ctx, hash) != 0) { mbedtls_sha256_free(&ctx); return ""; }
  mbedtls_sha256_free(&ctx);
  char output[65];
  for (int i=0;i<32;i++) snprintf(&output[i*2],3,"%02x",hash[i]);
  output[64]='\0';
  return String(output);
}

static String randomHexSalt(size_t bytes) {
  String salt; salt.reserve(bytes*2);
  while (salt.length() < bytes*2) {
    uint32_t r = esp_random();
    char buf[9]; snprintf(buf,sizeof(buf),"%08lx",(unsigned long)r);
    salt += buf;
  }
  salt.remove(bytes*2);
  return salt;
}

static String saltedPasswordDigest(const String &password, const String &saltHex) {
  // Versioned, random 128-bit salt. We deliberately keep the construction
  // simple and deterministic on the ESP32 so it does not introduce a new
  // crypto library or destabilize the proven AsyncTCP/WebServer stack.
  return sha256Hex(String("v1:") + saltHex + ":" + password);
}

String createPasswordRecord(const String& password) {
  const String salt = randomHexSalt(16); // 128-bit salt
  const String digest = saltedPasswordDigest(password, salt);
  if (digest.length() != 64) return "";
  return String("v1$") + salt + "$" + digest;
}

static bool constantTimeEquals(const String &a, const String &b) {
  const size_t alen=a.length(), blen=b.length();
  const size_t n = alen > blen ? alen : blen;
  unsigned diff = (unsigned)(alen ^ blen);
  for(size_t i=0;i<n;i++) {
    const unsigned char ca = (i<alen) ? (unsigned char)a[i] : 0;
    const unsigned char cb = (i<blen) ? (unsigned char)b[i] : 0;
    diff |= (unsigned)(ca ^ cb);
  }
  return diff == 0;
}

bool verifyPasswordRecord(const String& password, const String& storedRecord, bool *legacyOut) {
  if (legacyOut) *legacyOut = false;
  if (storedRecord.startsWith("v1$")) {
    // Format is exactly: v1$<32-hex-char-salt>$<64-hex-char-digest>
    // There are TWO '$' delimiters. The previous verifier incorrectly
    // searched for a third '$', making every newly-created salted account
    // fail login even when the username/password were correct.
    const int p1=storedRecord.indexOf('$',3);
    if (p1<0) return false;
    const String salt=storedRecord.substring(3,p1);
    const String expected=storedRecord.substring(p1+1);
    if (salt.length()!=32 || expected.length()!=64) return false;
    const String actual=saltedPasswordDigest(password,salt);
    return constantTimeEquals(actual,expected);
  }
  // Backward-compatible one-time migration from the old unsalted SHA-256
  // record. A successful login immediately upgrades it to the salted v1 form.
  if (storedRecord.length()==64) {
    if (legacyOut) *legacyOut = true;
    return constantTimeEquals(sha256Hex(password),storedRecord);
  }
  return false;
}

bool migrateLegacyPasswordRecordIfValid(const String& password, const String& storedRecord) {
  if (storedRecord.startsWith("v1$")) return true;
  bool legacy=false;
  if (!verifyPasswordRecord(password,storedRecord,&legacy) || !legacy) return false;
  const String upgraded=createPasswordRecord(password);
  if (upgraded.length()<1 || !prefsPutString(AUTH_PASSWORD_KEY,upgraded)) return false;
  if (lockState(100)) { authPasswordHash=upgraded; unlockState(); }
  return true;
}

String generateSessionToken() {
  // 256-bit session token: 8 x 32-bit ESP hardware RNG words = 64 hex chars.
  // allocateAuthSession()/isAuthenticated() deliberately require exactly 64 chars.
  char token[65];
  uint32_t r[8];
  for(uint8_t i=0;i<8;i++) r[i]=esp_random();
  snprintf(token,sizeof(token),
           "%08lx%08lx%08lx%08lx%08lx%08lx%08lx%08lx",
           (unsigned long)r[0],(unsigned long)r[1],(unsigned long)r[2],(unsigned long)r[3],
           (unsigned long)r[4],(unsigned long)r[5],(unsigned long)r[6],(unsigned long)r[7]);
  return String(token);
}

String cookieValue(const String &cookieHeader,const String &name) {
  int pos=0;
  while(pos<(int)cookieHeader.length()){
    while(pos<(int)cookieHeader.length() &&
          (cookieHeader[pos]==' ' || cookieHeader[pos]==';')) pos++;
    int eq=cookieHeader.indexOf('=',pos);
    if(eq<0) break;
    int end=cookieHeader.indexOf(';',eq+1);
    if(end<0) end=cookieHeader.length();
    String key=cookieHeader.substring(pos,eq); key.trim();
    if(key==name){
      String value=cookieHeader.substring(eq+1,end); value.trim(); return value;
    }
    pos=end+1;
  }
  return "";
}

static bool sessionExpired(const AuthSession &s, uint32_t nowMs) {
  return !s.active || (uint32_t)(nowMs - s.lastSeenMs) > AUTH_IDLE_TIMEOUT_MS;
}

// Authentication sessions have their own mutex. Async HTTP callbacks should
// never be blocked by SD, RTC, fingerprint, or other shared application state.
static bool lockAuthSessions(unsigned long ms=200) {
  return authSessionMutex && xSemaphoreTake(authSessionMutex,pdMS_TO_TICKS(ms))==pdTRUE;
}
static void unlockAuthSessions() {
  if(authSessionMutex) xSemaphoreGive(authSessionMutex);
}

int allocateAuthSession(const String& token) {
  if (token.length()!=64) {
    Serial.printf("[AUTH] Invalid session token length=%u\n", (unsigned)token.length());
    return -1;
  }
  // Async HTTP callbacks can briefly overlap with auth-status/logout activity.
  // Give the dedicated auth mutex a bounded retry window instead of turning a
  // short contention period into the user-visible "service busy" error.
  bool locked=false;
  for(uint8_t attempt=0; attempt<5 && !locked; ++attempt) {
    locked=lockAuthSessions(200);
    if(!locked) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if(!locked) {
    Serial.printf("[AUTH] Session mutex unavailable after retries handle=%p\n", (void*)authSessionMutex);
    return -1;
  }
  const uint32_t nowMs=millis();
  int freeSlot=-1, oldestSlot=0;
  uint32_t oldestAge=0;
  int activeCount=0;
  for(int i=0;i<AUTH_MAX_SESSIONS;i++) {
    if(sessionExpired(authSessions[i],nowMs)) { freeSlot=i; break; }
    activeCount++;
    const uint32_t age=(uint32_t)(nowMs-authSessions[i].lastSeenMs);
    if(age>=oldestAge){oldestAge=age;oldestSlot=i;}
  }
  const int slot=(freeSlot>=0)?freeSlot:oldestSlot;
  strlcpy(authSessions[slot].token,token.c_str(),sizeof(authSessions[slot].token));
  authSessions[slot].issuedAtMs=nowMs;
  authSessions[slot].lastSeenMs=nowMs;
  authSessions[slot].active=true;
  activeSessionToken=token;
  unlockAuthSessions();
  Serial.printf("[AUTH] Session allocated slot=%d activeBefore=%d\n",slot,activeCount);
  return slot;
}

bool removeAuthSessionForToken(const String& token) {
  if(token.length()==0 || !lockAuthSessions(200)) return false;
  bool removed=false;
  for(int i=0;i<AUTH_MAX_SESSIONS;i++) {
    if(authSessions[i].active && constantTimeEquals(String(authSessions[i].token),token)) {
      memset(&authSessions[i],0,sizeof(authSessions[i])); removed=true; break;
    }
  }
  unlockAuthSessions();
  return removed;
}

void clearAllAuthSessions() {
  if(lockAuthSessions(200)) {
    memset(authSessions,0,sizeof(authSessions));
    activeSessionToken="";
    unlockAuthSessions();
  }
}

bool isAuthenticated(AsyncWebServerRequest* request) {
  if(!request || !accountExists() || !request->hasHeader("Cookie")) return false;
  const String token=cookieValue(request->header("Cookie"),AUTH_SESSION_COOKIE);
  if(token.length()!=64 || !lockAuthSessions(200)) return false;
  const uint32_t nowMs=millis();
  bool ok=false;
  for(int i=0;i<AUTH_MAX_SESSIONS;i++) {
    if(sessionExpired(authSessions[i],nowMs)) {
      if(authSessions[i].active) memset(&authSessions[i],0,sizeof(authSessions[i]));
      continue;
    }
    if(constantTimeEquals(String(authSessions[i].token),token)) {
      authSessions[i].lastSeenMs=nowMs;
      ok=true;
      break;
    }
  }
  unlockAuthSessions();
  return ok;
}

void setLoginCookie(AsyncWebServerResponse* response, const String &token) {
  if(!response) return;
  response->addHeader("Set-Cookie",String(AUTH_SESSION_COOKIE)+"="+token+"; Path=/; HttpOnly; SameSite=Strict");
}

void clearLoginCookieForRequest(AsyncWebServerRequest* request, AsyncWebServerResponse* response) {
  if(request && request->hasHeader("Cookie")) {
    const String token=cookieValue(request->header("Cookie"),AUTH_SESSION_COOKIE);
    removeAuthSessionForToken(token);
  }
  if(response) response->addHeader("Set-Cookie",String(AUTH_SESSION_COOKIE)+"=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

void clearLoginCookie(AsyncWebServerResponse* response) {
  if(response) response->addHeader("Set-Cookie",String(AUTH_SESSION_COOKIE)+"=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

void requireAuthentication(AsyncWebServerRequest* request) {
  if(request) request->send(401,"application/json","{\"ok\":false,\"message\":\"Authentication required\"}");
}


// -----------------------------------------------------------------------------
// Stable dashboard/page delivery
// -----------------------------------------------------------------------------
// IMPORTANT: ESPAsyncWebServer 3.6.0 exposes a RAM-oriented beginResponse()
// overload and a flash-aware AsyncProgmemResponse. PROGMEM pointers must never
// be passed through the ordinary RAM response path.
//
// V2.9.44 introduced /__asset and then passed PROGMEM addresses to ordinary
// beginResponse(). The browser successfully rendered that shell, then failed on
// the first /__asset fetch. This release removes that transport layer entirely.
// Small setup/login/logout pages are copied once to a String; the large dashboard
// and JPEG assets stay in flash and use AsyncProgmemResponse with an exact length.
// -----------------------------------------------------------------------------

static AsyncWebServerResponse* beginProgmemResponse(int code,
                                                    const char* contentType,
                                                    const uint8_t* data,
                                                    size_t length) {
  if (!data || length == 0) return nullptr;
  return new AsyncProgmemResponse(code, contentType, data, length);
}

static void sendNativeProgmemPage(AsyncWebServerRequest* request,
                                  const char* label,
                                  const uint8_t* page,
                                  size_t length,
                                  const char* contentType = "text/html; charset=UTF-8") {
  if (!request || !page || length == 0) return;
  httpd_req_t* req = request->nativeRequest();
  httpd_resp_set_type(req, contentType);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const uint32_t t0 = millis();
  esp_err_t err = httpd_resp_send(req, (const char*)page, (ssize_t)length);
  const int savedErrno = errno;
  request->reportSend(label, err, savedErrno, t0, length, true);
}

static void sendSmallProgmemPage(AsyncWebServerRequest* request,
                                 const char* label,
                                 const char* page,
                                 size_t length) {
  sendNativeProgmemPage(request, label, (const uint8_t*)page, length);
}

void registerDashboardRoutes() {
  server.on("/logo.jpg", HTTP_GET, [](AsyncWebServerRequest* request){
    AsyncWebServerResponse* response = beginProgmemResponse(
        200, "image/jpeg", LOGO_BADGE_JPG, LOGO_BADGE_JPG_LEN);
    if (!response) {
      request->send(500, "text/plain", "Logo unavailable");
      return;
    }
    response->addHeader("Cache-Control", "public, max-age=86400");
    request->send(response);
  });

  server.on("/logo-bg.jpg", HTTP_GET, [](AsyncWebServerRequest* request){
    AsyncWebServerResponse* response = beginProgmemResponse(
        200, "image/jpeg", LOGO_BG_JPG, LOGO_BG_JPG_LEN);
    if (!response) {
      request->send(500, "text/plain", "Background logo unavailable");
      return;
    }
    response->addHeader("Cache-Control", "public, max-age=86400");
    request->send(response);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request){
    Serial.printf("[HTTP] GET / heap=%u\n", (unsigned)ESP.getFreeHeap());

    if (!accountExists()) {
      sendCreateAccountPage(request);
      return;
    }

    if (!isAuthenticated(request)) {
      sendLoginPage(request);
      return;
    }

    // The AsyncProgmemResponse formerly allocated here was never transmitted (the native
    // path below bypasses it) and is removed. Exactly one httpd_resp_send() is issued.
    httpd_req_t* req = request->nativeRequest();
    Serial.printf("[HTTP] dashboard auth-ok heap=%u largest=%u\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    const uint32_t t0 = millis();
    esp_err_t err = httpd_resp_send(req, (const char*)DASHBOARD_GZIP, (ssize_t)DASHBOARD_GZIP_LEN);
    const int savedErrno = errno;
    request->reportSend("dashboard-gzip", err, savedErrno, t0, DASHBOARD_GZIP_LEN, true);
  });

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request){
    if (accountExists() && !isAuthenticated(request)) {
      requireAuthentication(request);
      return;
    }
    String body = String("OK\nip=") + WiFi.localIP().toString() +
                  "\nwifi=" + String(wifiDashboardReady() ? "connected" : "disconnected") +
                  "\nrssi=" + String(wifiDashboardReady() ? (int)WiFi.RSSI() : 0) +
                  "\nfreeHeap=" + String(ESP.getFreeHeap()) +
                  "\nsd=" + String(sdAvailable ? "mounted" : "not-mounted");
    request->send(200, "text/plain", body);
  });

  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* request){
    sendLogoutPage(request);
  });
}

void setupDashboardRoutes() {
  refreshBootSnapshot();
  registerDashboardRoutes();
  registerApiRoutes();
  server.begin();
  Serial.println("Dashboard async server started.");
}

static const char CREATE_ACCOUNT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MABOSYTECH — Create Account</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;font-family:Arial,Helvetica,sans-serif;color:#e6edf5}
body{
  min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;
  background:
    radial-gradient(circle at 20% 15%, rgba(37,99,235,.35), transparent 55%),
    radial-gradient(circle at 80% 85%, rgba(34,197,94,.30), transparent 55%),
    linear-gradient(135deg, rgba(7,13,24,.82) 0%, rgba(11,26,46,.82) 45%, rgba(10,37,64,.85) 100%),
    url('/logo-bg.jpg');
  background-size:cover,cover,cover,cover;
  background-position:center,center,center,center;
  background-repeat:no-repeat,no-repeat,no-repeat,no-repeat;
  position:relative;overflow:hidden;
}
body::before{
  content:"";position:absolute;inset:0;
  background-image:
    linear-gradient(rgba(59,130,246,.10) 1px, transparent 1px),
    linear-gradient(90deg, rgba(59,130,246,.10) 1px, transparent 1px);
  background-size:60px 60px, 60px 60px;
  mask-image: radial-gradient(circle at 50% 50%, black 30%, transparent 85%);
  -webkit-mask-image: radial-gradient(circle at 50% 50%, black 30%, transparent 85%);
  pointer-events:none;
}
body::after{
  content:"MABOSYTECH";position:absolute;bottom:24px;left:50%;transform:translateX(-50%);
  font-size:52px;font-weight:900;letter-spacing:14px;
  color:rgba(96,165,250,.10);text-shadow:0 0 40px rgba(34,197,94,.18);
  pointer-events:none;user-select:none;white-space:nowrap;
}
.card{
  position:relative;z-index:2;
  width:100%;max-width:440px;
  background:rgba(15,23,42,.72);
  backdrop-filter:blur(18px);
  -webkit-backdrop-filter:blur(18px);
  border:1px solid rgba(96,165,250,.28);
  border-radius:22px;
  padding:34px 30px;
  box-shadow:0 30px 80px rgba(0,0,0,.55), inset 0 0 0 1px rgba(255,255,255,.04);
}
.brand{display:flex;align-items:center;gap:12px;margin-bottom:18px}
.logo{width:52px;height:52px;border-radius:14px;
  background:#fff;overflow:hidden;
  display:grid;place-items:center;
  box-shadow:0 12px 28px rgba(37,99,235,.45);
}
.logo img{width:100%;height:100%;object-fit:contain;padding:3px}
.brand h1{margin:0;font-size:22px;letter-spacing:1px}
.brand p{margin:4px 0 0;font-size:12px;color:#94a3b8;letter-spacing:2px;text-transform:uppercase}
h2{margin:0 0 8px;color:#fff}
p.sub{color:#cbd5e1;line-height:1.5;font-size:13px;margin:0 0 20px}
label{display:block;margin:16px 0 7px;font-weight:600;color:#cbd5e1;font-size:13px}
input{width:100%;padding:13px 14px;border:1px solid rgba(148,163,184,.30);
  background:rgba(2,6,23,.55);color:#fff;border-radius:12px;font-size:15px;outline:none}
input:focus{border-color:#2563eb;box-shadow:0 0 0 4px rgba(37,99,235,.20)}
button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:12px;
  background:linear-gradient(135deg,#2563eb,#22c55e);color:#fff;font-size:16px;font-weight:800;
  cursor:pointer;letter-spacing:.5px;box-shadow:0 12px 30px rgba(34,197,94,.28)}
button:disabled{opacity:.6;cursor:not-allowed}
#status{margin-top:15px;padding:11px;border-radius:10px;display:none;line-height:1.4;font-size:13px}
</style></head><body>
<div class="card">
  <div class="brand">
    <div class="logo"><img src="/logo.jpg" alt="E.S. GAHUNGA TSS logo"></div>
    <div><h1>MABOSYTECH</h1><p>Your Tech Partner for Tomorrow</p></div>
  </div>
  <h2>Create Account</h2>
  <p class="sub">This device supports one dashboard account only.</p>
  <form id="form">
    <label>Username</label>
    <input id="username" autocomplete="username" required minlength="3">
    <label>Password</label>
    <input id="password" type="password" autocomplete="new-password" required minlength="6">
    <label>Confirm Password</label>
    <input id="confirm" type="password" autocomplete="new-password" required minlength="6">
    <button id="btn" type="submit">Create Account</button>
  </form>
  <div id="status"></div>
</div>
<script>
const form=document.getElementById("form"),statusBox=document.getElementById("status"),btn=document.getElementById("btn");
function show(m,e){statusBox.style.display="block";statusBox.textContent=m;statusBox.style.background=e?"rgba(220,38,38,.18)":"rgba(34,197,94,.18)";statusBox.style.color=e?"#fecaca":"#bbf7d0"}
form.addEventListener("submit",async e=>{
  e.preventDefault();
  const u=document.getElementById("username").value.trim();
  const p=document.getElementById("password").value;
  const c=document.getElementById("confirm").value;
  if(p!==c){show("Passwords do not match.",true);return;}
  if(p.length<6){show("Password must be at least 6 characters.",true);return;}
  btn.disabled=true;
  try{
    const body=new URLSearchParams();body.set("username",u);body.set("password",p);
    const r=await fetch("/api/setup",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body});
    const d=await r.json();
    if(!r.ok){show(d.message||"Account creation failed.",true);btn.disabled=false;return;}
    show("Account created. Opening login...",false);
    setTimeout(()=>location.reload(),600);
  }catch(err){show("Connection error. Try again.",true);btn.disabled=false;}
});
</script>
</body></html>
)rawliteral";

void sendCreateAccountPage(AsyncWebServerRequest* request) {
  sendSmallProgmemPage(request, "Create Account", CREATE_ACCOUNT_PAGE,
                       sizeof(CREATE_ACCOUNT_PAGE) - 1);
}

static const char LOGIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MABOSYTECH — Login</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;font-family:Arial,Helvetica,sans-serif;color:#e6edf5}
body{
  min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;
  background:
    radial-gradient(circle at 18% 20%, rgba(37,99,235,.38), transparent 55%),
    radial-gradient(circle at 82% 82%, rgba(34,197,94,.32), transparent 55%),
    linear-gradient(135deg, rgba(6,11,20,.82) 0%, rgba(10,26,47,.82) 45%, rgba(8,32,54,.85) 100%),
    url('/logo-bg.jpg');
  background-size:cover,cover,cover,cover;
  background-position:center,center,center,center;
  background-repeat:no-repeat,no-repeat,no-repeat,no-repeat;
  position:relative;overflow:hidden;
}
body::before{
  content:"";position:absolute;inset:0;
  background-image:
    linear-gradient(rgba(59,130,246,.10) 1px, transparent 1px),
    linear-gradient(90deg, rgba(59,130,246,.10) 1px, transparent 1px);
  background-size:64px 64px, 64px 64px;
  mask-image: radial-gradient(circle at 50% 50%, black 25%, transparent 85%);
  -webkit-mask-image: radial-gradient(circle at 50% 50%, black 25%, transparent 85%);
  pointer-events:none;
}
body::after{
  content:"MABOSYTECH";position:absolute;bottom:22px;left:50%;transform:translateX(-50%);
  font-size:56px;font-weight:900;letter-spacing:16px;
  color:rgba(96,165,250,.10);text-shadow:0 0 48px rgba(34,197,94,.22);
  pointer-events:none;user-select:none;white-space:nowrap;
}
.card{
  position:relative;z-index:2;width:100%;max-width:440px;
  background:rgba(15,23,42,.74);
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
  border:1px solid rgba(96,165,250,.30);
  border-radius:24px;padding:36px 30px;
  box-shadow:0 30px 90px rgba(0,0,0,.60), inset 0 0 0 1px rgba(255,255,255,.05);
}
.brand{display:flex;align-items:center;gap:14px;margin-bottom:22px;justify-content:center;flex-direction:column}
.logo{width:74px;height:74px;border-radius:20px;
  background:#fff;overflow:hidden;
  display:grid;place-items:center;
  box-shadow:0 16px 36px rgba(37,99,235,.50);
}
.logo img{width:100%;height:100%;object-fit:contain;padding:4px}
.brand h1{margin:0;font-size:24px;letter-spacing:2px}
.brand p{margin:4px 0 0;font-size:11px;color:#94a3b8;letter-spacing:3px;text-transform:uppercase;text-align:center}
h2{margin:0 0 8px;color:#fff;text-align:center}
p.sub{color:#cbd5e1;line-height:1.5;text-align:center;font-size:13px;margin:0 0 20px}
label{display:block;margin:16px 0 7px;font-weight:600;color:#cbd5e1;font-size:13px}
input{width:100%;padding:13px 14px;border:1px solid rgba(148,163,184,.30);
  background:rgba(2,6,23,.55);color:#fff;border-radius:12px;font-size:15px;outline:none}
input:focus{border-color:#2563eb;box-shadow:0 0 0 4px rgba(37,99,235,.20)}
button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:12px;
  background:linear-gradient(135deg,#2563eb,#22c55e);color:#fff;font-size:16px;font-weight:800;
  cursor:pointer;letter-spacing:.5px;box-shadow:0 12px 30px rgba(34,197,94,.30)}
button:disabled{opacity:.6;cursor:not-allowed}
#status{margin-top:15px;padding:11px;border-radius:10px;display:none;line-height:1.4;font-size:13px}
</style></head><body>
<div class="card">
  <div class="brand">
    <div class="logo"><img src="/logo.jpg" alt="E.S. GAHUNGA TSS logo"></div>
    <div>
      <h1>MABOSYTECH</h1>
      <p>Your Tech Partner for Tomorrow</p>
    </div>
  </div>
  <h2>Login</h2>
  <p class="sub">Sign in to access the attendance dashboard.</p>
  <form id="form">
    <label>Username</label>
    <input id="username" autocomplete="username" required>
    <label>Password</label>
    <input id="password" type="password" autocomplete="current-password" required>
    <button id="btn" type="submit">Login</button>
  </form>
  <div id="status"></div>
</div>
<script>
const form=document.getElementById("form"),statusBox=document.getElementById("status"),btn=document.getElementById("btn");
function show(m,e){statusBox.style.display="block";statusBox.textContent=m;statusBox.style.background=e?"rgba(220,38,38,.18)":"rgba(34,197,94,.18)";statusBox.style.color=e?"#fecaca":"#bbf7d0"}
form.addEventListener("submit",async e=>{
  e.preventDefault();btn.disabled=true;
  try{
    const body=new URLSearchParams();
    body.set("username",document.getElementById("username").value.trim());
    body.set("password",document.getElementById("password").value);
    const r=await fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body});
    const d=await r.json();
    if(!r.ok){show(d.message||"Login failed.",true);btn.disabled=false;return;}
    show("Login successful...",false);
    location.href="/";
  }catch(err){show("Connection error. Try again.",true);btn.disabled=false;}
});
</script>
</body></html>
)rawliteral";

void sendLoginPage(AsyncWebServerRequest* request) {
  sendSmallProgmemPage(request, "Login", LOGIN_PAGE,
                       sizeof(LOGIN_PAGE) - 1);
}

static const char LOGOUT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MABOSYTECH — Logged Out</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;font-family:Arial,Helvetica,sans-serif;color:#e6edf5}
body{
  min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;
  background:
    radial-gradient(circle at 18% 20%, rgba(37,99,235,.38), transparent 55%),
    radial-gradient(circle at 82% 82%, rgba(34,197,94,.32), transparent 55%),
    linear-gradient(135deg, rgba(6,11,20,.82) 0%, rgba(10,26,47,.82) 45%, rgba(8,32,54,.85) 100%),
    url('/logo-bg.jpg');
  background-size:cover,cover,cover,cover;
  background-position:center,center,center,center;
  background-repeat:no-repeat,no-repeat,no-repeat,no-repeat;
  position:relative;overflow:hidden;
}
body::before{
  content:"";position:absolute;inset:0;
  background-image:
    linear-gradient(rgba(59,130,246,.10) 1px, transparent 1px),
    linear-gradient(90deg, rgba(59,130,246,.10) 1px, transparent 1px);
  background-size:64px 64px, 64px 64px;
  mask-image: radial-gradient(circle at 50% 50%, black 25%, transparent 85%);
  -webkit-mask-image: radial-gradient(circle at 50% 50%, black 25%, transparent 85%);
  pointer-events:none;
}
body::after{
  content:"MABOSYTECH";position:absolute;bottom:22px;left:50%;transform:translateX(-50%);
  font-size:56px;font-weight:900;letter-spacing:16px;
  color:rgba(96,165,250,.10);text-shadow:0 0 48px rgba(34,197,94,.22);
  pointer-events:none;user-select:none;white-space:nowrap;
}
.card{
  position:relative;z-index:2;width:100%;max-width:440px;text-align:center;
  background:rgba(15,23,42,.74);
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
  border:1px solid rgba(96,165,250,.30);
  border-radius:24px;padding:44px 30px;
  box-shadow:0 30px 90px rgba(0,0,0,.60), inset 0 0 0 1px rgba(255,255,255,.05);
}
.logo{width:84px;height:84px;border-radius:24px;margin:0 auto 18px;
  background:#fff;overflow:hidden;
  display:grid;place-items:center;
  box-shadow:0 16px 40px rgba(37,99,235,.50);
}
.logo img{width:100%;height:100%;object-fit:contain;padding:4px}
h1{margin:0 0 10px;font-size:26px;letter-spacing:1px;color:#fff}
p{color:#cbd5e1;line-height:1.55;font-size:14px;margin:0 0 24px}
a.btn{
  display:inline-block;padding:14px 30px;border-radius:12px;
  background:linear-gradient(135deg,#2563eb,#22c55e);color:#fff;
  text-decoration:none;font-weight:800;letter-spacing:.5px;
  box-shadow:0 12px 30px rgba(34,197,94,.30);
}
</style></head><body>
<div class="card">
  <div class="logo"><img src="/logo.jpg" alt="E.S. GAHUNGA TSS logo"></div>
  <h1>Logged Out</h1>
  <p>You have been logged out of MABOSYTECH Smart Teacher Attendance.</p>
  <a class="btn" href="/">Login Again</a>
</div>

</body></html>
)rawliteral";

void sendLogoutPage(AsyncWebServerRequest* request) {
  String body;
  body.reserve(sizeof(LOGOUT_PAGE));
  body = FPSTR(LOGOUT_PAGE);

  AsyncWebServerResponse* response =
      request->beginResponse(200, "text/html; charset=UTF-8", body);
  if (!response) {
    request->send(500, "text/plain", "Logout page unavailable");
    return;
  }
  response->addHeader("Cache-Control", "no-store");
  clearLoginCookieForRequest(request, response);
  Serial.printf("[HTTP] Logout direct response bytes=%u heap=%u\n",
                (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
  request->send(response);
}

// =====================================================
// DASHBOARD ROUTES
// =====================================================